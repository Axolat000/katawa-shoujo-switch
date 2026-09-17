#include "audio.h"
#include "game.h"
#include "input.h"
#include <algorithm>
#include <cmath>
#include <random>

Input g_input;

bool platformPump();  // main.cpp: polls events into g_input, false = quit
void platformSwap();  // main.cpp
void drawOverlays();  // screens.cpp: indicator icons, notifications
bool nativeScreenActive();

static DispP g_rootWidget;
static std::shared_ptr<TransitionDisp> g_transition;
static double g_transitionStart = -1;

static Value tupleAt(const Value &t, size_t i) {
    ListObj *l = t.list();
    return (l && i < l->v.size()) ? l->v[i] : Value();
}

// ------------------------------------------------------------------ frame

void Game::render() {
    g_frameTime = now;
    gfx::bindScreen();
    gfx::clear(0, 0, 0, 1);
    extern void pushBinding(std::function<void()> rebind);
    extern void popBinding();
    pushBinding([]() { gfx::bindScreen(); });
    if (g_rootWidget) {
        double st = g_transitionStart >= 0 ? now - g_transitionStart : 0;
        RenderP r = g_rootWidget->render(VW, VH, st, st);
        drawRender(r, Mat(), 1.0f);
    }
    if (cur && cur->draw)
        cur->draw();
    drawOverlays();
    popBinding();
}

void Game::frame(bool drawOnly) {
    (void)drawOnly;
    g_input = Input();
    if (!platformPump()) {
        quitRequested = true;
        throw QuitSignal();
    }
    double t = nowSeconds();
    double dt = std::max(0.0, std::min(0.25, t - now));
    now = t;
    if (!inMainMenuContext)
        playTime += dt;
    texture::beginFrame();
    text::beginFrame();
    render();
    platformSwap();
}

// ------------------------------------------------------------------ window / with

void Game::showWindowIfNeeded() {
    if (!storeGet("_window").truthy())
        return;
    if (scene.shownWindow)
        return;
    // config.empty_window: narrator("", interact=False)
    try {
        Value narrator = storeGet("narrator");
        CallArgs a;
        a.pos.push_back(Value::str(""));
        a.kw.emplace_back("interact", Value::boolean(false));
        PY.call(narrator, a);
    } catch (PyError &e) {
        logf("empty window: %s %s", e.type.c_str(), e.msg.c_str());
    }
}

void Game::withNone() {
    showWindowIfNeeded();
    oldScene = scene.computeScene();
    scene.clear("overlay");
    scene.clear("transient");
    scene.shownWindow = false;
}

bool Game::withStatement(const Value &transIn, bool clear) {
    Value trans = transIn;
    if (!configV.inst()->get("skipping").isNone())
        trans = Value();
    if (!prefs.transitions)
        trans = Value();
    if (trans.isNone()) {
        withNone();
        return false;
    }
    pendingTransition = trans;
    Interaction in;
    in.type = IType::With;
    in.transPause = true;
    Value r = interact(in);
    (void)clear;
    return r.truthy();
}

// ------------------------------------------------------------------ interaction

static bool skipAllowed(Game &g) {
    Value sk = g.configV.inst()->get("skipping");
    return !sk.isNone() && g.configV.inst()->get("allow_skipping").truthy();
}

Value Game::interact(Interaction &in) {
    Interaction *prev = cur;
    cur = &in;
    interactDepth++;
    if (!in.suppressWindow)
        showWindowIfNeeded();

    // transition setup
    std::shared_ptr<RootDisp> sceneRoot = scene.computeScene();
    Value trans = pendingTransition;
    pendingTransition = Value();
    bool suppressTrans = skipAllowed(*this) || !prefs.transitions;
    if (in.transPause && (trans.isNone() || suppressTrans || !oldScene)) {
        cur = prev;
        interactDepth--;
        scene.clear("transient");
        scene.shownWindow = false;
        oldScene = sceneRoot;
        return Value::boolean(false);
    }
    DispP root = sceneRoot;
    std::shared_ptr<TransitionDisp> transDisp;
    if (!trans.isNone() && !suppressTrans && oldScene) {
        transDisp = makeTransition(trans, oldScene, sceneRoot);
        if (transDisp)
            root = transDisp;
    }
    DispP prevRoot = g_rootWidget;
    auto prevTransition = g_transition;
    double prevTransitionStart = g_transitionStart;
    g_rootWidget = root;
    g_transition = transDisp;
    g_interactTime = nowSeconds();
    g_transitionStart = g_interactTime;
    now = g_interactTime;
    scene.setTimes(g_interactTime);
    oldScene = sceneRoot;
    double start = now;
    double skipDelay = configV.inst()->get("skip_delay").isNum() ? configV.inst()->get("skip_delay").num() / 1000.0 : 0.025;

    Value result;
    try {
        while (!in.done) {
            frame();
            const Input &inp = g_input;
            double elapsed = now - start;
            if (inp.quit)
                throw QuitSignal();

            // global keys (not while a native screen owns input)
            if (in.type != IType::Screen) {
                if (inp.menu && !inMainMenuContext && interactDepth == 1) {
                    gameMenu("");
                    g_rootWidget = root;
                    continue;
                }
                if (inp.history && !inMainMenuContext && interactDepth == 1) {
                    gameMenu("history");
                    g_rootWidget = root;
                    continue;
                }
                if (inp.rollback && !inMainMenuContext && interactDepth == 1 && in.type != IType::Movie) {
                    if (!rollbackLog.empty())
                        throw RollbackSignal();
                }
                if (inp.skipToggle && configV.inst()->get("allow_skipping").truthy() && !inMainMenuContext) {
                    if (configV.inst()->get("skipping").isNone())
                        configV.inst()->attrs["skipping"] = Value::str("slow");
                    else
                        configV.inst()->attrs["skipping"] = Value();
                }
                if (inp.autoToggle && !inMainMenuContext) {
                    Value afm = PY.getAttr(preferencesV, "afm_time");
                    if (afm.num() == 0)
                        PY.setAttr(preferencesV, "afm_time", persistentGet("afm_time").isNum() ? persistentGet("afm_time") : Value::integer(5));
                    else
                        PY.setAttr(preferencesV, "afm_time", Value::integer(0));
                }
                if (inp.hide && (in.type == IType::Say || in.type == IType::Menu) && !inMainMenuContext) {
                    gameMenu("hide");
                    g_rootWidget = root;
                    continue;
                }
                skipHeld = inp.skipHeld && configV.inst()->get("allow_skipping").truthy() && !inMainMenuContext;
            }
            bool skipping = (skipAllowed(*this) || skipHeld) && !inMainMenuContext;
            bool clicked = inp.accept || (inp.pointerReleased && in.type != IType::Menu);
            if (in.update && in.update()) {
                in.done = true;
                break;
            }
            switch (in.type) {
            case IType::With:
                if (!transDisp || now - g_transitionStart >= transDisp->delay)
                    in.done = true;
                else if (clicked || skipping)
                    in.done = true;
                if (in.done)
                    result = Value::boolean(clicked);
                break;
            case IType::Pause:
                if (in.delay >= 0 && elapsed >= in.delay) {
                    in.done = true;
                    result = Value::boolean(false);
                } else if ((clicked && !in.hard) || (skipping && !in.hard)) {
                    in.done = true;
                    result = Value::boolean(true);
                }
                break;
            case IType::Say: {
                SayState &say = *in.say;
                int cps = prefs.textCps;
                Value tcps = PY.getAttr(preferencesV, "text_cps");
                if (tcps.isNum())
                    cps = (int)tcps.num();
                if (say.what) {
                    if (say.slow && cps > 0 && !say.slowDone) {
                        int shown = say.start + (int)((now - say.startTime) * cps);
                        int end = say.end < 0 ? INT32_MAX : say.end;
                        if (shown >= end) {
                            say.slowDone = true;
                            say.what->reveal = say.end;
                        } else {
                            say.what->reveal = shown;
                        }
                    } else {
                        say.slowDone = true;
                        say.what->reveal = say.end;
                    }
                }
                if (skipping) {
                    if (skipHeld || elapsed >= skipDelay) {
                        in.done = true;
                        result = Value::boolean(true);
                    }
                    break;
                }
                if (clicked) {
                    if (!say.slowDone) {
                        say.slowDone = true;
                        if (say.what)
                            say.what->reveal = say.end;
                    } else {
                        in.done = true;
                        result = Value::boolean(true);
                    }
                    break;
                }
                // auto-forward mode
                Value afm = PY.getAttr(preferencesV, "afm_time");
                if (say.slowDone && afm.isNum() && afm.num() > 0 && in.clickable) {
                    double delay = afm.num() * 0.25 + say.afmChars * 0.02 + 0.5;
                    static double doneAt = 0;
                    if (say.startTime > doneAt)
                        doneAt = now;
                    if (now - std::max(doneAt, say.startTime) > delay) {
                        in.done = true;
                        result = Value::boolean(true);
                    }
                }
                break;
            }
            case IType::Menu:
                // handled by the menu's update hook
                if (skipping && prefs.skipAfterChoices == false)
                    configV.inst()->attrs["skipping"] = Value();
                break;
            case IType::Movie:
            case IType::Screen:
                break;
            }
        }
    } catch (...) {
        cur = prev;
        interactDepth--;
        g_rootWidget = prevRoot;
        g_transition = prevTransition;
        g_transitionStart = prevTransitionStart;
        scene.clear("transient");
        scene.shownWindow = false;
        throw;
    }
    if (!in.result.isNone())
        result = in.result;
    cur = prev;
    interactDepth--;
    // Ren'Py keeps the last frame on screen until the next interaction
    g_rootWidget = sceneRoot;
    g_transition = nullptr;
    scene.clear("transient");
    scene.shownWindow = false;
    return result;
}

// ------------------------------------------------------------------ checkpoints / rollback

void Game::checkpoint() {
    if (!haveCheckpoint)
        return;
    if (rollbackBlocked) {
        rollbackBlocked = false;
    }
    rollbackLog.push_back(lastCheckpoint);
    while (rollbackLog.size() > 100)
        rollbackLog.pop_front();
}

void Game::rollback() {
    if (rollbackLog.empty())
        return;
    Snapshot s = rollbackLog.back();
    rollbackLog.pop_back();
    restore(s);
    afterRollback = true;
}

// ------------------------------------------------------------------ say window

static Value charAttr(const Value &ch, const char *name) {
    InstObj *in = ch.inst();
    return in ? in->get(name) : Value();
}

static std::string styleNameOf(const Value &args, const std::string &def) {
    if (DictObj *d = args.dict())
        if (const Value *s = d->find(Value::str("style")))
            if (s->isStr())
                return s->s();
    return def;
}

DispP Game::buildSayWindow(const Value &ch, const std::string &who, const std::string &what,
                           std::shared_ptr<TextDisp> &whatText) {
    Value whoArgs = charAttr(ch, "who_args"), whatArgs = charAttr(ch, "what_args"),
          windowArgs = charAttr(ch, "window_args");
    TextStyle whatSt = textStyleFor(styleNameOf(whatArgs, "say_dialogue"));
    applyTextOverrides(whatSt, whatArgs);
    whatText = makeText(what, whatSt);
    std::vector<DispP> kids;
    if (!who.empty()) {
        TextStyle whoSt = textStyleFor(styleNameOf(whoArgs, "say_label"));
        applyTextOverrides(whoSt, whoArgs);
        kids.push_back(makeText(who, whoSt));
    }
    kids.push_back(whatText);
    auto vbox = makeBox("say_vbox", true, kids);
    return makeWindow(styleNameOf(windowArgs, "say_window"), vbox, windowArgs);
}

DispP Game::buildNvlWindow(std::shared_ptr<TextDisp> &lastWhat) {
    std::vector<DispP> entries;
    lastWhat = nullptr;
    if (ListObj *nl = storeGet("nvl_list").list()) {
        for (auto &e : nl->v) {
            ListObj *el = e.list();
            if (!el || el->v.size() < 3)
                continue;
            Value who = el->v[0], what = el->v[1], kw = el->v[2];
            Value whoArgs, whatArgs, windowArgs;
            if (DictObj *kd = kw.dict()) {
                if (const Value *v = kd->find(Value::str("who_args")))
                    whoArgs = *v;
                if (const Value *v = kd->find(Value::str("what_args")))
                    whatArgs = *v;
                if (const Value *v = kd->find(Value::str("window_args")))
                    windowArgs = *v;
            }
            TextStyle whatSt = textStyleFor(styleNameOf(whatArgs, "nvl_dialogue"));
            applyTextOverrides(whatSt, whatArgs);
            auto whatT = makeText(what.s(), whatSt);
            std::vector<DispP> row;
            if (who.isStr() && !who.s().empty()) {
                TextStyle whoSt = textStyleFor(styleNameOf(whoArgs, "nvl_label"));
                applyTextOverrides(whoSt, whoArgs);
                row.push_back(makeText(who.s(), whoSt));
            }
            row.push_back(whatT);
            auto box = makeBox("say_vbox", false, row);
            entries.push_back(makeWindow(styleNameOf(windowArgs, "nvl_entry"), box));
            lastWhat = whatT;
        }
    }
    auto vbox = makeBox("nvl_vbox", true, entries);
    return makeWindow("nvl_window", vbox);
}

static bool isNvl(const Value &ch) {
    InstObj *in = ch.inst();
    if (!in)
        return false;
    Value mode = in->get("mode");
    return mode.isStr() && mode.s() == "nvl";
}

void Game::nvlShow(const Value &trans, bool hideIt) {
    std::shared_ptr<TextDisp> last;
    DispP w = buildNvlWindow(last);
    scene.add("transient", w, "", 0, {}, {}, "", false);
    scene.shownWindow = true;
    if (hideIt) {
        withStatement(Value());
        withStatement(trans);
    } else {
        withStatement(trans);
    }
}

struct DialogueTags {
    std::string text;
    std::vector<int> pauseStart{0}, pauseEnd;
    std::vector<double> pauseDelay; // <0 none
    bool noWait = false;
};

static DialogueTags parseDialogueTags(const std::string &s) {
    DialogueTags d;
    int chars = 0;
    std::vector<int> ps{0}, pe;
    std::vector<double> pd;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '{') {
            if (i + 1 < s.size() && s[i + 1] == '{') {
                d.text += "{{";
                chars++;
                i += 2;
                continue;
            }
            size_t e = s.find('}', i);
            if (e == std::string::npos) {
                d.text += s.substr(i);
                break;
            }
            std::string tag = s.substr(i + 1, e - i - 1);
            std::string name = tag.substr(0, tag.find('='));
            double val = tag.find('=') != std::string::npos ? atof(tag.c_str() + tag.find('=') + 1) : -1;
            if (name == "p" || name == "w") {
                ps.push_back(chars);
                pe.push_back(chars);
                pd.push_back(val);
            } else if (name == "nw") {
                d.noWait = true;
            } else if (name == "fast") {
                ps.assign(1, chars);
                pe.clear();
                pd.clear();
                d.noWait = false;
            }
            d.text += s.substr(i, e - i + 1);
            i = e + 1;
            continue;
        }
        size_t st = i;
        utf8Decode(s, i);
        d.text += s.substr(st, i - st);
        chars++;
    }
    pe.push_back(chars);
    pd.push_back(d.noWait ? 0 : -1);
    d.pauseStart = ps;
    d.pauseEnd = pe;
    d.pauseDelay = pd;
    return d;
}

void Game::characterCall(const Value &ch, const std::string &whatIn, bool interactFlag, const CallArgs *extra) {
    InstObj *c = ch.inst();
    if (!c)
        return;
    Value cond = c->get("condition");
    if (cond.isStr() && !cond.s().empty()) {
        // conditions are rare; ignore
    }
    Value displayArgs = c->get("display_args");
    bool interact = interactFlag;
    if (DictObj *da = displayArgs.dict())
        if (const Value *v = da->find(Value::str("interact")))
            interact = interact && v->truthy();
    if (extra)
        for (auto &kw : extra->kw)
            if (kw.first == "interact")
                interact = interact && kw.second.truthy();

    std::string who;
    Value name = c->get("name");
    if (c->get("dynamic").truthy() && name.isStr()) {
        try {
            extern Value evalExpressionString(const std::string &src);
            name = evalExpressionString(name.s());
        } catch (PyError &) {
        }
    }
    if (name.isStr())
        who = valueStr(c->get("who_prefix")) + name.s() + valueStr(c->get("who_suffix"));
    std::string what = valueStr(c->get("what_prefix")) + whatIn + valueStr(c->get("what_suffix"));
    // [[ escapes (substitutions are disabled for KS characters)
    for (size_t p = what.find("[["); p != std::string::npos; p = what.find("[[", p + 1))
        what.erase(p, 1);

    bool nvl = isNvl(ch);
    if (nvl) {
        // do_add
        Value list = storeGet("nvl_list");
        if (!list.list()) {
            list = mkList();
            storeSet("nvl_list", list);
        }
        Value kw = mkDict();
        kw.dict()->set(Value::str("what_args"), c->get("what_args"));
        kw.dict()->set(Value::str("who_args"), c->get("who_args"));
        kw.dict()->set(Value::str("window_args"), c->get("window_args"));
        list.list()->v.push_back(mkTuple({name.isStr() ? Value::str(who) : Value(), Value::str(what), kw}));
    }

    // show function preprocessing (quotefixer / say_wrapper)
    std::string shown = what;
    Value showFn = c->get("show_function");
    std::string sfName;
    if (FuncObj *fo = showFn.func())
        sfName = fo->name.substr(fo->name.rfind('.') + 1);
    try {
        if (sfName == "quotefixer")
            shown = valueStr(PY.call(storeGet("change_quotes"), {Value::str(what)}));
        if (sfName == "quotefixer" || sfName == "say_wrapper")
            PY.call(storeGet("store_current_line"), {name.isStr() ? Value::str(who) : Value(), Value::str(shown)});
    } catch (PyError &e) {
        logf("show function: %s %s", e.type.c_str(), e.msg.c_str());
    }

    // display_say
    bool fast = skipAllowed(*this) && configV.inst()->get("skipping").s() == "fast";
    if (interact && fast) {
        withStatement(Value());
    } else {
        DialogueTags dtt = parseDialogueTags(nvl ? shown : shown);
        bool slow = !afterRollback;
        Value ctcV;
        std::string ctcPos = "nestled";
        if (DictObj *da = displayArgs.dict()) {
            if (const Value *v = da->find(Value::str("ctc")))
                ctcV = *v;
            if (const Value *v = da->find(Value::str("ctc_position")))
                ctcPos = valueStr(*v);
        }
        if (nvl) {
            Value pageCtc = configV.inst()->get("nvl_page_ctc");
            bool clear = c->get("clear").truthy();
            if (!pageCtc.isNone() && clear) {
                ctcV = pageCtc;
                ctcPos = valueStr(configV.inst()->get("nvl_page_ctc_position"));
            }
        }
        size_t segs = interact ? dtt.pauseStart.size() : 1;
        for (size_t seg = 0; seg < segs; seg++) {
            bool lastPause = seg + 1 == segs;
            int segStart = interact ? dtt.pauseStart[seg] : dtt.pauseStart[0];
            int segEnd = interact ? dtt.pauseEnd[seg] : dtt.pauseEnd.back();
            double delay = interact ? dtt.pauseDelay[seg] : dtt.pauseDelay.back();
            auto say = std::make_shared<SayState>();
            std::shared_ptr<TextDisp> whatText;
            DispP window;
            if (nvl) {
                window = buildNvlWindow(whatText);
                if (whatText) {
                    // the last entry is being displayed; earlier ones are complete
                }
            } else {
                window = buildSayWindow(ch, name.isStr() ? who : "", dtt.text, whatText);
            }
            scene.add("transient", window, "", 0, {}, {}, "", false);
            scene.shownWindow = true;
            if (whatText) {
                whatText->reveal = slow ? segStart : segEnd;
                say->what = whatText;
            }
            say->start = segStart;
            say->end = segEnd;
            say->slow = slow;
            say->afmChars = segEnd - segStart;
            bool showCtc = (interact && delay != 0) && !ctcV.isNone();
            if (showCtc && ctcPos == "fixed") {
                DispP ctc = toDisp(ctcV);
                if (ctc)
                    scene.add("transient", ctc, "", 0, {}, {}, "", false);
            }
            if (!interact)
                break;
            Interaction in;
            in.type = IType::Say;
            in.say = say;
            say->startTime = nowSeconds();
            if (delay > 0) {
                double d = delay;
                in.update = [this, say, d]() { return say->slowDone && nowSeconds() - say->startTime >= d; };
            } else if (delay == 0) {
                in.update = [say]() { return say->slowDone; };
            }
            Value r = this->interact(in);
            if (r.t == T::Bool && !r.truthy() && !lastPause)
                break;
            (void)lastPause;
        }
        afterRollback = false;
        if (interact) {
            if (!dtt.noWait)
                checkpoint();
            withNone();
        }
    }

    // do_done: readback buffer + nvl clear
    if (nvl && c->get("clear").truthy())
        storeSet("nvl_list", mkList());
    std::string cls = c->cls;
    if (cls.find("Readback") != std::string::npos) {
        std::string doneWhat = sfName == "quotefixer" ? shown : what;
        try {
            PY.call(storeGet("store_say"), {name.isStr() ? Value::str(who) : Value(), Value::str(doneWhat)});
        } catch (PyError &e) {
            logf("store_say: %s %s", e.type.c_str(), e.msg.c_str());
        }
    }
}

void Game::say(const Value &whoIn, const std::string &whatIn, bool interactFlag) {
    Value who = whoIn;
    if (who.isNone())
        who = storeGet("narrator");
    std::string what = whatIn;
    // config.say_menu_text_filter = remove_hyperlinks
    if (!persistentGet("commentary_on").truthy()) {
        std::string out;
        for (size_t i = 0; i < what.size();) {
            if (what.compare(i, 3, "{a=") == 0) {
                size_t e = what.find('}', i);
                if (e != std::string::npos) {
                    i = e + 1;
                    continue;
                }
            }
            if (what.compare(i, 4, "{/a}") == 0) {
                i += 4;
                continue;
            }
            out += what[i++];
        }
        what = out;
    }
    if (who.isStr()) {
        // a string speaker: use the narrator style with a name
        Value adv = storeGet("adv");
        InstObj *base = adv.inst();
        Value ch = mkInst(base ? base->cls : "ADVCharacter");
        if (base)
            ch.inst()->attrs = base->attrs;
        ch.inst()->attrs["name"] = who;
        who = ch;
    }
    characterCall(who, what, interactFlag, nullptr);
}

// ------------------------------------------------------------------ main loop

void Game::run() {
    extern void runStartup();
    runStartup();
}
