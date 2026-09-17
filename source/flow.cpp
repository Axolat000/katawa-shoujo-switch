#include "audio.h"
#include "game.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>

static const Loc kStopMarker{-2, -2};
double ListObj_at(const Value &v, size_t i);

// ------------------------------------------------------------------ value graph writer (KSG1)

struct GraphWriter {
    std::vector<std::string> strings;
    std::unordered_map<std::string, uint32_t> sindex;
    std::vector<ByteWriter> objects;
    std::unordered_map<Obj *, uint32_t> oindex;
    std::vector<std::pair<uint32_t, Value>> pending;
    std::function<Value(const Value &)> mapper; // converts non-serializable objects

    uint32_t sid(const std::string &s) {
        auto it = sindex.find(s);
        if (it != sindex.end())
            return it->second;
        uint32_t i = (uint32_t)strings.size();
        strings.push_back(s);
        sindex[s] = i;
        return i;
    }
    void val(const Value &v0, ByteWriter &w) {
        Value v = v0;
        if (v.t == T::Obj && (v.inst() || v.func()) && mapper)
            v = mapper(v);
        switch (v.t) {
        case T::None:
            w.u8(0);
            return;
        case T::Bool:
            w.u8(v.i ? 2 : 1);
            return;
        case T::Int:
            w.u8(3);
            w.svar(v.i);
            return;
        case T::Float:
            w.u8(4);
            w.f64(v.f);
            return;
        case T::Str:
            w.u8(5);
            w.uvar(sid(v.s()));
            return;
        case T::Code:
            w.u8(7);
            w.uvar((uint64_t)v.i);
            return;
        case T::Obj:
            break;
        }
        if (v.o->kind == OK::Tuple) {
            w.u8(8);
            w.uvar(v.list()->v.size());
            for (auto &x : v.list()->v)
                val(x, w);
            return;
        }
        auto it = oindex.find(v.o.get());
        uint32_t idx;
        if (it != oindex.end()) {
            idx = it->second;
        } else {
            idx = (uint32_t)objects.size();
            oindex[v.o.get()] = idx;
            objects.emplace_back();
            pending.emplace_back(idx, v);
        }
        w.u8(6);
        w.uvar(idx);
    }
    void encodeObj(uint32_t idx, const Value &v) {
        ByteWriter w;
        if (ListObj *l = v.list()) {
            w.u8(1);
            w.uvar(l->v.size());
            for (auto &x : l->v)
                val(x, w);
        } else if (DictObj *d = v.dict()) {
            if (v.o->kind == OK::Set) {
                // sets are stored as instances holding a list
                w.u8(3);
                w.uvar(sid("#set"));
                w.uvar(1);
                w.uvar(sid("items"));
                std::vector<Value> keys;
                for (auto &kv : d->items)
                    keys.push_back(kv.first);
                val(mkList(keys), w);
            } else {
                w.u8(2);
                w.uvar(d->items.size());
                for (auto &kv : d->items) {
                    val(kv.first, w);
                    val(kv.second, w);
                }
            }
        } else if (InstObj *in = v.inst()) {
            w.u8(3);
            w.uvar(sid(in->cls));
            w.uvar(in->attrs.size());
            for (auto &kv : in->attrs) {
                w.uvar(sid(kv.first));
                val(kv.second, w);
            }
        } else if (FuncObj *f = v.func()) {
            w.u8(4);
            w.uvar(sid(f->name));
        } else {
            w.u8(1);
            w.uvar(0);
        }
        objects[idx] = std::move(w);
    }
    std::vector<uint8_t> finish(const Value &root) {
        ByteWriter rw;
        val(root, rw);
        while (!pending.empty()) {
            auto p = pending.back();
            pending.pop_back();
            encodeObj(p.first, p.second);
        }
        ByteWriter out;
        out.b = {'K', 'S', 'G', '1'};
        out.uvar(strings.size());
        for (auto &s : strings)
            out.str(s);
        out.uvar(objects.size());
        for (auto &o : objects)
            out.b.insert(out.b.end(), o.b.begin(), o.b.end());
        out.b.insert(out.b.end(), rw.b.begin(), rw.b.end());
        return out.b;
    }
};

// Converts "#set" instances produced by the writer back into sets.
static Value fixSets(const Value &v, std::unordered_map<Obj *, Value> &done) {
    if (v.t != T::Obj)
        return v;
    auto it = done.find(v.o.get());
    if (it != done.end())
        return it->second;
    if (InstObj *in = v.inst()) {
        if (in->cls == "#set") {
            Value s = mkSet();
            done[v.o.get()] = s;
            if (ListObj *l = in->get("items").list())
                for (auto &x : l->v)
                    s.dict()->set(fixSets(x, done), Value());
            return s;
        }
        done[v.o.get()] = v;
        for (auto &kv : in->attrs)
            kv.second = fixSets(kv.second, done);
        return v;
    }
    done[v.o.get()] = v;
    if (ListObj *l = v.list()) {
        for (auto &x : l->v)
            x = fixSets(x, done);
    } else if (DictObj *d = v.dict()) {
        std::vector<std::pair<Value, Value>> items = d->items;
        d->clear();
        for (auto &kv : items)
            d->set(fixSets(kv.first, done), fixSets(kv.second, done));
    }
    return v;
}

// ------------------------------------------------------------------ snapshots

static bool isPlain(const Value &v, int depth = 0) {
    if (depth > 8)
        return false;
    switch (v.t) {
    case T::None:
    case T::Bool:
    case T::Int:
    case T::Float:
    case T::Str:
        return true;
    case T::Obj:
        if (ListObj *l = v.list()) {
            for (auto &x : l->v)
                if (!isPlain(x, depth + 1))
                    return false;
            return true;
        }
        if (DictObj *d = v.dict()) {
            for (auto &kv : d->items)
                if (!isPlain(kv.first, depth + 1) || !isPlain(kv.second, depth + 1))
                    return false;
            return true;
        }
        return false;
    default:
        return false;
    }
}

static Value deepCopy(const Value &v) {
    if (v.t != T::Obj)
        return v;
    if (ListObj *l = v.list()) {
        if (v.o->kind == OK::Tuple) {
            bool mutableInside = false;
            for (auto &x : l->v)
                if (x.t == T::Obj && x.o->kind != OK::Tuple)
                    mutableInside = true;
            if (!mutableInside)
                return v;
        }
        std::vector<Value> items;
        items.reserve(l->v.size());
        for (auto &x : l->v)
            items.push_back(deepCopy(x));
        return v.o->kind == OK::Tuple ? mkTuple(std::move(items)) : mkList(std::move(items));
    }
    if (DictObj *d = v.dict()) {
        Value r = v.o->kind == OK::Set ? mkSet() : mkDict();
        for (auto &kv : d->items)
            r.dict()->set(kv.first, deepCopy(kv.second));
        return r;
    }
    return v;
}

static const char *kSnapshotChannels[] = {"music", "ambient", "ambient2"};

Snapshot Game::capture() {
    Snapshot s;
    s.loc = current;
    s.returnStack = returnStack;
    s.dynamicStack = dynamicStack;
    for (auto &kv : store->attrs) {
        const Value &v = kv.second;
        if (v.t == T::Obj && (v.inst() || v.func()))
            continue;
        if (v.t == T::Code)
            continue;
        if (v.t == T::Obj && !isPlain(v))
            continue;
        s.store.emplace_back(kv.first, deepCopy(v));
    }
    s.scene = scene;
    s.scene.clear("transient");
    for (const char *ch : kSnapshotChannels) {
        s.music[ch] = audio::lastQueued(ch);
        s.channelVolumes[ch] = audio::channelVolume(ch);
    }
    s.playTime = playTime;
    s.saveName = valueStr(storeGet("save_name"));
    s.language = lang();
    return s;
}

void Game::restore(const Snapshot &s) {
    if (s.language != lang())
        switchLanguage(s.language);
    for (auto &kv : s.store)
        store->attrs[kv.first] = deepCopy(kv.second);
    returnStack = s.returnStack;
    dynamicStack = s.dynamicStack;
    scene = s.scene;
    next = s.loc;
    current = s.loc;
    for (const char *ch : kSnapshotChannels) {
        auto it = s.music.find(ch);
        std::vector<std::string> files = it == s.music.end() ? std::vector<std::string>() : it->second;
        std::string playing = audio::playing(ch);
        if (files.empty()) {
            audio::stop(ch, 0);
        } else if (std::find(files.begin(), files.end(), playing) == files.end()) {
            audio::play(ch, files, 0, 0, 1, false);
        }
        auto vit = s.channelVolumes.find(ch);
        if (vit != s.channelVolumes.end())
            audio::setChannelVolume(ch, vit->second, 0);
    }
    playTime = s.playTime;
    oldScene = scene.computeScene();
}

// ------------------------------------------------------------------ execution

static bool isInteractiveOp(uint8_t op) {
    switch (op) {
    case OP_SAY:
    case OP_MENU:
    case OP_PYTHON:
    case OP_WITH:
    case OP_PAUSE:
    case OP_WINDOW:
    case OP_NVL:
    case OP_CALL:
        return true;
    default:
        return false;
    }
}

static void runContext(Game &g) {
    while (true) {
        if (g.next == kStopMarker) {
            g.next = Loc();
            return;
        }
        const Op *op = g.opAt(g.next);
        if (!op) {
            logf("end of script at %d:%d", g.next.script, g.next.pc);
            throw FullRestartSignal();
        }
        g.current = g.next;
        g.next.pc++;
        if (isInteractiveOp(op->op) && !g.inMainMenuContext) {
            g.lastCheckpoint = g.capture();
            g.haveCheckpoint = true;
        }
        try {
            g.execOp(*op, g.current);
        } catch (JumpSignal &j) {
            if (!j.label.empty()) {
                try {
                    g.jumpTo(j.label);
                } catch (PyError &e) {
                    logf("jump failed: %s", e.msg.c_str());
                }
            }
        } catch (RollbackSignal &) {
            g.rollback();
        } catch (LoadSignal &l) {
            if (!g.loadSlot(l.slot))
                logf("load failed: %s", l.slot.c_str());
        } catch (PyError &e) {
            logf("python error at %s:%d (op %d, pc %d): %s: %s", op->file.c_str(), op->line, op->op, g.current.pc,
                 e.type.c_str(), e.msg.c_str());
        }
    }
}

void runLabelToEnd(const std::string &label) {
    Game &g = G;
    if (!g.hasLabel(label))
        return;
    g.returnStack.push_back(kStopMarker);
    g.dynamicStack.emplace_back();
    g.jumpTo(label);
    runContext(g);
}

void Game::fullRestart(const std::string &target) {
    audio::stopAll();
    scene = SceneLists();
    returnStack.clear();
    dynamicStack.clear();
    rollbackLog.clear();
    haveCheckpoint = false;
    oldScene = nullptr;
    pendingTransition = Value();
    configV.inst()->attrs["skipping"] = Value();
    storeSet("_window", Value::boolean(false));
    storeSet("nvl_list", mkList());
    inMainMenuContext = true;
    pendingNativeScreen = target;
    savePersistent();
}

void runStartup() {
    Game &g = G;
    bool splash = true;
    while (true) {
        try {
            if (splash) {
                splash = false;
                g.inMainMenuContext = true;
                runLabelToEnd("splashscreen");
            }
            g.inMainMenuContext = true;
            // label main_menu: menu_init() then the native main menu
            try {
                PY.call(g.storeGet("menu_init"));
            } catch (PyError &e) {
                logf("menu_init: %s %s", e.type.c_str(), e.msg.c_str());
            }
            g.mainMenu(); // returns once a game label must run
            runContext(g);
        } catch (FullRestartSignal &s) {
            g.fullRestart(s.target);
        } catch (QuitSignal &) {
            g.savePersistent();
            return;
        } catch (JumpSignal &j) {
            // jump out of a native screen into the script
            g.returnStack.clear();
            try {
                g.jumpTo(j.label);
                runContext(g);
            } catch (FullRestartSignal &s) {
                g.fullRestart(s.target);
            } catch (QuitSignal &) {
                g.savePersistent();
                return;
            }
        } catch (PyError &e) {
            logf("unhandled python error: %s %s", e.type.c_str(), e.msg.c_str());
            g.fullRestart("");
        }
    }
}

void Game::startGame() {
    inMainMenuContext = true;
    throw JumpSignal("start_from_mm");
}

// ------------------------------------------------------------------ saves & persistent

static std::unordered_map<Obj *, std::string> storeRefs() {
    std::unordered_map<Obj *, std::string> m;
    for (auto &kv : G.store->attrs)
        if (kv.second.t == T::Obj && (kv.second.inst() || kv.second.func()))
            m[kv.second.o.get()] = kv.first;
    return m;
}

static Value sceneToValue(const SceneLists &s, const std::unordered_map<Obj *, std::string> &refs) {
    Value layers = mkDict();
    for (auto &kv : s.layers) {
        if (kv.first == "transient" || kv.first == "overlay")
            continue;
        std::vector<Value> entries;
        for (auto &e : kv.second) {
            if (e.name.empty() || e.tag.find('$') != std::string::npos)
                continue;
            Value ev = mkDict();
            ev.dict()->set(Value::str("tag"), Value::str(e.tag));
            ev.dict()->set(Value::str("name"), Value::str(e.name));
            ev.dict()->set(Value::str("zorder"), Value::integer(e.zorder));
            std::vector<Value> at;
            for (auto &a : e.atList) {
                if (a.t == T::Obj) {
                    auto it = refs.find(a.o.get());
                    if (it != refs.end()) {
                        Value r = mkInst("#storeref");
                        r.inst()->attrs["name"] = Value::str(it->second);
                        at.push_back(r);
                        continue;
                    }
                    if (DispP d = valueDisp(a)) {
                        if (d->isTransform()) {
                            auto t = std::static_pointer_cast<TransformDisp>(d);
                            Value r = mkInst("#transform");
                            Value kwv = mkDict();
                            for (auto &k : t->kwargs)
                                if (isPlain(k.second))
                                    kwv.dict()->set(Value::str(k.first), k.second);
                            r.inst()->attrs["kwargs"] = kwv;
                            at.push_back(r);
                            continue;
                        }
                    }
                    continue;
                }
                at.push_back(a);
            }
            ev.dict()->set(Value::str("at"), mkList(at));
            entries.push_back(ev);
        }
        layers.dict()->set(Value::str(kv.first), mkList(entries));
    }
    return layers;
}

static void sceneFromValue(Game &g, const Value &layers) {
    g.scene = SceneLists();
    DictObj *d = layers.dict();
    if (!d)
        return;
    for (auto &kv : d->items) {
        ListObj *entries = kv.second.list();
        if (!entries)
            continue;
        for (auto &ev : entries->v) {
            DictObj *e = ev.dict();
            if (!e)
                continue;
            auto get = [&](const char *k) {
                const Value *v = e->find(Value::str(k));
                return v ? *v : Value();
            };
            std::vector<Value> at;
            if (ListObj *al = get("at").list())
                for (auto &a : al->v) {
                    if (InstObj *in = a.inst()) {
                        if (in->cls == "#storeref") {
                            at.push_back(g.storeGet(in->get("name").s()));
                        } else if (in->cls == "#transform") {
                            CallArgs ca;
                            if (DictObj *kd = in->get("kwargs").dict())
                                for (auto &k : kd->items)
                                    ca.kw.emplace_back(k.first.s(), k.second);
                            at.push_back(PY.call(PY.natives["renpy.display.motion.Transform"], ca));
                        }
                    } else {
                        at.push_back(a);
                    }
                }
            std::string name = get("name").s(), tag = get("tag").s();
            std::string key = name.substr(0, name.find(' '));
            g.showName(name, at, kv.first.s(), tag != key ? tag : "", (int)get("zorder").asInt(), {}, nullptr, Value());
        }
    }
}

static Value snapshotToValue(const Snapshot &s) {
    auto refs = storeRefs();
    Value root = mkDict();
    auto set = [&](const char *k, const Value &v) { root.dict()->set(Value::str(k), v); };
    auto locV = [](const Loc &l) { return mkTuple({Value::integer(l.script), Value::integer(l.pc)}); };
    set("version", Value::integer(1));
    set("loc", locV(s.loc));
    std::vector<Value> rs;
    for (auto &l : s.returnStack)
        rs.push_back(locV(l));
    set("returnStack", mkList(rs));
    std::vector<Value> ds;
    for (auto &frame : s.dynamicStack) {
        Value f = mkDict();
        for (auto &p : frame)
            if (isPlain(p.second))
                f.dict()->set(Value::str(p.first), p.second);
        ds.push_back(f);
    }
    set("dynamicStack", mkList(ds));
    Value st = mkDict();
    for (auto &kv : s.store)
        st.dict()->set(Value::str(kv.first), kv.second);
    set("store", st);
    set("scene", sceneToValue(s.scene, refs));
    Value music = mkDict();
    for (auto &kv : s.music) {
        std::vector<Value> files;
        for (auto &f : kv.second)
            files.push_back(Value::str(f));
        music.dict()->set(Value::str(kv.first), mkList(files));
    }
    set("music", music);
    Value vols = mkDict();
    for (auto &kv : s.channelVolumes)
        vols.dict()->set(Value::str(kv.first), Value::real(kv.second));
    set("volumes", vols);
    set("playTime", Value::real(s.playTime));
    set("saveName", Value::str(s.saveName));
    set("language", Value::str(s.language));
    set("scriptLanguage", Value::str(G.scriptLanguage));
    return root;
}

extern void captureSaveThumbnail(std::vector<uint8_t> &jpeg);

bool Game::saveSlot(const std::string &slot, const std::string &extraInfo) {
    if (!haveCheckpoint)
        return false;
    Value root = snapshotToValue(lastCheckpoint);
    root.dict()->set(Value::str("extraInfo"), Value::str(extraInfo));
    root.dict()->set(Value::str("mtime"), Value::integer((int64_t)time(nullptr)));
    std::vector<uint8_t> thumb;
    captureSaveThumbnail(thumb);
    root.dict()->set(Value::str("thumb"), Value::str(std::string(thumb.begin(), thumb.end())));
    GraphWriter w;
    std::vector<uint8_t> data = w.finish(root);
    makeDirs(userPath("saves/"));
    bool ok = writeFile(userPath("saves/" + slot + ".save"), data);
    savePersistent();
    logf("saved slot %s (%d bytes): %d", slot.c_str(), (int)data.size(), ok);
    return ok;
}

bool Game::loadSlot(const std::string &slot) {
    std::vector<uint8_t> data;
    if (!readFile(userPath("saves/" + slot + ".save"), data))
        return false;
    ValueFile vf;
    if (!vf.load(data))
        return false;
    std::unordered_map<Obj *, Value> done;
    Value root = fixSets(vf.root, done);
    DictObj *d = root.dict();
    auto get = [&](const char *k) {
        const Value *v = d->find(Value::str(k));
        return v ? *v : Value();
    };
    audio::stopAll();
    std::string language = get("language").s();
    if (!language.empty() && language != lang())
        switchLanguage(language);
    std::string savedScriptLang = get("scriptLanguage").s();
    auto locOf = [&](const Value &v) {
        Loc l{(int)ListObj_at(v, 0), (int)ListObj_at(v, 1)};
        return l;
    };
    (void)savedScriptLang;
    Snapshot s;
    s.loc = locOf(get("loc"));
    if (ListObj *rs = get("returnStack").list())
        for (auto &l : rs->v)
            s.returnStack.push_back(locOf(l));
    if (ListObj *ds = get("dynamicStack").list())
        for (auto &f : ds->v) {
            std::vector<std::pair<std::string, Value>> frame;
            if (DictObj *fd = f.dict())
                for (auto &kv : fd->items)
                    frame.emplace_back(kv.first.s(), kv.second);
            s.dynamicStack.push_back(frame);
        }
    if (DictObj *st = get("store").dict())
        for (auto &kv : st->items)
            s.store.emplace_back(kv.first.s(), kv.second);
    if (DictObj *m = get("music").dict())
        for (auto &kv : m->items) {
            std::vector<std::string> files;
            if (ListObj *fl = kv.second.list())
                for (auto &f : fl->v)
                    files.push_back(f.s());
            s.music[kv.first.s()] = files;
        }
    if (DictObj *vd = get("volumes").dict())
        for (auto &kv : vd->items)
            s.channelVolumes[kv.first.s()] = kv.second.num();
    s.playTime = get("playTime").num();
    s.language = lang();
    restore(s);
    sceneFromValue(*this, get("scene"));
    rollbackLog.clear();
    haveCheckpoint = false;
    inMainMenuContext = false;
    oldScene = nullptr;
    storeSet("_window", storeGet("_window"));
    // label after_load
    try {
        PY.call(storeGet("initialize_prefs"));
    } catch (PyError &) {
    }
    storeSet("ask_to_quit", Value::boolean(true));
    logf("loaded slot %s at %d:%d", slot.c_str(), s.loc.script, s.loc.pc);
    return true;
}

double ListObj_at(const Value &v, size_t i) {
    ListObj *l = v.list();
    return (l && i < l->v.size()) ? l->v[i].num() : -1;
}

void Game::savePersistent() {
    Value root = mkDict();
    Value attrs = mkDict();
    for (auto &kv : persistentV.inst()->attrs)
        if (isPlain(kv.second) || kv.second.dict())
            attrs.dict()->set(Value::str(kv.first), kv.second);
    root.dict()->set(Value::str("persistent"), attrs);
    Value prefsV = mkDict();
    for (auto &kv : preferencesV.inst()->attrs)
        if (isPlain(kv.second))
            prefsV.dict()->set(Value::str(kv.first), kv.second);
    prefsV.dict()->set(Value::str("#music"), Value::real(prefs.musicVolume));
    prefsV.dict()->set(Value::str("#sfx"), Value::real(prefs.sfxVolume));
    root.dict()->set(Value::str("preferences"), prefsV);
    GraphWriter w;
    makeDirs(userPath(""));
    writeFile(userPath("persistent.bin"), w.finish(root));
}

void Game::loadPersistent() {
    std::vector<uint8_t> data;
    InstObj *p = persistentV.inst();
    if (readFile(userPath("persistent.bin"), data)) {
        ValueFile vf;
        if (vf.load(data)) {
            std::unordered_map<Obj *, Value> done;
            Value root = fixSets(vf.root, done);
            if (DictObj *d = root.dict()) {
                if (const Value *pa = d->find(Value::str("persistent")))
                    if (DictObj *pd = pa->dict())
                        for (auto &kv : pd->items)
                            p->attrs[kv.first.s()] = kv.second;
                if (const Value *pr = d->find(Value::str("preferences")))
                    if (DictObj *prd = pr->dict())
                        for (auto &kv : prd->items) {
                            std::string k = kv.first.s();
                            if (k == "#music")
                                prefs.musicVolume = kv.second.num();
                            else if (k == "#sfx")
                                prefs.sfxVolume = kv.second.num();
                            else
                                preferencesV.inst()->attrs[k] = kv.second;
                        }
            }
        }
    }
    // defaults normally created by init code
    auto ensure = [&](const char *name, const Value &v) {
        if (!p->has(name) || p->get(name).isNone())
            p->attrs[name] = v;
    };
    ensure("seen_labels", mkList());
    ensure("_seen_images", mkDict());
    ensure("_seen_audio", mkDict());
    ensure("_chosen", mkDict());
    ensure("breadcrumbs", mkList());
    ensure("seen_videos", mkList({Value::str("video/4ls.mkv")}));
    ensure("afm_time", Value::integer(5));
    ensure("oldvol", mkDict());
    for (const char *k : {"emi", "emibad", "hanako", "hanakosad", "hanakorage", "lilly", "lillybad", "shizune",
                          "shizunebad", "rin", "rinbad", "rintrue", "bad"})
        ensure(k, Value::integer(0));
    ensure("hdisabled", Value::boolean(false));
    audio::setMixerVolume("music", prefs.musicVolume);
    audio::setMixerVolume("sfx", prefs.sfxVolume);
}
