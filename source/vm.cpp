#include "audio.h"
#include "game.h"
#include <algorithm>
#include <cmath>

Game G;

static Value tupleAt(const Value &t, size_t i) {
    ListObj *l = t.list();
    return (l && i < l->v.size()) ? l->v[i] : Value();
}
static int codeOf(const Value &v) { return v.t == T::Code ? (int)v.i : -1; }
static std::string strOf(const Value &v) { return v.isStr() ? v.s() : std::string(); }

static Value rootGet(const Value &root, const char *k) {
    if (DictObj *d = root.dict())
        if (const Value *v = d->find(Value::str(k)))
            return *v;
    return Value();
}

// ------------------------------------------------------------------ script loading

static ImSpecOp parseImspec(const Value &t) {
    ImSpecOp im;
    if (!t.list())
        return im;
    im.valid = true;
    if (ListObj *n = tupleAt(t, 0).list())
        for (auto &x : n->v)
            im.name.push_back(valueStr(x));
    im.expr = codeOf(tupleAt(t, 1));
    im.tag = strOf(tupleAt(t, 2));
    if (ListObj *al = tupleAt(t, 3).list())
        for (auto &x : al->v)
            im.atList.push_back(codeOf(x));
    im.layer = tupleAt(t, 4).isStr() ? tupleAt(t, 4).s() : "master";
    im.zorder = codeOf(tupleAt(t, 5));
    if (ListObj *bl = tupleAt(t, 6).list())
        for (auto &x : bl->v)
            im.behind.push_back(valueStr(x));
    return im;
}

bool Game::loadScript(const std::string &path, const std::string &name) {
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes)) {
        logf("missing script %s", path.c_str());
        return false;
    }
    ValueFile vf;
    if (!vf.load(bytes)) {
        logf("bad script %s", path.c_str());
        return false;
    }
    Script sc;
    sc.name = name;
    ListObj *ops = rootGet(vf.root, "ops").list();
    if (!ops)
        return false;
    sc.ops.resize(ops->v.size());
    for (size_t i = 0; i < ops->v.size(); i++) {
        const Value &t = ops->v[i];
        Op &o = sc.ops[i];
        o.op = (uint8_t)tupleAt(t, 0).asInt();
        switch (o.op) {
        case OP_LABEL:
            o.s1 = strOf(tupleAt(t, 1));
            if (ListObj *pl = tupleAt(t, 2).list())
                for (auto &p : pl->v)
                    o.params.emplace_back(strOf(tupleAt(p, 0)), codeOf(tupleAt(p, 1)));
            o.b1 = tupleAt(t, 2).list() != nullptr;
            break;
        case OP_SAY:
            o.c1 = codeOf(tupleAt(t, 1));
            o.s1 = strOf(tupleAt(t, 2));
            o.b1 = tupleAt(t, 3).truthy();
            o.c2 = codeOf(tupleAt(t, 4));
            o.file = strOf(tupleAt(t, 6));
            o.line = (int)tupleAt(t, 7).asInt();
            break;
        case OP_SHOW:
            o.im = parseImspec(tupleAt(t, 1));
            o.atl = tupleAt(t, 2);
            break;
        case OP_SCENE:
            o.im = parseImspec(tupleAt(t, 1));
            o.s1 = strOf(tupleAt(t, 2));
            o.atl = tupleAt(t, 3);
            break;
        case OP_HIDE:
            o.im = parseImspec(tupleAt(t, 1));
            break;
        case OP_WITH:
            o.c1 = codeOf(tupleAt(t, 1));
            o.c2 = codeOf(tupleAt(t, 2));
            break;
        case OP_PYTHON:
            o.c1 = codeOf(tupleAt(t, 1));
            o.b1 = tupleAt(t, 2).truthy();
            o.file = strOf(tupleAt(t, 3));
            o.line = (int)tupleAt(t, 4).asInt();
            break;
        case OP_JUMP:
        case OP_JUMPIN:
        case OP_JUMPOUT: {
            Value a = tupleAt(t, 1);
            if (a.isInt())
                o.target = (int)a.i;
            else
                o.s1 = strOf(a);
            o.c1 = codeOf(tupleAt(t, 2));
            break;
        }
        case OP_JIFNOT:
            o.c1 = codeOf(tupleAt(t, 1));
            o.target = (int)tupleAt(t, 2).asInt();
            break;
        case OP_MENU:
            if (ListObj *items = tupleAt(t, 1).list())
                for (auto &it : items->v) {
                    MenuItemOp mi;
                    mi.label = strOf(tupleAt(it, 0));
                    mi.cond = codeOf(tupleAt(it, 1));
                    mi.target = (int)tupleAt(it, 2).asInt();
                    o.items.push_back(mi);
                }
            o.c1 = codeOf(tupleAt(t, 2));
            o.c2 = codeOf(tupleAt(t, 3));
            o.file = strOf(tupleAt(t, 4));
            o.line = (int)tupleAt(t, 5).asInt();
            o.endPc = (int)tupleAt(t, 6).asInt();
            break;
        case OP_CALL:
            o.s1 = strOf(tupleAt(t, 1));
            o.c1 = codeOf(tupleAt(t, 2));
            if (ListObj *al = tupleAt(t, 3).list())
                for (auto &p : al->v)
                    o.params.emplace_back(strOf(tupleAt(p, 0)), codeOf(tupleAt(p, 1)));
            break;
        case OP_RETURN:
            o.c1 = codeOf(tupleAt(t, 1));
            break;
        case OP_PLAY:
        case OP_QUEUE: {
            Value ch = tupleAt(t, 1);
            if (ch.isStr())
                o.s1 = ch.s();
            else
                o.c4 = codeOf(ch);
            o.c1 = codeOf(tupleAt(t, 2));
            o.c2 = codeOf(tupleAt(t, 3));
            o.c3 = codeOf(tupleAt(t, 4));
            Value lp = tupleAt(t, 5);
            o.loop = lp.isNone() ? -1 : (lp.truthy() ? 1 : 0);
            o.b1 = tupleAt(t, 6).truthy();
            break;
        }
        case OP_STOP: {
            Value ch = tupleAt(t, 1);
            if (ch.isStr())
                o.s1 = ch.s();
            else
                o.c4 = codeOf(ch);
            o.c1 = codeOf(tupleAt(t, 2));
            break;
        }
        case OP_WINDOW:
            o.b1 = tupleAt(t, 1).truthy();
            o.c1 = codeOf(tupleAt(t, 2));
            break;
        case OP_NVL:
            o.s1 = strOf(tupleAt(t, 1));
            o.c1 = codeOf(tupleAt(t, 2));
            break;
        case OP_PAUSE:
            o.c1 = codeOf(tupleAt(t, 1));
            break;
        case OP_USER:
            o.s1 = strOf(tupleAt(t, 1));
            break;
        default:
            break;
        }
    }
    if (DictObj *lb = rootGet(vf.root, "labels").dict())
        for (auto &kv : lb->items)
            sc.labels[kv.first.s()] = (int)kv.second.asInt();
    // replace existing script with the same name
    int index = -1;
    for (size_t i = 0; i < scripts.size(); i++)
        if (scripts[i].name == name)
            index = (int)i;
    if (index < 0) {
        scripts.push_back(std::move(sc));
        index = (int)scripts.size() - 1;
    } else {
        for (auto it = labels.begin(); it != labels.end();)
            if (it->second.script == index)
                it = labels.erase(it);
            else
                ++it;
        scripts[index] = std::move(sc);
    }
    for (auto &kv : scripts[index].labels)
        labels[kv.first] = Loc{index, kv.second};
    logf("script %s: %d ops, %d labels", name.c_str(), (int)scripts[index].ops.size(),
         (int)scripts[index].labels.size());
    return true;
}

void Game::setScriptLanguage(const std::string &l) {
    std::string base = l.substr(l.rfind('_') + 1);
    if (base == scriptLanguage)
        return;
    // unload previous language script
    for (size_t i = 1; i < scripts.size(); i++) {
        if (scripts[i].name == "lang") {
            for (auto it = labels.begin(); it != labels.end();)
                if (it->second.script == (int)i)
                    it = labels.erase(it);
                else
                    ++it;
            scripts[i].ops.clear();
            scripts[i].ops.shrink_to_fit();
            scripts[i].labels.clear();
        }
    }
    scriptLanguage = base;
    if (base == "en" || base.empty())
        return;
    std::string file = "data/script_" + base + ".bin";
    if (fileExists(romfsPath(file)))
        loadScript(romfsPath(file), "lang");
}

bool Game::hasLabel(const std::string &name) const { return labels.count(name) != 0; }

const Op *Game::opAt(const Loc &l) const {
    if (l.script < 0 || l.script >= (int)scripts.size())
        return nullptr;
    const Script &s = scripts[l.script];
    if (l.pc < 0 || l.pc >= (int)s.ops.size())
        return nullptr;
    return &s.ops[l.pc];
}

Value Game::evalCode(int code) {
    if (code < 0)
        return Value();
    return PY.eval(code);
}

void Game::execCode(int code, bool hide) {
    if (code >= 0)
        PY.exec(code, nullptr, hide);
}

// ------------------------------------------------------------------ init

static Value instFromDict(const std::string &cls, const Value &dict) {
    Value v = mkInst(cls);
    if (DictObj *d = dict.dict())
        for (auto &kv : d->items)
            v.inst()->attrs[kv.first.s()] = kv.second;
    return v;
}

static bool isDataValue(const Value &v, int depth = 0) {
    if (depth > 6)
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
                if (!isDataValue(x, depth + 1))
                    return false;
            return true;
        }
        if (DictObj *d = v.dict()) {
            for (auto &kv : d->items)
                if (!isDataValue(kv.first, depth + 1) || !isDataValue(kv.second, depth + 1))
                    return false;
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool Game::init() {
    std::vector<uint8_t> bytes;
    double t0 = nowSeconds();
    if (!readFile(romfsPath("data/game.bin"), bytes) || !data.load(bytes)) {
        logf("cannot load data/game.bin");
        return false;
    }
    bytes.clear();
    bytes.shrink_to_fit();
    logf("game.bin loaded in %.2fs (%d objects)", nowSeconds() - t0, (int)data.objects.size());
    const Value &root = data.root;

    PY.init();
    PY.loadCodes(rootGet(root, "codes"), rootGet(root, "ast_fields"));

    storeV = mkInst("store");
    store = storeV.inst();
    PY.store = store;
    PY.storeValue = storeV;
    if (DictObj *sd = rootGet(root, "store").dict())
        for (auto &kv : sd->items)
            store->attrs[kv.first.s()] = kv.second;
    PY.modules["store"] = storeV;
    store->attrs["store"] = storeV;

    configV = instFromDict("config", rootGet(root, "config"));
    configV.inst()->attrs["skipping"] = Value();
    configV.inst()->attrs["allow_skipping"] = Value::boolean(true);
    configV.inst()->attrs["fade_music"] = Value::real(0.0);
    store->attrs["config"] = configV;
    persistentV = mkInst("persistent");
    store->attrs["persistent"] = persistentV;
    preferencesV = mkInst("preferences");
    store->attrs["_preferences"] = preferencesV;
    languagesV = rootGet(root, "languages");
    if (ListObj *al = rootGet(root, "available_languages").list())
        for (auto &x : al->v)
            availableLanguages.push_back(x.s());

    if (DictObj *images = rootGet(root, "images").dict())
        for (auto &kv : images->items)
            registerImage(kv.first.s(), kv.second);

    registerTransformNatives(PY);
    registerTransitionNatives(PY);
    registerGameNatives(PY);

    // function definitions (common library first, then the game)
    int defsOk = 0, defsErr = 0;
    if (ListObj *defs = rootGet(root, "defs").list())
        for (auto &d : defs->v) {
            int code = codeOf(tupleAt(d, 0));
            if (code < 0 || code >= (int)PY.codes.size() || !PY.codes[code])
                continue;
            for (Node *s : PY.codes[code]->l1) {
                if (s->k != S_DEF)
                    continue;
                try {
                    Value ret;
                    std::vector<Node *> one{s};
                    PY.execBody(one, nullptr, ret);
                    defsOk++;
                } catch (PyError &e) {
                    defsErr++;
                }
            }
        }
    logf("python defs: %d ok, %d failed", defsOk, defsErr);
    // natives registered as store overrides take precedence over python defs
    extern void installStoreOverrides(Interp & I);
    installStoreOverrides(PY);

    for (auto &kv : store->attrs)
        if (isDataValue(kv.second))
            dataVars.insert(kv.first);

    if (!loadScript(romfsPath("data/game.bin"), "base"))
        return false;
    loadPersistent();
    std::string language = persistentGet("current_language").isStr() ? persistentGet("current_language").s() : "en";
    if (std::find(availableLanguages.begin(), availableLanguages.end(), language) == availableLanguages.end())
        language = "en";
    switchLanguage(language);
    logf("init done in %.2fs", nowSeconds() - t0);
    return true;
}

// ------------------------------------------------------------------ language

std::string Game::lang() const {
    Value v = const_cast<Game *>(this)->persistentGet("current_language");
    return v.isStr() ? v.s() : "en";
}

void Game::switchLanguage(const std::string &l) {
    DictObj *langs = languagesV.dict();
    if (!langs)
        return;
    const Value *ld = langs->find(Value::str(l));
    if (!ld)
        return;
    persistentSet("current_language", Value::str(l));
    storeSet("np_current_language", Value::str(l));
    Value dsv = rootGet(*ld, "displayStrings");
    storeSet("displayStrings", dsv);
    storeSet("s_scenes", rootGet(*ld, "s_scenes"));
    if (DictObj *chars = rootGet(*ld, "characters").dict())
        for (auto &kv : chars->items)
            storeSet(kv.first.s(), kv.second);
    setStyleTable(rootGet(*ld, "styles"));
    storeSet("readback_buffer", mkList());
    setScriptLanguage(l);
    Value lastVisited = storeGet("last_visited_label");
    if (!lastVisited.isNone()) {
        try {
            storeSet("gm_exit_to", PY.call(storeGet("wrap_label"), {lastVisited}));
        } catch (PyError &) {
        }
    }
}

Value Game::displayString(const std::string &key) {
    Value ds = storeGet("displayStrings");
    if (InstObj *in = ds.inst())
        return in->get(key);
    return Value();
}

// ------------------------------------------------------------------ persistent

Value Game::persistentGet(const std::string &name) { return persistentV.inst()->get(name); }
void Game::persistentSet(const std::string &name, const Value &v) { persistentV.inst()->attrs[name] = v; }

// ------------------------------------------------------------------ control flow

void Game::jumpTo(const std::string &label) {
    auto it = labels.find(label);
    if (it == labels.end()) {
        logf("jump to unknown label %s", label.c_str());
        throwPy("ScriptError", "could not find label '" + label + "'");
    }
    next = it->second;
}

void Game::callLabel(const std::string &label, const std::vector<Value> &args,
                     const std::vector<std::pair<std::string, Value>> &kwargs) {
    auto it = labels.find(label);
    if (it == labels.end())
        throwPy("ScriptError", "could not find label '" + label + "'");
    returnStack.push_back(next);
    dynamicStack.emplace_back();
    storeSet("_args", mkTuple(args));
    Value kw = mkDict();
    for (auto &k : kwargs)
        kw.dict()->set(Value::str(k.first), k.second);
    storeSet("_kwargs", kw);
    next = it->second;
}

static void setDynamic(Game &g, const std::string &name, const Value &v) {
    if (!g.dynamicStack.empty()) {
        auto &frame = g.dynamicStack.back();
        bool already = false;
        for (auto &p : frame)
            if (p.first == name)
                already = true;
        if (!already)
            frame.emplace_back(name, g.store->has(name) ? g.store->attrs[name] : Value::str("\x01unset"));
    }
    g.storeSet(name, v);
}

void Game::execOp(const Op &o, Loc loc) {
    switch (o.op) {
    case OP_NOP:
        break;
    case OP_LABEL: {
        Value args = storeGet("_args"), kwargs = storeGet("_kwargs");
        storeSet("_args", Value());
        storeSet("_kwargs", Value());
        if (o.b1) {
            std::vector<Value> pos;
            if (ListObj *l = args.list())
                pos = l->v;
            for (size_t i = 0; i < o.params.size(); i++) {
                Value v;
                bool found = false;
                if (i < pos.size()) {
                    v = pos[i];
                    found = true;
                } else if (DictObj *kd = kwargs.dict()) {
                    if (const Value *kv = kd->find(Value::str(o.params[i].first))) {
                        v = *kv;
                        found = true;
                    }
                }
                if (!found && o.params[i].second >= 0)
                    v = evalCode(o.params[i].second);
                setDynamic(*this, o.params[i].first, v);
            }
        }
        // config.label_callback = fallthrough_catcher (script labels reached without a call jump back)
        break;
    }
    case OP_SAY: {
        Value who = o.c1 >= 0 ? evalCode(o.c1) : Value();
        say(who, o.s1, o.b1);
        break;
    }
    case OP_SHOW:
        show(o.im, o.atl);
        break;
    case OP_SCENE:
        sceneClear(o.s1.empty() ? "master" : o.s1);
        if (o.im.valid)
            show(o.im, o.atl, o.s1);
        break;
    case OP_HIDE: {
        std::string tag = !o.im.tag.empty() ? o.im.tag : (o.im.name.empty() ? "" : o.im.name[0]);
        hide(tag, o.im.layer);
        break;
    }
    case OP_WITH: {
        if (o.c2 >= 0) {
            // paired with: the transition was already started by the show statement
            break;
        }
        Value trans = evalCode(o.c1);
        withStatement(trans);
        break;
    }
    case OP_PYTHON:
        PY.currentFile = o.file;
        PY.currentLine = o.line;
        execCode(o.c1, o.b1);
        break;
    case OP_JUMP:
        if (o.target >= 0)
            next = Loc{loc.script, o.target};
        else if (o.c1 >= 0)
            jumpTo(valueStr(evalCode(o.c1)));
        else
            jumpTo(o.s1);
        break;
    case OP_JIFNOT:
        if (!evalCode(o.c1).truthy())
            next = Loc{loc.script, o.target};
        break;
    case OP_MENU: {
        std::vector<std::pair<std::string, Value>> items;
        for (size_t i = 0; i < o.items.size(); i++) {
            const MenuItemOp &mi = o.items[i];
            if (mi.cond >= 0 && !evalCode(mi.cond).truthy())
                continue;
            items.emplace_back(mi.label, mi.target >= 0 ? Value::integer((int64_t)i) : Value());
        }
        Value setv = o.c1 >= 0 ? evalCode(o.c1) : Value();
        Value choice;
        {
            // call the store's menu function (KS overrides it)
            std::vector<Value> tuples;
            for (auto &it : items)
                tuples.push_back(mkTuple({Value::str(it.first), it.second}));
            CallArgs a;
            a.pos.push_back(mkList(tuples));
            choice = PY.call(storeGet("menu"), a);
        }
        if (choice.isInt() && choice.i >= 0 && choice.i < (int64_t)o.items.size())
            next = Loc{loc.script, o.items[choice.i].target};
        else
            next = Loc{loc.script, o.endPc};
        break;
    }
    case OP_CALL: {
        std::string label = o.c1 >= 0 ? valueStr(evalCode(o.c1)) : o.s1;
        std::vector<Value> args;
        std::vector<std::pair<std::string, Value>> kwargs;
        for (auto &p : o.params) {
            Value v = evalCode(p.second);
            if (p.first.empty())
                args.push_back(v);
            else
                kwargs.emplace_back(p.first, v);
        }
        callLabel(label, args, kwargs);
        break;
    }
    case OP_RETURN: {
        Value rv = o.c1 >= 0 ? evalCode(o.c1) : Value();
        storeSet("_return", rv);
        if (returnStack.empty()) {
            // return from the top level: end of game
            throw FullRestartSignal();
        }
        next = returnStack.back();
        returnStack.pop_back();
        if (!dynamicStack.empty()) {
            for (auto &p : dynamicStack.back()) {
                if (p.second.isStr() && p.second.s() == "\x01unset")
                    store->attrs.erase(p.first);
                else
                    store->attrs[p.first] = p.second;
            }
            dynamicStack.pop_back();
        }
        break;
    }
    case OP_PLAY:
    case OP_QUEUE: {
        std::string channel = o.c4 >= 0 ? valueStr(evalCode(o.c4)) : o.s1;
        Value file = evalCode(o.c1);
        std::vector<std::string> files;
        if (ListObj *fl = file.list()) {
            for (auto &x : fl->v)
                files.push_back(x.s());
        } else if (file.isStr()) {
            files.push_back(file.s());
        }
        if (files.empty())
            break;
        if (channel == "movie") {
            extern void startBackgroundMovie(const std::string &file);
            startBackgroundMovie(files[0]);
            break;
        }
        double fadein = o.c2 >= 0 ? evalCode(o.c2).num() : 0;
        Value fo = o.c3 >= 0 ? evalCode(o.c3) : Value();
        double fadeout = fo.isNum() ? fo.num() : 0;
        if (o.op == OP_PLAY)
            audio::play(channel, files, fadein, fadeout, o.loop, o.b1);
        else
            audio::queue(channel, files, o.loop, true);
        extern void noteSeenAudio(const std::vector<std::string> &files);
        noteSeenAudio(files);
        break;
    }
    case OP_STOP: {
        std::string channel = o.c4 >= 0 ? valueStr(evalCode(o.c4)) : o.s1;
        Value fo = o.c1 >= 0 ? evalCode(o.c1) : Value();
        if (channel == "movie") {
            extern void stopBackgroundMovie();
            stopBackgroundMovie();
            break;
        }
        audio::stop(channel, fo.isNum() ? fo.num() : 0);
        break;
    }
    case OP_WINDOW: {
        bool windowOn = storeGet("_window").truthy();
        if (o.b1 == windowOn)
            break;
        Value trans;
        if (o.c1 >= 0)
            trans = evalCode(o.c1);
        else
            trans = configV.inst()->get(o.b1 ? "window_show_transition" : "window_hide_transition");
        withStatement(Value());
        storeSet("_window", Value::boolean(o.b1));
        withStatement(trans);
        break;
    }
    case OP_NVL:
        if (o.s1 == "clear") {
            storeSet("nvl_list", mkList());
        } else {
            Value trans = o.c1 >= 0 ? evalCode(o.c1) : Value();
            nvlShow(trans, o.s1 == "hide");
        }
        break;
    case OP_JUMPIN:
    case OP_JUMPOUT: {
        std::string label = o.c1 >= 0 ? valueStr(evalCode(o.c1)) : o.s1;
        if (o.op == OP_JUMPOUT) {
            storeSet("nvl_list", mkList());
            if (!storeGet("playthroughflag").truthy()) {
                PY.call(storeGet("replay_end"));
            }
        }
        logf("scene %s", label.c_str());
        storeSet("last_scene_label", Value::str(label));
        storeSet("save_name", Value::str(label));
        PY.call(storeGet("scene_register"), {Value::str(label)});
        jumpTo(label);
        break;
    }
    case OP_PAUSE: {
        if (o.c1 >= 0) {
            Value delay = evalCode(o.c1);
            CallArgs a;
            a.pos.push_back(delay);
            withStatement(PY.call(storeGet("Pause"), a));
        } else {
            PY.call(PY.getAttr(storeGet("renpy"), "pause"));
        }
        break;
    }
    default:
        logf("unhandled op %d", o.op);
        break;
    }
}

// ------------------------------------------------------------------ show / hide / scene

void Game::show(const ImSpecOp &im, const Value &atl, const std::string &layerOverride) {
    std::string layer = layerOverride.empty() ? im.layer : layerOverride;
    std::vector<Value> atList;
    for (int c : im.atList)
        atList.push_back(evalCode(c));
    int zorder = im.zorder >= 0 ? (int)evalCode(im.zorder).asInt() : 0;
    DispP what;
    std::string name;
    for (auto &n : im.name) {
        if (!name.empty())
            name += ' ';
        name += n;
    }
    if (im.expr >= 0) {
        Value v = evalCode(im.expr);
        what = toDisp(v);
    }
    showName(name, atList, layer, im.tag, zorder, im.behind, what, atl);
}

void Game::showName(const std::string &nameIn, const std::vector<Value> &atListIn, const std::string &layer,
                    const std::string &tagIn, int zorder, const std::vector<std::string> &behind, DispP what,
                    const Value &atl) {
    std::string name = nameIn;
    std::string key = !tagIn.empty() ? tagIn : name.substr(0, name.find(' '));
    DispP img = what;
    if (!img) {
        if (!hasImage(name)) {
            // image attributes: find an image with this tag whose attributes contain the requested ones
            const auto &cands = imageNamesWithTag(key);
            std::string want = name.size() > key.size() ? name.substr(key.size() + 1) : "";
            std::string found;
            for (auto &c : cands) {
                if (c.size() > key.size() && c.substr(key.size() + 1) == want) {
                    found = c;
                    break;
                }
            }
            if (!found.empty())
                name = found;
            else
                logf("show: unknown image '%s'", name.c_str());
        }
        img = toDisp(Value::str(name));
    }
    for (auto &t : atListIn)
        img = applyTransform(t, img);
    if (!name.empty()) {
        // persistent._seen_images[name] = True
        Value seen = persistentGet("_seen_images");
        if (!seen.dict()) {
            seen = mkDict();
            persistentSet("_seen_images", seen);
        }
        std::vector<Value> parts;
        size_t p = 0;
        while (p <= name.size()) {
            size_t sp = name.find(' ', p);
            if (sp == std::string::npos)
                sp = name.size();
            if (sp > p)
                parts.push_back(Value::str(name.substr(p, sp - p)));
            p = sp + 1;
        }
        seen.dict()->set(mkTuple(parts), Value::boolean(true));
    }
    bool hasAtl = atl.inst() != nullptr;
    if (hasAtl) {
        auto t = std::make_shared<TransformDisp>();
        t->kind = "ATLTransform";
        t->atlRaw = atl;
        t->context = mkDict();
        t->child = img;
        t->atlAnimation = atl.inst()->get("animation").truthy();
        img = t;
    }
    if (!tagIn.empty() && !name.empty())
        name = tagIn + name.substr(name.find(' ') == std::string::npos ? name.size() : name.find(' '));
    scene.add(layer, img, key, zorder, behind, atListIn, name, hasAtl);
}

void Game::hide(const std::string &tag, const std::string &layer) { scene.remove(layer, tag); }

void Game::sceneClear(const std::string &layer) { scene.clear(layer); }
