#include <set>
#include "audio.h"
#include "scene.h"
#include <algorithm>
#include <cmath>

RenderP renderChild(const DispP &d, float w, float h, double st, double at);

static double num(const Value &v, double def = 0) { return v.isNum() ? v.num() : def; }
static Value tupleAt(const Value &t, size_t i) {
    ListObj *l = t.list();
    return (l && i < l->v.size()) ? l->v[i] : Value();
}

// Applies a color matrix to every texture leaf of a render tree.
static RenderP tintRender(const RenderP &r, const CMat &m) {
    if (!r)
        return r;
    auto c = std::make_shared<Render>(*r);
    if (c->op == Render::TEX) {
        c->cm = m * c->cm;
        return c;
    }
    for (auto &ch : c->children)
        ch.r = tintRender(ch.r, m);
    return c;
}

// ------------------------------------------------------------------ basic transitions

struct DissolveT : TransitionDisp {
    bool alpha = false;
    Value timeWarp;
    RenderP render(float w, float h, double st, double at) override {
        if (st >= delay) {
            events = true;
            return renderChild(newW, w, h, st, at);
        }
        double complete = delay > 0 ? std::min(1.0, st / delay) : 1.0;
        if (!timeWarp.isNone())
            complete = callWarper(timeWarp, complete);
        RenderP bottom = renderChild(oldW, w, h, st, at);
        RenderP top = renderChild(newW, w, h, st, at);
        auto rv = Render::make(std::min(top->w, bottom->w), std::min(top->h, bottom->h));
        rv->op = Render::DISSOLVE;
        rv->opAlpha = alpha;
        rv->complete = (float)complete;
        rv->blit(bottom, 0, 0);
        rv->blit(top, 0, 0);
        return rv;
    }
    void children(std::vector<DispP> &out) override {
        out.push_back(oldW);
        out.push_back(newW);
    }
};

struct ImageDissolveT : TransitionDisp {
    DispP image;
    int ramp = 8;
    bool reverse = false, alpha = false;
    Value timeWarp;
    RenderP render(float w, float h, double st, double at) override {
        if (st >= delay) {
            events = true;
            return renderChild(newW, w, h, st, at);
        }
        RenderP img = renderChild(image, w, h, st, at);
        CMat m;
        double v[20] = {0};
        v[4] = v[9] = v[14] = 1;
        if (!reverse) {
            v[15] = 1;
        } else {
            v[15] = -1;
            v[19] = 1;
        }
        m = CMat::fromList(v, 20);
        img = tintRender(img, m);
        RenderP bottom = renderChild(oldW, w, h, st, at);
        RenderP top = renderChild(newW, w, h, st, at);
        auto rv = Render::make(std::min({bottom->w, top->w, img->w}), std::min({bottom->h, top->h, img->h}));
        double complete = delay > 0 ? st / delay : 1;
        if (!timeWarp.isNone())
            complete = callWarper(timeWarp, complete);
        rv->op = Render::IMAGEDISSOLVE;
        rv->opAlpha = alpha;
        rv->complete = (float)complete;
        rv->ramp = ramp;
        rv->blit(bottom, 0, 0);
        rv->blit(top, 0, 0);
        rv->blit(img, 0, 0);
        return rv;
    }
    void children(std::vector<DispP> &out) override {
        out.push_back(oldW);
        out.push_back(newW);
        out.push_back(image);
    }
};

struct NoTransitionT : TransitionDisp {
    RenderP render(float w, float h, double st, double at) override {
        if (st >= delay)
            events = true;
        return renderChild(newW, w, h, st, at);
    }
    void children(std::vector<DispP> &out) override { out.push_back(newW); }
};

struct MultipleT : TransitionDisp {
    std::vector<std::shared_ptr<TransitionDisp>> transitions;
    RenderP render(float w, float h, double st, double at) override {
        std::shared_ptr<TransitionDisp> trans;
        for (size_t i = 0; i + 1 < transitions.size(); i++) {
            if (transitions[i]->delay > st) {
                trans = transitions[i];
                break;
            }
            st -= transitions[i]->delay;
        }
        if (!trans) {
            trans = transitions.back();
            events = true;
        }
        RenderP surf = renderChild(trans, w, h, st, at);
        auto rv = Render::make(surf->w, surf->h);
        rv->blit(surf, 0, 0);
        return rv;
    }
    void children(std::vector<DispP> &out) override {
        for (auto &t : transitions)
            out.push_back(t);
    }
};

struct CropMoveT : TransitionDisp {
    double startpos[2] = {0, 0}, endpos[2] = {0, 0};
    double startcrop[4] = {0, 0, 0, 1}, endcrop[4] = {0, 0, 1, 1};
    bool topnew = true;
    RenderP render(float w, float h, double st, double at) override {
        double time = delay > 0 ? st / delay : 1;
        if (time >= 1.0) {
            events = true;
            return renderChild(newW, w, h, st, at);
        }
        double scales[4] = {w, h, w, h};
        int crop[4], pos[2];
        for (int i = 0; i < 4; i++)
            crop[i] = (int)(scales[i] * (startcrop[i] * (1 - time) + endcrop[i] * time));
        for (int i = 0; i < 2; i++)
            pos[i] = (int)(scales[i] * (startpos[i] * (1 - time) + endpos[i] * time));
        DispP topD = topnew ? newW : oldW, bottomD = topnew ? oldW : newW;
        RenderP top = renderChild(topD, w, h, st, at);
        RenderP bottom = renderChild(bottomD, w, h, st, at);
        auto rv = Render::make(std::min(bottom->w, w), std::min(bottom->h, h));
        rv->blit(bottom, 0, 0);
        auto ss = Render::make((float)crop[2], (float)crop[3]);
        ss->clipping = true;
        ss->blit(top, (float)-crop[0], (float)-crop[1]);
        rv->blit(ss, (float)pos[0], (float)pos[1]);
        return rv;
    }
    void children(std::vector<DispP> &out) override {
        out.push_back(oldW);
        out.push_back(newW);
    }
};

struct SoundT : TransitionDisp {
    std::string sound, channel = "sound";
    bool played = false;
    RenderP render(float w, float h, double st, double at) override {
        if (!played) {
            played = true;
            audio::play(channel, sound, 0, 0, false);
        }
        if (st >= delay)
            events = true;
        return Render::make(w, h);
    }
};

// ------------------------------------------------------------------ Motion / Move (vpunch, hpunch, move factories)

struct MotionT : TransitionDisp {
    DispP child;
    Value start, end; // tuples (xpos, ypos[, xanchor, yanchor])
    double period = 1;
    bool repeat = false, bounce = false, animTimebase = false;
    Value timeWarp;
    bool havePos = false;
    Placement pos;
    RenderP render(float width, float height, double st, double at) override {
        double t = animTimebase ? at : st;
        if (delay > 0 && t >= delay) {
            t = delay;
            if (repeat)
                t = std::fmod(t, period);
        } else if (repeat) {
            t = std::fmod(t, period);
        } else if (t > period) {
            t = period;
        }
        if (delay > 0 && st >= delay)
            events = true;
        t = period > 0 ? t / period : 1;
        if (!timeWarp.isNone())
            t = callWarper(timeWarp, t);
        if (bounce) {
            t *= 2;
            if (t > 1)
                t = 2 - t;
        }
        RenderP c = renderChild(child, width, height, st, at);
        double sizes[4] = {width, height, c->w, c->h};
        ListObj *sl = start.list(), *el = end.list();
        size_t n = sl ? sl->v.size() : 0;
        double res[4] = {0, 0, 0, 0};
        bool isSet[4] = {false, false, false, false};
        for (size_t i = 0; i < n && i < 4; i++) {
            Value a = sl->v[i], b = el && i < el->v.size() ? el->v[i] : a;
            auto val = [&](const Value &x) -> double {
                if (x.isStr()) {
                    const std::string &s = x.s();
                    double f = (s == "center") ? 0.5 : (s == "bottom" || s == "right") ? 1.0 : 0.0;
                    return f * sizes[i];
                }
                if (x.t == T::Float)
                    return x.f * sizes[i];
                return x.num();
            };
            if (a.isNone() && b.isNone())
                continue;
            res[i] = val(a) + t * (val(b) - val(a));
            isSet[i] = true;
        }
        pos = Placement();
        pos.xpos = Pos::absolute(res[0]);
        pos.ypos = Pos::absolute(res[1]);
        if (n >= 4) {
            pos.xanchor = Pos::absolute(res[2]);
            pos.yanchor = Pos::absolute(res[3]);
        } else {
            pos.xanchor = style.place.xanchor;
            pos.yanchor = style.place.yanchor;
        }
        pos.xoffset = style.place.xoffset;
        pos.yoffset = style.place.yoffset;
        pos.subpixel = true;
        havePos = true;
        auto rv = Render::make(c->w, c->h);
        rv->blit(c, 0, 0);
        return rv;
    }
    Placement placement() override {
        if (!havePos)
            return child ? child->placement() : style.place;
        return pos;
    }
    void children(std::vector<DispP> &out) override { out.push_back(child); }
};

static Value placementTuple(const DispP &d) {
    Placement p = d->placement();
    auto pv = [](const Pos &x) { return x.set ? x.toValue() : Value::integer(0); };
    return mkTuple({pv(p.xpos), pv(p.ypos), pv(p.xanchor), pv(p.yanchor)});
}

static std::shared_ptr<MotionT> makeMove(const Value &startpos, const Value &endpos, double time, const DispP &child,
                                         const CallArgs &a, size_t kwFrom) {
    auto m = std::make_shared<MotionT>();
    m->kind = "Move";
    m->start = startpos;
    m->end = endpos;
    m->period = time;
    m->child = child;
    m->style.place.xanchor = Pos::absolute(0);
    m->style.place.yanchor = Pos::absolute(0);
    for (size_t i = 0; i < a.kw.size(); i++) {
        const auto &kw = a.kw[i];
        if (kw.first == "repeat")
            m->repeat = kw.second.truthy();
        else if (kw.first == "bounce")
            m->bounce = kw.second.truthy();
        else if (kw.first == "delay")
            m->delay = num(kw.second, 0);
        else if (kw.first == "time_warp")
            m->timeWarp = kw.second;
        else if (kw.first == "anim_timebase")
            m->animTimebase = kw.second.truthy();
        else if (kw.first == "new_widget" && !m->child)
            m->child = toDisp(kw.second);
        else if (kw.first == "old_widget")
            m->oldW = toDisp(kw.second);
        else if (kw.first == "xoffset")
            m->style.place.xoffset = num(kw.second);
        else if (kw.first == "yoffset")
            m->style.place.yoffset = num(kw.second);
    }
    (void)kwFrom;
    m->newW = m->child;
    return m;
}

// ------------------------------------------------------------------ move transitions

// Calls a factory value (MoveFactory / MoveIn / MoveOut curries, or a python callable)
static DispP callFactory(const Value &factory, const std::vector<Value> &args, const DispP &d, double delay,
                         const Value &offsets) {
    CallArgs a;
    a.pos = args;
    a.pos.push_back(Value::real(delay));
    a.pos.push_back(dispValue(d));
    if (DictObj *od = offsets.dict())
        for (auto &kv : od->items)
            a.kw.emplace_back(kv.first.s(), kv.second);
    try {
        Value r = PY.call(factory, a);
        if (r.isNone())
            return nullptr;
        return toDisp(r);
    } catch (PyError &e) {
        logf("move factory error: %s %s", e.type.c_str(), e.msg.c_str());
        return d;
    }
}

struct MoveResultT : TransitionDisp {
    DispP root;
    RenderP render(float w, float h, double st, double at) override {
        if (st >= delay)
            events = true;
        return renderChild(root, w, h, st, at);
    }
    void children(std::vector<DispP> &out) override { out.push_back(root); }
};

static bool isNullDisp(const DispP &d) { return !d || d->kind == "Null"; }

static std::shared_ptr<TransitionDisp> oldMoveTransition(double delay, const DispP &oldW, const DispP &newW,
                                                         Value factory, Value enterFactory, Value leaveFactory,
                                                         bool useOld, const std::vector<std::string> &layers) {
    std::function<DispP(const DispP &, const DispP &)> mergeSlide = [&](const DispP &old, const DispP &nw) -> DispP {
        auto newRoot = std::dynamic_pointer_cast<RootDisp>(nw);
        auto newLayer = std::dynamic_pointer_cast<LayerDisp>(nw);
        if (!newRoot && !newLayer) {
            DispP child = useOld ? old : nw;
            Value op = placementTuple(old), np = placementTuple(nw);
            if (!valueEq(op, np)) {
                if (factory.isNone())
                    return makeMove(op, np, delay, child, CallArgs(), 0);
                return callFactory(factory, {op, np}, child, delay, Value());
            }
            return child;
        }
        if (newRoot) {
            auto oldRoot = std::dynamic_pointer_cast<RootDisp>(old);
            auto rv = std::make_shared<RootDisp>();
            rv->kind = "Root";
            for (auto &l : newRoot->layers) {
                DispP f = l.second;
                if (oldRoot && std::find(layers.begin(), layers.end(), l.first) != layers.end() &&
                    std::dynamic_pointer_cast<LayerDisp>(f)) {
                    DispP o = oldRoot->layer(l.first);
                    if (o)
                        f = mergeSlide(o, f);
                }
                rv->layers.emplace_back(l.first, f);
            }
            return rv;
        }
        auto oldLayer = std::dynamic_pointer_cast<LayerDisp>(old);
        if (!oldLayer)
            return nw;
        auto wrap = [](const SceneEntry &e) {
            auto a = std::make_shared<AdjustTimesDisp>();
            a->kind = "AdjustTimes";
            a->child = e.d;
            a->showTime = e.showTime < 0 ? g_interactTime : e.showTime;
            a->animTime = e.animTime < 0 ? g_interactTime : e.animTime;
            return a;
        };
        auto tagOf = [](const SceneEntry &e) { return e.tag.empty() ? std::to_string((intptr_t)e.d.get()) : e.tag; };
        std::vector<SceneEntry> oldSl = oldLayer->entries, newSl = newLayer->entries, rvSl;
        std::map<std::string, SceneEntry> oldMap;
        std::set<std::string> newTags, rvTags;
        for (auto &e : oldSl)
            oldMap[tagOf(e)] = e;
        for (auto &e : newSl)
            newTags.insert(tagOf(e));
        auto merge = [&](const SceneEntry &e, const DispP &d) {
            SceneEntry r = e;
            r.showTime = 0;
            r.animTime = 0;
            r.d = d;
            rvSl.push_back(r);
        };
        size_t oi = 0, ni = 0;
        while (oi < oldSl.size() || ni < newSl.size()) {
            if (oi < oldSl.size()) {
                const SceneEntry &os = oldSl[oi];
                std::string ot = tagOf(os);
                if (rvTags.count(ot)) {
                    oi++;
                    continue;
                }
                if (!newTags.count(ot)) {
                    // leaving
                    if (!leaveFactory.isNone()) {
                        DispP od = wrap(os);
                        DispP mv = callFactory(leaveFactory, {placementTuple(od)}, od, delay, Value());
                        if (mv)
                            merge(os, mv);
                    }
                    rvTags.insert(ot);
                    oi++;
                    continue;
                }
            }
            if (ni >= newSl.size())
                break;
            const SceneEntry ns = newSl[ni++];
            std::string nt = tagOf(ns);
            auto it = oldMap.find(nt);
            if (it != oldMap.end()) {
                DispP od = wrap(it->second), nd = wrap(ns);
                DispP child = useOld ? od : nd;
                Value op = placementTuple(od), np = placementTuple(nd);
                DispP mv;
                if (factory.isNone())
                    mv = valueEq(op, np) ? child : DispP(makeMove(op, np, delay, child, CallArgs(), 0));
                else
                    mv = callFactory(factory, {op, np}, child, delay, Value());
                if (mv)
                    merge(ns, mv);
                rvTags.insert(nt);
            } else {
                DispP nd = wrap(ns);
                DispP mv = nd;
                if (!enterFactory.isNone())
                    mv = callFactory(enterFactory, {placementTuple(nd)}, nd, delay, Value());
                if (mv)
                    merge(ns, mv);
                rvTags.insert(nt);
            }
        }
        std::stable_sort(rvSl.begin(), rvSl.end(), [](const SceneEntry &a, const SceneEntry &b) { return a.zorder < b.zorder; });
        auto rv = std::make_shared<LayerDisp>();
        rv->kind = "Layer";
        rv->layer = newLayer->layer;
        for (auto &e : rvSl) {
            // merged entries use times relative to the transition start
            e.showTime = -1;
            e.animTime = -1;
        }
        rv->entries = rvSl;
        return rv;
    };
    auto res = std::make_shared<MoveResultT>();
    res->kind = "MoveTransition";
    res->delay = delay;
    res->oldW = oldW;
    res->newW = newW;
    res->root = mergeSlide(oldW, newW);
    return res;
}

// ------------------------------------------------------------------ dispatch

static std::shared_ptr<TransitionDisp> asTransition(const DispP &d) {
    return std::dynamic_pointer_cast<TransitionDisp>(d);
}

std::shared_ptr<TransitionDisp> makeTransition(const Value &trans, const DispP &oldW, const DispP &newW) {
    if (trans.isNone())
        return nullptr;
    CallArgs a;
    a.kw.emplace_back("old_widget", dispValue(oldW ? oldW : makeNull()));
    a.kw.emplace_back("new_widget", dispValue(newW ? newW : makeNull()));
    try {
        Value r = PY.call(trans, a);
        DispP d = toDisp(r);
        auto t = asTransition(d);
        if (!t && d) {
            auto wrapper = std::make_shared<NoTransitionT>();
            wrapper->kind = "WrappedTransition";
            wrapper->newW = d;
            wrapper->delay = 0;
            return wrapper;
        }
        return t;
    } catch (PyError &e) {
        logf("transition error: %s %s", e.type.c_str(), e.msg.c_str());
        return nullptr;
    }
}

static DispP kwDisp(const CallArgs &a, const char *name) {
    for (auto &kw : a.kw)
        if (kw.first == name)
            return toDisp(kw.second);
    return nullptr;
}
static Value kwValue(const CallArgs &a, const char *name, const Value &def = Value()) {
    for (auto &kw : a.kw)
        if (kw.first == name)
            return kw.second;
    return def;
}

void registerTransitionNatives(Interp &I) {
    // renpy.curry.Curry(callable, *args, **kwargs)
    I.registerNative("renpy.curry.Curry", [](Interp &, CallArgs &a) {
        Value c = mkInst("renpy.curry.Curry");
        c.inst()->attrs["callable"] = a.pos.empty() ? Value() : a.pos[0];
        std::vector<Value> rest;
        if (a.pos.size() > 1)
            rest.assign(a.pos.begin() + 1, a.pos.end());
        c.inst()->attrs["args"] = mkTuple(rest);
        Value kw = mkDict();
        for (auto &k : a.kw)
            kw.dict()->set(Value::str(k.first), k.second);
        c.inst()->attrs["kwargs"] = kw;
        return c;
    });
    I.registerNative("renpy.curry.curry", [](Interp &I2, CallArgs &a) {
        Value c = mkInst("renpy.curry.Curry");
        c.inst()->attrs["callable"] = I2.natives["renpy.curry.Curry"];
        c.inst()->attrs["args"] = mkTuple({a.arg(0, nullptr)});
        c.inst()->attrs["kwargs"] = mkDict();
        return c;
    });
    I.registerNative("renpy.display.transition.Dissolve", [](Interp &, CallArgs &a) {
        auto t = std::make_shared<DissolveT>();
        t->kind = "Dissolve";
        t->delay = argNum(a, 0, "time", 0.5);
        t->alpha = argBool(a, 1000, "alpha", false);
        t->timeWarp = kwValue(a, "time_warp");
        t->oldW = kwDisp(a, "old_widget");
        t->newW = kwDisp(a, "new_widget");
        return dispValue(t, "#transition");
    });
    I.registerNative("renpy.display.transition.ImageDissolve", [](Interp &, CallArgs &a) {
        auto t = std::make_shared<ImageDissolveT>();
        t->kind = "ImageDissolve";
        t->image = toDisp(a.arg(0, "image"));
        t->delay = argNum(a, 1, "time", 1.0);
        t->ramp = (int)argNum(a, 2, "ramplen", 8);
        Value ramp = kwValue(a, "ramp");
        if (ListObj *rl = ramp.list())
            t->ramp = (int)rl->v.size();
        t->reverse = argBool(a, 1000, "reverse", false);
        t->alpha = argBool(a, 1000, "alpha", false);
        t->timeWarp = kwValue(a, "time_warp");
        t->oldW = kwDisp(a, "old_widget");
        t->newW = kwDisp(a, "new_widget");
        return dispValue(t, "#transition");
    });
    I.registerNative("renpy.display.transition.NoTransition", [](Interp &, CallArgs &a) {
        auto t = std::make_shared<NoTransitionT>();
        t->kind = "NoTransition";
        t->delay = argNum(a, 0, "delay", 0);
        t->oldW = kwDisp(a, "old_widget");
        t->newW = kwDisp(a, "new_widget");
        return dispValue(t, "#transition");
    });
    I.registerNative("renpy.display.transition.MultipleTransition", [](Interp &, CallArgs &a) {
        auto t = std::make_shared<MultipleT>();
        t->kind = "MultipleTransition";
        DispP oldW = kwDisp(a, "old_widget"), newW = kwDisp(a, "new_widget");
        ListObj *args = a.arg(0, "args").list();
        if (!args || args->v.size() < 3)
            throwPy("Exception", "MultipleTransition requires an odd number of arguments");
        auto oldnew = [&](const Value &w) -> DispP {
            if (w.t == T::Bool)
                return w.i ? newW : oldW;
            return toDisp(w);
        };
        std::vector<DispP> screens;
        for (size_t i = 0; i < args->v.size(); i += 2)
            screens.push_back(oldnew(args->v[i]));
        for (size_t i = 1; i < args->v.size(); i += 2) {
            DispP o = screens[i / 2], n = screens[i / 2 + 1];
            auto tr = makeTransition(args->v[i], o, n);
            if (!tr) {
                auto nt = std::make_shared<NoTransitionT>();
                nt->newW = n;
                tr = nt;
            }
            t->transitions.push_back(tr);
        }
        t->delay = 0;
        for (auto &tr : t->transitions)
            t->delay += tr->delay;
        t->oldW = oldW;
        t->newW = newW;
        return dispValue(t, "#transition");
    });
    I.registerNative("renpy.display.transition.Fade", [](Interp &I2, CallArgs &a) {
        double outT = argNum(a, 0, "out_time", 0.5), holdT = argNum(a, 1, "hold_time", 0),
               inT = argNum(a, 2, "in_time", 0.5);
        bool alpha = argBool(a, 1000, "alpha", false);
        Value color = kwValue(a, "color");
        Value widget = kwValue(a, "widget");
        DispP w;
        if (!color.isNone()) {
            float r = 0, g = 0, b = 0, al = 1;
            if (color.isStr()) {
                Color c = hexColor(color.s().c_str());
                r = c.r;
                g = c.g;
                b = c.b;
                al = c.a;
            }
            w = makeSolid(r, g, b, al);
        } else if (!widget.isNone()) {
            w = toDisp(widget);
        } else {
            w = makeSolid(0, 0, 0, 1);
        }
        DispP oldW = kwDisp(a, "old_widget"), newW = kwDisp(a, "new_widget");
        auto mt = std::make_shared<MultipleT>();
        mt->kind = "Fade";
        auto d1 = std::make_shared<DissolveT>();
        d1->delay = outT;
        d1->alpha = alpha;
        d1->oldW = oldW;
        d1->newW = w;
        mt->transitions.push_back(d1);
        if (holdT > 0) {
            auto h = std::make_shared<NoTransitionT>();
            h->delay = holdT;
            h->oldW = w;
            h->newW = w;
            mt->transitions.push_back(h);
        }
        auto d2 = std::make_shared<DissolveT>();
        d2->delay = inT;
        d2->alpha = alpha;
        d2->oldW = w;
        d2->newW = newW;
        mt->transitions.push_back(d2);
        mt->delay = outT + holdT + inT;
        mt->oldW = oldW;
        mt->newW = newW;
        (void)I2;
        return dispValue(mt, "#transition");
    });
    I.registerNative("renpy.display.transition.CropMove", [](Interp &, CallArgs &a) {
        auto t = std::make_shared<CropMoveT>();
        t->kind = "CropMove";
        t->delay = argNum(a, 0, "time", 1);
        std::string mode = argStr(a, 1, "mode", "slideright");
        auto set = [&](double sp0, double sp1, double sc0, double sc1, double sc2, double sc3, double ep0, double ep1,
                       double ec0, double ec1, double ec2, double ec3, bool topnew) {
            t->startpos[0] = sp0;
            t->startpos[1] = sp1;
            t->startcrop[0] = sc0;
            t->startcrop[1] = sc1;
            t->startcrop[2] = sc2;
            t->startcrop[3] = sc3;
            t->endpos[0] = ep0;
            t->endpos[1] = ep1;
            t->endcrop[0] = ec0;
            t->endcrop[1] = ec1;
            t->endcrop[2] = ec2;
            t->endcrop[3] = ec3;
            t->topnew = topnew;
        };
        if (mode == "wiperight")
            set(0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, true);
        else if (mode == "wipeleft")
            set(1, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, true);
        else if (mode == "wipedown")
            set(0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, true);
        else if (mode == "wipeup")
            set(0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, true);
        else if (mode == "slideright")
            set(0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 1, true);
        else if (mode == "slideleft")
            set(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, true);
        else if (mode == "slideup")
            set(0, 1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 1, true);
        else if (mode == "slidedown")
            set(0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 1, 1, true);
        else if (mode == "slideawayleft")
            set(0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, false);
        else if (mode == "slideawayright")
            set(0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, false);
        else if (mode == "slideawaydown")
            set(0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, false);
        else if (mode == "slideawayup")
            set(0, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 0, false);
        else if (mode == "irisout")
            set(.5, .5, .5, .5, 0, 0, 0, 0, 0, 0, 1, 1, true);
        else if (mode == "irisin")
            set(0, 0, 0, 0, 1, 1, .5, .5, .5, .5, 0, 0, false);
        t->oldW = kwDisp(a, "old_widget");
        t->newW = kwDisp(a, "new_widget");
        return dispValue(t, "#transition");
    });
    I.registerNative("renpy.display.transition.ComposeTransition", [](Interp &, CallArgs &a) {
        Value trans = a.arg(0, "trans");
        Value before = kwValue(a, "before"), after = kwValue(a, "after");
        DispP oldW = kwDisp(a, "old_widget"), newW = kwDisp(a, "new_widget");
        DispP o = oldW, n = newW;
        if (!before.isNone())
            if (auto t = makeTransition(before, oldW, newW))
                o = t;
        if (!after.isNone())
            if (auto t = makeTransition(after, oldW, newW))
                n = t;
        auto t = makeTransition(trans, o, n);
        if (!t) {
            auto nt = std::make_shared<NoTransitionT>();
            nt->newW = n;
            t = nt;
        }
        return dispValue(t, "#transition");
    });
    auto moveTransition = [](Interp &, CallArgs &a) {
        double delay = argNum(a, 0, "delay", 1);
        DispP oldW = kwDisp(a, "old_widget"), newW = kwDisp(a, "new_widget");
        std::vector<std::string> layers = {"master"};
        Value lv = kwValue(a, "layers");
        if (ListObj *ll = lv.list()) {
            layers.clear();
            for (auto &x : ll->v)
                layers.push_back(x.s());
        }
        auto t = oldMoveTransition(delay, oldW, newW, kwValue(a, "factory"), kwValue(a, "enter_factory"),
                                   kwValue(a, "leave_factory"), argBool(a, 1000, "old", false), layers);
        return dispValue(t, "#transition");
    };
    I.registerNative("renpy.display.movetransition.OldMoveTransition", moveTransition);
    I.registerNative("renpy.display.movetransition.MoveTransition", moveTransition);
    // MoveFactory(pos1, pos2, delay, d, **kwargs)
    I.registerNative("renpy.display.movetransition.MoveFactory", [](Interp &, CallArgs &a) {
        Value p1 = a.arg(0, nullptr), p2 = a.arg(1, nullptr);
        DispP d = toDisp(a.arg(3, nullptr));
        if (valueEq(p1, p2))
            return dispValue(d);
        return dispValue(makeMove(p1, p2, argNum(a, 2, nullptr, 1), d, a, 0), "#motion");
    });
    auto aorb = [](const Value &pos, const Value &pos1) {
        std::vector<Value> r;
        ListObj *pl = pos.list(), *p1 = pos1.list();
        size_t n = std::min(pl ? pl->v.size() : 0, p1 ? p1->v.size() : 0);
        for (size_t i = 0; i < n; i++)
            r.push_back(pl->v[i].isNone() ? p1->v[i] : pl->v[i]);
        return mkTuple(r);
    };
    I.registerNative("renpy.display.movetransition.MoveIn", [aorb](Interp &, CallArgs &a) {
        Value pos = a.arg(0, nullptr), pos1 = a.arg(1, nullptr);
        Value p = aorb(pos, pos1);
        return dispValue(makeMove(p, pos1, argNum(a, 2, nullptr, 1), toDisp(a.arg(3, nullptr)), a, 0), "#motion");
    });
    I.registerNative("renpy.display.movetransition.MoveOut", [aorb](Interp &, CallArgs &a) {
        Value pos = a.arg(0, nullptr), pos1 = a.arg(1, nullptr);
        Value p = aorb(pos, pos1);
        return dispValue(makeMove(pos1, p, argNum(a, 2, nullptr, 1), toDisp(a.arg(3, nullptr)), a, 0), "#motion");
    });
    // Move(startpos, endpos, time, child=None, repeat, bounce, anim_timebase, style, time_warp, **properties)
    I.registerNative("renpy.display.motion.Move", [](Interp &, CallArgs &a) {
        DispP child = a.pos.size() > 3 ? toDisp(a.pos[3]) : nullptr;
        auto m = makeMove(a.arg(0, "startpos"), a.arg(1, "endpos"), argNum(a, 2, "time", 1), child, a, 0);
        return dispValue(m, "#transition");
    });
    auto soundTransition = [](Interp &, CallArgs &a) {
        auto t = std::make_shared<SoundT>();
        t->kind = "SoundTransition";
        t->sound = argStr(a, 0, "sound");
        t->delay = argNum(a, 1, "delay", 0.0001);
        t->channel = argStr(a, 2, "channel", "sound");
        t->oldW = kwDisp(a, "old_widget");
        t->newW = kwDisp(a, "new_widget");
        return dispValue(t, "#transition");
    };
    I.registerNative("store.SoundTransitionClassGL", soundTransition);
    I.registerNative("store.SoundTransitionClassCompat", soundTransition);
    I.registerNative("renpy.display.transition.AlphaDissolve", [](Interp &, CallArgs &a) {
        auto t = std::make_shared<DissolveT>();
        t->kind = "AlphaDissolve";
        t->delay = argNum(a, 1, "delay", 0);
        t->oldW = kwDisp(a, "old_widget");
        t->newW = kwDisp(a, "new_widget");
        return dispValue(t, "#transition");
    });
}
