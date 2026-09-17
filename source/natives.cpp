#include "audio.h"
#include "game.h"
#include "input.h"
#include <algorithm>
#include <cmath>
#include <random>

bool playMovie(const std::string &file, bool skippable); // video.cpp
void nativeNotify(const std::string &msg);               // screens.cpp

static Value tupleAt(const Value &t, size_t i) {
    ListObj *l = t.list();
    return (l && i < l->v.size()) ? l->v[i] : Value();
}

static Value kw(const CallArgs &a, const char *name, const Value &def = Value()) {
    for (auto &k : a.kw)
        if (k.first == name)
            return k.second;
    return def;
}

Value evalExpressionString(const std::string &src) {
    // only simple names are supported at runtime
    if (G.store->has(src))
        return G.store->get(src);
    return Value::str(src);
}

void noteSeenAudio(const std::vector<std::string> &files) {
    Value seen = G.persistentGet("_seen_audio");
    if (!seen.dict()) {
        seen = mkDict();
        G.persistentSet("_seen_audio", seen);
    }
    for (auto &f : files)
        seen.dict()->set(Value::str(f), Value::boolean(true));
}

static std::string channelArg(const CallArgs &a, size_t idx, const std::string &def) {
    const Value *v = a.get(idx, "channel");
    if (!v)
        return def;
    if (v->isStr())
        return v->s();
    if (v->isInt())
        return v->i == 0 ? "music" : v->i == 1 ? "sound" : std::to_string(v->i);
    return def;
}

static std::vector<std::string> filesArg(const Value &v) {
    std::vector<std::string> r;
    if (v.isStr())
        r.push_back(v.s());
    else if (ListObj *l = v.list())
        for (auto &x : l->v)
            if (x.isStr())
                r.push_back(x.s());
    return r;
}

static Value mkModule(Interp &I, const std::string &name) { return I.module(name); }

static void def(Value &mod, const std::string &name, NativeFn fn) { mod.inst()->attrs[name] = mkNative(name, std::move(fn)); }

// ------------------------------------------------------------------ menu (ui_ingamemenu.menu / custom_menu)

static Value ksMenu(Interp &I, CallArgs &a) {
    Game &g = G;
    ListObj *items = a.arg(0, "items").list();
    if (!items)
        return Value();
    std::vector<std::pair<std::string, Value>> choices;
    for (auto &it : items->v) {
        Value label = tupleAt(it, 0), val = tupleAt(it, 1);
        if (val.isNone()) {
            // caption: narrator(label, interact=False)
            CallArgs na;
            na.pos.push_back(label);
            na.kw.emplace_back("interact", Value::boolean(false));
            I.call(g.storeGet("narrator"), na);
        } else if (label.truthy()) {
            choices.emplace_back(label.s(), val);
        }
    }
    // custom_menu
    if (!g.configV.inst()->get("skipping").isNone() && !g.prefs.skipAfterChoices)
        g.configV.inst()->attrs["skipping"] = Value();
    std::string location = g.current.valid() ? std::to_string(g.current.script) + ":" + std::to_string(g.current.pc) : "";
    // location key like Ren'Py: the label that contains the menu is not tracked; use the scene label
    Value lastLabel = g.storeGet("last_visited_label");
    std::string locKey = valueStr(lastLabel) + "#" + location;

    std::vector<MenuChoice> mc;
    Value chosen = g.persistentGet("_chosen");
    if (!chosen.dict()) {
        chosen = mkDict();
        g.persistentSet("_chosen", chosen);
    }
    Value chosenSet;
    if (const Value *cs = chosen.dict()->find(Value::str(locKey)))
        chosenSet = *cs;
    else {
        chosenSet = mkSet();
        chosen.dict()->set(Value::str(locKey), chosenSet);
    }
    for (auto &c : choices) {
        MenuChoice m;
        m.label = c.first;
        m.value = c.second;
        m.chosenBefore = chosenSet.dict() && chosenSet.dict()->find(c.second);
        mc.push_back(m);
    }
    // renpy.random.shuffle(menuitems)
    static std::mt19937 rng((unsigned)(nowSeconds() * 1000));
    std::shuffle(mc.begin(), mc.end(), rng);

    extern Value runChoiceMenu(std::vector<MenuChoice> & choices, bool hasCaption);
    bool caption = choices.size() != items->v.size();
    Value rv = runChoiceMenu(mc, caption);
    if (!rv.isNone()) {
        if (DictObj *cs = chosenSet.dict())
            cs->set(rv, Value());
        Value bc = g.persistentGet("breadcrumbs");
        if (ListObj *bl = bc.list())
            bl->v.push_back(mkTuple({Value::str(locKey), rv}));
        for (auto &it : items->v)
            if (valueEq(tupleAt(it, 1), rv))
                I.call(g.storeGet("store_say"), {Value(), Value::str(">> " + tupleAt(it, 0).s())});
    }
    g.checkpoint();
    g.withNone();
    return rv;
}

// ------------------------------------------------------------------ readback helpers

static std::string preparse(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '{') {
            size_t e = s.find('}', i);
            if (e != std::string::npos) {
                std::string tag = s.substr(i + 1, e - i - 1);
                std::string name = tag.substr(0, tag.find('='));
                if (name == "nw" || name == "image" || name == "/image" || name == "color" || name == "/color" ||
                    name == "a" || name == "/a" || name == "font" || name == "/font" || name == "size" ||
                    name == "/size") {
                    i = e + 1;
                    continue;
                }
            }
        }
        if (s[i] == '\n') {
            i++;
            continue;
        }
        out += s[i++];
    }
    return out;
}

static Value storeSay(Interp &, CallArgs &a) {
    Game &g = G;
    Value whoV = a.arg(0, "who"), whatV = a.arg(1, "what");
    std::string narratorName = valueStr(g.storeGet("NARRATOR_NAME"));
    std::string who = whoV.isStr() ? whoV.s() : "";
    if (who == narratorName || who == preparse(narratorName))
        who = "";
    std::string what = whatV.isStr() ? whatV.s() : "";
    auto trim = [](const std::string &s) {
        size_t b = s.find_first_not_of(" \t\n"), e = s.find_last_not_of(" \t\n");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };
    if (trim(who).empty() && (trim(what).empty() || preparse(what).empty()))
        return Value();
    Value buf = g.storeGet("readback_buffer");
    if (!buf.list()) {
        buf = mkList();
        g.storeSet("readback_buffer", buf);
    }
    buf.list()->v.push_back(mkTuple({Value::str(preparse(who)), Value::str(preparse(what))}));
    auto &v = buf.list()->v;
    while (v.size() > 256)
        v.erase(v.begin());
    return Value();
}

// ------------------------------------------------------------------ written_note / doublespeak

// ui.grid(2, 1, xfill=True)
struct FixedDisp2 : Displayable {
    DispP left, right;
    RenderP render(float w, float h, double st, double at) override {
        extern RenderP renderChild(const DispP &d, float w, float h, double st, double at);
        float cw = w / 2;
        RenderP l = renderChild(left, cw, h, st, at), r = renderChild(right, cw, h, st, at);
        float rh = std::max(l->h, r->h);
        auto rv = Render::make(w, rh);
        left->place(*rv, 0, 0, cw, rh, l);
        right->place(*rv, cw, 0, cw, rh, r);
        return rv;
    }
    void children(std::vector<DispP> &out) override {
        out.push_back(left);
        out.push_back(right);
    }
};

static Value writtenNote(Interp &I, CallArgs &a) {
    Game &g = G;
    std::string text = argStr(a, 0, "text");
    Value windowArgs = a.arg(1, "window_args"), textArgs = a.arg(2, "text_args");
    bool quiet = argBool(a, 3, "quiet", false);
    g.storeSet("current_line", Value());
    I.call(g.storeGet("store_say"), {g.displayString("text_history_note"), Value::str([&] {
                                         std::string t = text;
                                         for (size_t p = t.find("\n\n"); p != std::string::npos; p = t.find("\n\n", p))
                                             t.replace(p, 2, "\n");
                                         return t;
                                     }())});
    if (!quiet)
        audio::play("sound", valueStr(g.storeGet("sfx_paper")), 0, 0, 0);

    auto buildNote = [&]() -> DispP {
        TextStyle st = textStyleFor("note_text");
        Value overrides = g.displayString("styleoverrides");
        applyTextOverrides(st, overrides);
        applyTextOverrides(st, textArgs);
        TextStyle empty = textStyleFor("note_text");
        std::vector<DispP> kids = {makeText("", empty), makeText(text, st), makeText("", empty)};
        auto vbox = makeBox("vbox", true, kids);
        return makeWindow("note_window", vbox, windowArgs);
    };
    auto showNote = [&](const Value &transform) {
        DispP note = applyTransform(transform, buildNote());
        g.scene.add("transient", note, "", 0, {}, {}, "", false);
        g.scene.shownWindow = true;
    };
    // entering
    showNote(g.storeGet("note_enter"));
    Value centered = g.storeGet("centered");
    Value ctc;
    if (InstObj *ci = centered.inst())
        if (DictObj *da = ci->get("display_args").dict())
            if (const Value *c = da->find(Value::str("ctc")))
                ctc = *c;
    if (!ctc.isNone())
        g.scene.add("transient", toDisp(ctc), "", 0, {}, {}, "", false);
    Interaction in;
    in.type = IType::Say;
    in.say = std::make_shared<SayState>();
    in.say->slowDone = true;
    g.interact(in);
    g.checkpoint();
    // leaving
    showNote(g.storeGet("note_exit"));
    Interaction p;
    p.type = IType::Pause;
    p.delay = 0.5;
    g.scene.shownWindow = true;
    g.interact(p);
    return Value();
}

static Value doublespeak(Interp &I, CallArgs &a) {
    Game &g = G;
    Value c0 = a.arg(0, "char0"), c1 = a.arg(1, "char1");
    std::string m0 = argStr(a, 2, "msg0");
    Value m1v = a.arg(3, "msg1", Value::boolean(false));
    auto speaker = [&](const Value &ch) {
        InstObj *in = ch.inst();
        if (!in)
            return valueStr(ch);
        std::string name = valueStr(in->get("name"));
        if (DictObj *wa = in->get("who_args").dict())
            if (const Value *col = wa->find(Value::str("color")))
                return "{color=" + col->s() + "}" + name + "{/color}";
        return name;
    };
    std::string s0 = speaker(c0), s1 = speaker(c1);
    std::string msg0 = valueStr(c0.inst() ? c0.inst()->get("what_prefix") : Value()) + m0 +
                       valueStr(c0.inst() ? c0.inst()->get("what_suffix") : Value());
    std::string msg1;
    if (!m1v.truthy())
        msg1 = msg0;
    else
        msg1 = valueStr(c1.inst() ? c1.inst()->get("what_prefix") : Value()) + m1v.s() +
               valueStr(c1.inst() ? c1.inst()->get("what_suffix") : Value());
    g.storeSet("current_line", Value());
    if (msg0 == msg1) {
        I.call(g.storeGet("store_say"), {Value::str(s0 + " & " + s1), Value::str(msg0)});
    } else {
        I.call(g.storeGet("store_say"), {Value::str(s0), Value::str(msg0)});
        I.call(g.storeGet("store_say"), {Value::str(s1), Value::str(msg1)});
    }
    Value overrides = g.displayString("styleoverrides");
    auto column = [&](const std::string &who, const std::string &what, std::shared_ptr<TextDisp> &whatT) {
        TextStyle ls = textStyleFor("say_label");
        applyTextOverrides(ls, overrides);
        TextStyle ds = textStyleFor("say_dialogue");
        applyTextOverrides(ds, overrides);
        ds.xmaximum = Value::integer(350);
        whatT = makeText(what, ds);
        return makeBox("say_vbox", true, {makeText(who, ls), whatT});
    };
    std::shared_ptr<TextDisp> t0, t1;
    DispP col0 = column(s0, msg0, t0), col1 = column(s1, msg1, t1);
    col1->style.place.xpos = Pos::absolute(20);
    // grid(2,1,xfill=True): two equal columns
    auto grid = std::make_shared<FixedDisp2>();
    grid->left = col0;
    grid->right = col1;
    Value wargs = mkDict();
    wargs.dict()->set(Value::str("background"), Value::str("ui/bg-doublespeak.png"));
    wargs.dict()->set(Value::str("yalign"), Value::real(1.0));
    wargs.dict()->set(Value::str("yminimum"), Value::integer(160));
    DispP window = makeWindow("say_window", grid, wargs);
    g.scene.add("transient", window, "", 0, {}, {}, "", false);
    g.scene.shownWindow = true;
    auto say = std::make_shared<SayState>();
    say->what = t0;
    say->start = 0;
    say->end = -1;
    t0->reveal = 0;
    t1->reveal = 0;
    say->startTime = nowSeconds();
    Interaction in;
    in.type = IType::Say;
    in.say = say;
    in.update = [t0, t1, say]() {
        t1->reveal = t0->reveal;
        return false;
    };
    g.interact(in);
    g.checkpoint();
    return Value();
}

// ------------------------------------------------------------------ displayable constructors

static Value styleProps(const CallArgs &a, size_t skipPos) {
    (void)skipPos;
    Value d = mkDict();
    for (auto &k : a.kw)
        d.dict()->set(Value::str(k.first), k.second);
    return d;
}

static Value textNative(Interp &, CallArgs &a) {
    Value t = a.arg(0, "text");
    std::string s;
    if (ListObj *l = t.list()) {
        for (auto &x : l->v)
            s += valueStr(x);
    } else {
        s = valueStr(t);
    }
    Value props = styleProps(a, 1);
    std::string styleName = "default";
    if (const Value *sv = props.dict()->find(Value::str("style")))
        if (sv->isStr())
            styleName = sv->s();
    TextStyle st = textStyleFor(styleName);
    applyTextOverrides(st, props);
    return dispValue(makeText(s, st), "renpy.text.text.Text#native");
}

static Value liveComposite(Interp &, CallArgs &a) {
    Value size = a.arg(0, "size");
    float w = (float)tupleAt(size, 0).num(), h = (float)tupleAt(size, 1).num();
    std::vector<std::pair<std::pair<float, float>, DispP>> kids;
    for (size_t i = 1; i + 1 < a.pos.size(); i += 2)
        kids.push_back({{(float)tupleAt(a.pos[i], 0).num(), (float)tupleAt(a.pos[i], 1).num()}, toDisp(a.pos[i + 1])});
    return dispValue(makeFixedSize(w, h, kids));
}

static Value imInst(const std::string &cls, std::initializer_list<std::pair<const char *, Value>> fields) {
    Value v = mkInst("renpy.display.im." + cls);
    for (auto &f : fields)
        v.inst()->attrs[f.first] = f.second;
    return v;
}

static Value imageArg(const Value &v) {
    if (v.isStr())
        return imInst("Image", {{"filename", v}});
    return v;
}

static Value matrixValue(const std::vector<double> &m) {
    std::vector<Value> vals;
    for (double x : m)
        vals.push_back(Value::real(x));
    Value t = mkInst("renpy.display.im.matrix");
    t.inst()->attrs["m"] = mkTuple(vals);
    return t;
}

static std::vector<double> matrixOf(const Value &v) {
    std::vector<double> m;
    Value src = v;
    if (InstObj *in = v.inst())
        src = in->get("m");
    if (ListObj *l = src.list())
        for (auto &x : l->v)
            m.push_back(x.num());
    if (m.size() == 20)
        m.insert(m.end(), {0, 0, 0, 0, 1});
    if (m.size() != 25)
        m.assign({1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1});
    return m;
}

static void registerIm(Interp &I) {
    Value im = I.module("im");
    def(im, "Image", [](Interp &, CallArgs &a) { return imInst("Image", {{"filename", a.arg(0, "filename")}}); });
    def(im, "Crop", [](Interp &, CallArgs &a) {
        Value x = a.arg(1, "x");
        if (ListObj *l = x.list())
            return imInst("Crop", {{"image", imageArg(a.arg(0, "im"))}, {"x", l->v[0]}, {"y", l->v[1]}, {"w", l->v[2]}, {"h", l->v[3]}});
        return imInst("Crop", {{"image", imageArg(a.arg(0, "im"))}, {"x", x}, {"y", a.arg(2, "y")}, {"w", a.arg(3, "w")}, {"h", a.arg(4, "h")}});
    });
    def(im, "Composite", [](Interp &, CallArgs &a) {
        std::vector<Value> positions, images;
        for (size_t i = 1; i + 1 < a.pos.size(); i += 2) {
            positions.push_back(a.pos[i]);
            images.push_back(imageArg(a.pos[i + 1]));
        }
        return imInst("Composite", {{"size", a.arg(0, "size")}, {"positions", mkTuple(positions)}, {"images", mkList(images)}});
    });
    def(im, "Scale", [](Interp &, CallArgs &a) {
        return imInst("Scale", {{"image", imageArg(a.arg(0, "im"))}, {"width", a.arg(1, "width")}, {"height", a.arg(2, "height")}});
    });
    def(im, "FactorScale", [](Interp &, CallArgs &a) {
        Value w = a.arg(1, "width");
        return imInst("FactorScale", {{"image", imageArg(a.arg(0, "im"))}, {"width", w}, {"height", a.arg(2, "height", w)}});
    });
    def(im, "MatrixColor", [](Interp &, CallArgs &a) {
        std::vector<Value> m;
        for (double x : matrixOf(a.arg(1, "matrix")))
            m.push_back(Value::real(x));
        return imInst("MatrixColor", {{"image", imageArg(a.arg(0, "im"))}, {"matrix", mkTuple(m)}});
    });
    def(im, "Alpha", [](Interp &, CallArgs &a) {
        double al = argNum(a, 1, "alpha", 1);
        return imInst("Recolor", {{"image", imageArg(a.arg(0, "image"))}, {"rmul", Value::integer(256)}, {"gmul", Value::integer(256)}, {"bmul", Value::integer(256)}, {"amul", Value::integer((int64_t)(255 * al) + 1)}});
    });
    def(im, "Grayscale", [](Interp &, CallArgs &a) {
        std::vector<Value> m;
        double r = 0.2126, g = 0.7152, b = 0.0722;
        for (double x : std::vector<double>{r, g, b, 0, 0, r, g, b, 0, 0, r, g, b, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1})
            m.push_back(Value::real(x));
        return imInst("MatrixColor", {{"image", imageArg(a.arg(0, "im"))}, {"matrix", mkTuple(m)}});
    });
    def(im, "Tile", [](Interp &, CallArgs &a) { return imInst("Tile", {{"image", imageArg(a.arg(0, "im"))}, {"size", a.arg(1, "size")}}); });
    def(im, "Flip", [](Interp &, CallArgs &a) {
        return imInst("Flip", {{"image", imageArg(a.arg(0, "im"))}, {"horizontal", kw(a, "horizontal")}, {"vertical", kw(a, "vertical")}});
    });
    def(im, "Rotozoom", [](Interp &, CallArgs &a) {
        return imInst("Rotozoom", {{"image", imageArg(a.arg(0, "im"))}, {"angle", a.arg(1, "angle")}, {"zoom", a.arg(2, "zoom")}});
    });
    def(im, "image", [](Interp &, CallArgs &a) { return imageArg(a.arg(0, nullptr)); });
    // im.matrix helpers
    Value matrix = mkInst("#module");
    im.inst()->attrs["matrix"] = matrix;
    auto M = [](std::vector<double> v) { return matrixValue(v); };
    def(matrix, "identity", [M](Interp &, CallArgs &) { return M({1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1}); });
    def(matrix, "tint", [M](Interp &, CallArgs &a) {
        double r = argNum(a, 0, "r", 1), g = argNum(a, 1, "g", 1), b = argNum(a, 2, "b", 1);
        return M({r, 0, 0, 0, 0, 0, g, 0, 0, 0, 0, 0, b, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1});
    });
    def(matrix, "brightness", [M](Interp &, CallArgs &a) {
        double b = argNum(a, 0, "b", 0);
        return M({1, 0, 0, 0, b, 0, 1, 0, 0, b, 0, 0, 1, 0, b, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1});
    });
    def(matrix, "opacity", [M](Interp &, CallArgs &a) {
        double o = argNum(a, 0, "o", 1);
        return M({1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, o, 0, 0, 0, 0, 0, 1});
    });
    auto sat = [M](double level) {
        double r = 0.2126, g = 0.7152, b = 0.0722;
        auto I = [&](double x, double y) { return x * (1 - level) + y * level; };
        return M({I(r, 1), I(g, 0), I(b, 0), 0, 0, I(r, 0), I(g, 1), I(b, 0), 0, 0, I(r, 0), I(g, 0), I(b, 1), 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1});
    };
    def(matrix, "saturation", [sat](Interp &, CallArgs &a) { return sat(argNum(a, 0, "level", 1)); });
    def(matrix, "desaturate", [sat](Interp &, CallArgs &) { return sat(0); });
    def(matrix, "colorize", [M](Interp &, CallArgs &a) {
        auto col = [](const Value &v, double &r, double &g, double &b) {
            Color c = hexColor(v.isStr() ? v.s().c_str() : "#000");
            r = c.r;
            g = c.g;
            b = c.b;
        };
        double r0, g0, b0, r1, g1, b1;
        col(a.arg(0, nullptr), r0, g0, b0);
        col(a.arg(1, nullptr), r1, g1, b1);
        return M({r1 - r0, 0, 0, 0, r0, 0, g1 - g0, 0, 0, g0, 0, 0, b1 - b0, 0, b0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1});
    });
    I.methods["renpy.display.im.matrix"]["__mul__"] = [M](Interp &, CallArgs &a) {
        std::vector<double> x = matrixOf(a.pos[0]), y = matrixOf(a.pos[1]), r(25, 0);
        if (!a.pos[1].inst()) {
            for (size_t i = 0; i < 25; i++)
                r[i] = x[i] * a.pos[1].num();
            return M(r);
        }
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 5; j++)
                for (int k = 0; k < 5; k++)
                    r[i * 5 + j] += x[i * 5 + k] * y[k * 5 + j];
        return M(r);
    };
    I.methods["renpy.display.im.matrix"]["__getitem__"] = [](Interp &, CallArgs &a) {
        return Value::real(matrixOf(a.pos[0])[(size_t)a.pos[1].asInt() % 25]);
    };
    I.methods["renpy.display.im.matrix"]["__len__"] = [](Interp &, CallArgs &) { return Value::integer(25); };
    G.storeSet("im", im);
}

// ------------------------------------------------------------------ renpy module

static Value pauseNative(Interp &, CallArgs &a) {
    Game &g = G;
    Value delay = a.arg(0, "delay");
    bool hard = argBool(a, 1000, "hard", false);
    if (!g.configV.inst()->get("skipping").isNone() && g.configV.inst()->get("skipping").s() == "fast")
        return Value::boolean(false);
    Interaction in;
    in.type = IType::Pause;
    in.delay = delay.isNum() ? delay.num() : -1;
    in.hard = hard;
    Value r = g.interact(in);
    if (argBool(a, 1000, "checkpoint", true))
        g.checkpoint();
    g.withNone();
    return r;
}

void registerGameNatives(Interp &I) {
    Game &g = G;
    Value renpy = I.module("renpy");
    g.storeSet("renpy", renpy);
    renpy.inst()->attrs["store"] = g.storeV;
    renpy.inst()->attrs["random"] = I.module("random");
    renpy.inst()->attrs["windows"] = Value::boolean(false);
    renpy.inst()->attrs["macintosh"] = Value::boolean(false);
    renpy.inst()->attrs["linux"] = Value::boolean(false);
    renpy.inst()->attrs["android"] = Value::boolean(false);

    def(renpy, "version", [](Interp &, CallArgs &a) {
        if (argBool(a, 0, "tuple", false))
            return mkTuple({Value::integer(6), Value::integer(16), Value::integer(5), Value::integer(525)});
        return Value::str("Ren'Py 6.16.5.525");
    });
    def(renpy, "pause", pauseNative);
    def(renpy, "with_statement", [](Interp &, CallArgs &a) { return Value::boolean(G.withStatement(a.arg(0, "trans"))); });
    def(renpy, "transition", [](Interp &, CallArgs &a) {
        G.pendingTransition = a.arg(0, "trans");
        return Value();
    });
    def(renpy, "jump", [](Interp &, CallArgs &a) -> Value { throw JumpSignal(argStr(a, 0, "label")); });
    def(renpy, "jump_out_of_context", [](Interp &, CallArgs &a) -> Value {
        G.inMainMenuContext = false;
        throw JumpSignal(argStr(a, 0, "label"));
    });
    def(renpy, "call", [](Interp &, CallArgs &a) -> Value {
        std::vector<Value> args(a.pos.begin() + std::min<size_t>(1, a.pos.size()), a.pos.end());
        G.callLabel(argStr(a, 0, "label"), args, a.kw);
        throw JumpSignal("");
    });
    def(renpy, "has_label", [](Interp &, CallArgs &a) { return Value::boolean(G.hasLabel(valueStr(a.arg(0, "name")))); });
    def(renpy, "loadable", [](Interp &, CallArgs &a) {
        std::string f = argStr(a, 0, "filename");
        if (f.size() > 4 && f.substr(f.size() - 4) == ".mkv")
            f = f.substr(0, f.size() - 4) + ".mpg";
        return Value::boolean(texture::exists(f) || fileExists(romfsPath("game/" + f)));
    });
    renpy.inst()->attrs["exists"] = renpy.inst()->attrs["loadable"];
    def(renpy, "full_restart", [](Interp &, CallArgs &a) -> Value {
        FullRestartSignal s;
        std::string label = argStr(a, 1000, "label", "");
        if (label == "invoke_scene_select")
            s.target = "scene_select";
        throw s;
    });
    def(renpy, "quit", [](Interp &, CallArgs &) -> Value { throw QuitSignal(); });
    def(renpy, "scene", [](Interp &, CallArgs &a) {
        G.sceneClear(argStr(a, 0, "layer", "master"));
        return Value();
    });
    def(renpy, "show", [](Interp &, CallArgs &a) {
        std::vector<Value> at;
        if (ListObj *l = kw(a, "at_list").list())
            at = l->v;
        DispP what;
        Value w = kw(a, "what");
        if (!w.isNone())
            what = toDisp(w);
        std::vector<std::string> behind;
        if (ListObj *b = kw(a, "behind").list())
            for (auto &x : b->v)
                behind.push_back(x.s());
        G.showName(valueStr(a.arg(0, "name")), at, valueStr(kw(a, "layer", Value::str("master"))),
                   kw(a, "tag").isStr() ? kw(a, "tag").s() : "", (int)kw(a, "zorder", Value::integer(0)).asInt(), behind,
                   what, Value());
        return Value();
    });
    def(renpy, "hide", [](Interp &, CallArgs &a) {
        std::string name = valueStr(a.arg(0, "name"));
        G.hide(name.substr(0, name.find(' ')), valueStr(kw(a, "layer", Value::str("master"))));
        return Value();
    });
    def(renpy, "showing", [](Interp &, CallArgs &a) {
        std::string name = valueStr(a.arg(0, "name"));
        return Value::boolean(G.scene.showing(valueStr(kw(a, "layer", Value::str("master"))), name.substr(0, name.find(' '))));
    });
    def(renpy, "image", [](Interp &, CallArgs &a) {
        Value n = a.arg(0, "name");
        std::string name;
        if (ListObj *l = n.list()) {
            for (auto &x : l->v)
                name += (name.empty() ? "" : " ") + valueStr(x);
        } else {
            name = valueStr(n);
        }
        registerImage(name, a.arg(1, "d"));
        return Value();
    });
    def(renpy, "block_rollback", [](Interp &, CallArgs &) {
        G.rollbackLog.clear();
        return Value();
    });
    def(renpy, "checkpoint", [](Interp &, CallArgs &) {
        G.checkpoint();
        return Value();
    });
    def(renpy, "restart_interaction", [](Interp &, CallArgs &) { return Value(); });
    def(renpy, "shown_window", [](Interp &, CallArgs &) {
        G.scene.shownWindow = true;
        return Value();
    });
    def(renpy, "in_rollback", [](Interp &, CallArgs &) { return Value::boolean(false); });
    def(renpy, "roll_forward_info", [](Interp &, CallArgs &) { return Value(); });
    def(renpy, "mode", [](Interp &, CallArgs &) { return Value(); });
    def(renpy, "get_game_runtime", [](Interp &, CallArgs &) { return Value::real(G.playTime); });
    def(renpy, "clear_game_runtime", [](Interp &, CallArgs &) {
        G.playTime = 0;
        return Value();
    });
    def(renpy, "notify", [](Interp &, CallArgs &a) {
        nativeNotify(argStr(a, 0, "message"));
        return Value();
    });
    def(renpy, "seen_audio", [](Interp &, CallArgs &a) {
        Value seen = G.persistentGet("_seen_audio");
        return Value::boolean(seen.dict() && seen.dict()->find(a.arg(0, "filename")));
    });
    def(renpy, "seen_label", [](Interp &, CallArgs &a) {
        Value seen = G.persistentGet("_seen_labels");
        return Value::boolean(seen.dict() && seen.dict()->find(a.arg(0, "label")));
    });
    def(renpy, "movie_cutscene", [](Interp &, CallArgs &a) {
        std::string f = argStr(a, 0, "filename");
        return Value::boolean(playMovie(f, true));
    });
    def(renpy, "call_in_new_context", [](Interp &, CallArgs &a) {
        std::string label = argStr(a, 0, "label");
        if (label == "_main_menu")
            G.pendingNativeScreen = argStr(a, 1, nullptr, "");
        return Value();
    });
    def(renpy, "invoke_in_new_context", [](Interp &I2, CallArgs &a) {
        std::vector<Value> rest(a.pos.begin() + std::min<size_t>(1, a.pos.size()), a.pos.end());
        CallArgs b;
        b.pos = rest;
        b.kw = a.kw;
        return I2.call(a.arg(0, nullptr), b);
    });
    def(renpy, "context", [](Interp &, CallArgs &) {
        Value c = mkInst("context");
        c.inst()->attrs["_main_menu"] = Value::boolean(G.inMainMenuContext);
        c.inst()->attrs["main_menu"] = Value::boolean(G.inMainMenuContext);
        return c;
    });
    def(renpy, "list_saved_games", [](Interp &, CallArgs &) {
        extern Value listSavedGames();
        return listSavedGames();
    });
    def(renpy, "easy_displayable", [](Interp &, CallArgs &a) { return dispValue(toDisp(a.arg(0, "d"))); });
    def(renpy, "get_placement", [](Interp &, CallArgs &) { return Value(); });
    def(renpy, "free_memory", [](Interp &, CallArgs &) { return Value(); });
    def(renpy, "curry", [](Interp &I2, CallArgs &a) {
        Value c = mkInst("renpy.curry.Curry");
        c.inst()->attrs["callable"] = a.arg(0, nullptr);
        c.inst()->attrs["args"] = mkTuple({});
        c.inst()->attrs["kwargs"] = mkDict();
        (void)I2;
        return c;
    });

    // renpy.music / renpy.sound
    Value music = I.module("renpy.music");
    Value sound = I.module("renpy.sound");
    auto playFn = [](const std::string &defChannel) {
        return [defChannel](Interp &, CallArgs &a) {
            std::string ch = channelArg(a, 1000, defChannel);
            Value loop = kw(a, "loop");
            Value fo = kw(a, "fadeout");
            auto files = filesArg(a.arg(0, "filenames"));
            audio::play(ch, files, kw(a, "fadein", Value::real(0)).num(), fo.isNum() ? fo.num() : 0,
                        loop.isNone() ? -1 : (loop.truthy() ? 1 : 0), kw(a, "if_changed").truthy());
            noteSeenAudio(files);
            return Value();
        };
    };
    def(music, "play", playFn("music"));
    def(sound, "play", playFn("sound"));
    auto queueFn = [](const std::string &defChannel) {
        return [defChannel](Interp &, CallArgs &a) {
            std::string ch = channelArg(a, 1000, defChannel);
            Value loop = kw(a, "loop");
            audio::queue(ch, filesArg(a.arg(0, "filenames")), loop.isNone() ? -1 : (loop.truthy() ? 1 : 0),
                         kw(a, "clear_queue", Value::boolean(true)).truthy());
            return Value();
        };
    };
    def(music, "queue", queueFn("music"));
    def(sound, "queue", queueFn("sound"));
    auto stopFn = [](const std::string &defChannel) {
        return [defChannel](Interp &, CallArgs &a) {
            std::string ch = channelArg(a, 0, defChannel);
            if (a.pos.size() > 0 && !a.pos[0].isStr())
                ch = defChannel;
            Value fo = kw(a, "fadeout");
            audio::stop(ch, fo.isNum() ? fo.num() : 0);
            return Value();
        };
    };
    def(music, "stop", stopFn("music"));
    def(sound, "stop", stopFn("sound"));
    def(music, "set_volume", [](Interp &, CallArgs &a) {
        audio::setChannelVolume(channelArg(a, 2, "music"), argNum(a, 0, "volume", 1), argNum(a, 1, "delay", 0));
        return Value();
    });
    def(sound, "set_volume", [](Interp &, CallArgs &a) {
        audio::setChannelVolume(channelArg(a, 2, "sound"), argNum(a, 0, "volume", 1), argNum(a, 1, "delay", 0));
        return Value();
    });
    def(music, "get_playing", [](Interp &, CallArgs &a) {
        std::string p = audio::playing(channelArg(a, 0, "music"));
        return p.empty() ? Value() : Value::str(p);
    });
    def(sound, "get_playing", [](Interp &, CallArgs &a) {
        std::string p = audio::playing(channelArg(a, 0, "sound"));
        return p.empty() ? Value() : Value::str(p);
    });
    def(music, "register_channel", [](Interp &, CallArgs &a) {
        audio::registerChannel(argStr(a, 0, "name"), argStr(a, 1, "mixer", "music"), argBool(a, 2, "loop", false),
                               argBool(a, 1000, "tight", false));
        return Value();
    });
    def(music, "channel_defined", [](Interp &, CallArgs &a) { return Value::boolean(audio::hasChannel(argStr(a, 0, "channel"))); });
    renpy.inst()->attrs["music"] = music;
    renpy.inst()->attrs["sound"] = sound;
    Value audioMod = I.module("renpy.audio.audio");
    def(audioMod, "set_force_stop", [](Interp &, CallArgs &a) {
        audio::setForceStop(argStr(a, 0, "name"), argBool(a, 1, "value", false));
        return Value();
    });

    // renpy.display.image.images membership: ("completionbonus_fr",) in renpy.display.image.images
    Value imagesMod = I.module("renpy.display.image");
    Value imagesProxy = mkInst("#images");
    imagesMod.inst()->attrs["images"] = imagesProxy;
    I.methods["#images"]["__contains__"] = [](Interp &, CallArgs &a) {
        std::string name;
        if (ListObj *l = a.pos[1].list())
            for (auto &x : l->v)
                name += (name.empty() ? "" : " ") + valueStr(x);
        return Value::boolean(hasImage(name));
    };

    // Character objects: who(what, interact=True)
    auto charCall = [](Interp &, CallArgs &a) {
        Value self = a.pos[0];
        std::string what = a.pos.size() > 1 ? valueStr(a.pos[1]) : "";
        bool interact = true;
        for (auto &k : a.kw)
            if (k.first == "interact")
                interact = k.second.truthy();
        G.characterCall(self, what, interact, nullptr);
        return Value();
    };
    for (const char *cls : {"store.ReadbackADVCharacter", "store.ReadbackNVLCharacter", "store.NVLCharacter",
                            "renpy.character.ADVCharacter", "ADVCharacter"})
        I.instCall[cls] = charCall;

    // persistent: missing attributes are None
    I.methods["persistent"]["__getattr__"] = [](Interp &, CallArgs &) { return Value(); };
    I.methods["config"]["__getattr__"] = [](Interp &, CallArgs &) { return Value(); };
    Value mp = mkInst("multipersistent");
    I.methods["multipersistent"]["__getattr__"] = [](Interp &, CallArgs &) { return Value(); };
    I.methods["multipersistent"]["save"] = [](Interp &, CallArgs &) { return Value(); };
    g.storeSet("multipersistent", mp);

    // _preferences
    InstObj *pr = g.preferencesV.inst();
    pr->attrs["afm_time"] = Value::integer(0);
    pr->attrs["text_cps"] = Value::integer(70);
    pr->attrs["skip_unseen"] = Value::boolean(false);
    pr->attrs["skip_after_choices"] = Value::boolean(false);
    pr->attrs["transitions"] = Value::integer(2);
    pr->attrs["fullscreen"] = Value::boolean(false);
    I.methods["preferences"]["get_volume"] = [](Interp &, CallArgs &a) {
        std::string mixer = argStr(a, 1, "mixer");
        return Value::real(mixer == "music" ? G.prefs.musicVolume : G.prefs.sfxVolume);
    };
    I.methods["preferences"]["set_volume"] = [](Interp &, CallArgs &a) {
        std::string mixer = argStr(a, 1, "mixer");
        double v = std::max(0.0, argNum(a, 2, "volume", 1));
        if (mixer == "music")
            G.prefs.musicVolume = v;
        else
            G.prefs.sfxVolume = v;
        audio::setMixerVolume(mixer, v);
        return Value();
    };

    // achievements (Steam) are not available
    Value ach = mkInst("#module");
    def(ach, "grant", [](Interp &, CallArgs &) { return Value(); });
    def(ach, "has", [](Interp &, CallArgs &) { return Value::boolean(false); });
    ach.inst()->attrs["steamapi"] = Value();
    g.storeSet("achievement", ach);

    registerIm(I);

    // ui / layout stubs (native screens replace all ui code)
    Value ui = I.module("ui");
    I.methods["#module"]["__getattr__"] = [](Interp &, CallArgs &a) {
        std::string name = a.pos[1].s();
        return mkNative(name, [name](Interp &, CallArgs &) {
            logf("ui stub called: %s", name.c_str());
            return Value();
        });
    };
    g.storeSet("ui", ui);
    g.storeSet("layout", I.module("layout"));
    g.storeSet("style", I.module("style"));
}

// Store functions replaced by natives after the python defs have run.
void installStoreOverrides(Interp &I) {
    Game &g = G;
    auto S = [&](const std::string &name, NativeFn fn) { g.storeSet(name, mkNative(name, std::move(fn))); };
    S("menu", ksMenu);
    // classes are not part of the dumped store
    g.storeSet("Transform", I.natives["renpy.display.motion.Transform"]);
    g.storeSet("Position", I.natives["renpy.display.motion.Transform"]);
    S("store_say", storeSay);
    S("preparse_say_for_store", [](Interp &, CallArgs &a) {
        Value v = a.arg(0, "input");
        return v.isStr() ? Value::str(preparse(v.s())) : v;
    });
    S("remove_hyperlinks", [](Interp &, CallArgs &a) { return a.arg(0, "input"); });
    S("written_note", writtenNote);
    S("doublespeak", doublespeak);
    S("nvl_clear", [](Interp &, CallArgs &) {
        G.storeSet("nvl_list", mkList());
        return Value();
    });
    S("nvl_show", [](Interp &, CallArgs &a) {
        G.nvlShow(a.arg(0, "with_"), false);
        return Value();
    });
    S("nvl_hide", [](Interp &, CallArgs &a) {
        G.nvlShow(a.arg(0, "with_"), true);
        return Value();
    });
    S("nvl_erase", [](Interp &, CallArgs &) {
        if (ListObj *l = G.storeGet("nvl_list").list())
            if (!l->v.empty())
                l->v.pop_back();
        return Value();
    });
    S("switch_language", [](Interp &, CallArgs &a) {
        G.switchLanguage(argStr(a, 0, "target"));
        return Value::boolean(true);
    });
    S("mm_context", [](Interp &, CallArgs &) { return Value::boolean(G.inMainMenuContext); });
    S("is_glrenpy", [](Interp &, CallArgs &) { return Value::boolean(true); });
    S("Text", textNative);
    S("LiveComposite", liveComposite);
    S("Solid", [](Interp &, CallArgs &a) {
        Color c = hexColor(argStr(a, 0, "color", "#000").c_str());
        auto d = makeSolid(c.r, c.g, c.b, c.a);
        for (auto &k : a.kw)
            d->style.apply(k.first, k.second);
        return dispValue(d);
    });
    S("Null", [](Interp &, CallArgs &a) {
        return dispValue(makeNull((float)kw(a, "width", Value::integer(0)).num(), (float)kw(a, "height", Value::integer(0)).num()));
    });
    S("Frame", [](Interp &, CallArgs &a) {
        Value f = mkInst("renpy.display.imagelike.Frame");
        f.inst()->attrs["image"] = imageArg(a.arg(0, "image"));
        f.inst()->attrs["left"] = a.arg(1, "xborder", Value::integer(0));
        f.inst()->attrs["top"] = a.arg(2, "yborder", Value::integer(0));
        f.inst()->attrs["tile"] = kw(a, "tile", Value::boolean(false));
        return f;
    });
    S("Fixed", [](Interp &, CallArgs &a) {
        std::vector<DispP> kids;
        for (auto &p : a.pos)
            kids.push_back(toDisp(p));
        auto d = makeFixed(kids);
        for (auto &k : a.kw)
            d->style.apply(k.first, k.second);
        return dispValue(d);
    });
    auto boxFn = [](bool vertical) {
        return [vertical](Interp &, CallArgs &a) {
            std::vector<DispP> kids;
            for (auto &p : a.pos)
                kids.push_back(toDisp(p));
            Value props = mkDict();
            for (auto &k : a.kw)
                props.dict()->set(Value::str(k.first), k.second);
            return dispValue(makeBox(vertical ? "vbox" : "hbox", vertical, kids, props));
        };
    };
    S("VBox", boxFn(true));
    S("HBox", boxFn(false));
    S("At", [](Interp &, CallArgs &a) {
        DispP d = toDisp(a.arg(0, nullptr));
        for (size_t i = 1; i < a.pos.size(); i++)
            d = applyTransform(a.pos[i], d);
        return dispValue(d);
    });
    // Pause(delay) is a transition factory: return a curry called later with old/new widgets
    S("Pause", [](Interp &I2, CallArgs &a) {
        Value c = mkInst("renpy.curry.Curry");
        c.inst()->attrs["callable"] = I2.natives["renpy.display.transition.NoTransition"];
        c.inst()->attrs["args"] = mkTuple({a.arg(0, "delay")});
        c.inst()->attrs["kwargs"] = mkDict();
        return c;
    });
    S("Movie", [](Interp &, CallArgs &a) {
        extern DispP makeMovieDisp(const CallArgs &a);
        return dispValue(makeMovieDisp(a));
    });
    S("custom_movie_cutscene", [](Interp &, CallArgs &a) {
        return Value::boolean(playMovie(argStr(a, 0, "filename"), true));
    });
    g.storeSet("renpy.movie_cutscene", Value());
    S("ksgallery_unlock", [](Interp &, CallArgs &a) {
        Value n = a.arg(0, "name");
        std::vector<Value> parts;
        if (ListObj *l = n.list()) {
            parts = l->v;
        } else {
            std::string s = valueStr(n);
            size_t p = 0;
            while (p < s.size()) {
                size_t sp = s.find(' ', p);
                if (sp == std::string::npos)
                    sp = s.size();
                if (sp > p)
                    parts.push_back(Value::str(s.substr(p, sp - p)));
                p = sp + 1;
            }
        }
        Value seen = G.persistentGet("_seen_images");
        if (!seen.dict()) {
            seen = mkDict();
            G.persistentSet("_seen_images", seen);
        }
        seen.dict()->set(mkTuple(parts), Value::boolean(true));
        return Value();
    });
    S("datedisplay", [](Interp &, CallArgs &) { return Value(); });
    S("mute_toggle", [](Interp &, CallArgs &) {
        G.prefs.muted = !G.prefs.muted;
        G.persistentSet("is_muted", Value::boolean(G.prefs.muted));
        audio::setMixerVolume("music", G.prefs.muted ? 0 : G.prefs.musicVolume);
        audio::setMixerVolume("sfx", G.prefs.muted ? 0 : G.prefs.sfxVolume);
        return Value();
    });
    (void)I;
}
