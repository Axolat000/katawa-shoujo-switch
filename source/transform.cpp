#include "disp.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <random>

RenderP renderChild(const DispP &d, float w, float h, double st, double at);

// ------------------------------------------------------------------ warpers

double builtinWarp(const std::string &name, double x, bool &found) {
    found = true;
    if (name == "linear")
        return x;
    if (name == "pause")
        return x < 1.0 ? 0.0 : 1.0;
    if (name == "ease" || name == "_ease_time_warp")
        return .5 - std::cos(M_PI * x) / 2.0;
    if (name == "easein" || name == "_ease_in_time_warp")
        return std::cos((1.0 - x) * M_PI / 2.0);
    if (name == "easeout" || name == "_ease_out_time_warp")
        return 1.0 - std::cos(x * M_PI / 2.0);
    found = false;
    return x;
}

double callWarper(const Value &warper, double t) {
    if (warper.isNone())
        return t;
    if (warper.isStr()) {
        bool found;
        double r = builtinWarp(warper.s(), t, found);
        if (found)
            return r;
        if (PY.hasGlobal(warper.s()))
            return PY.call(PY.getGlobal(warper.s()), {Value::real(t)}).num();
        return t;
    }
    if (FuncObj *fn = warper.func()) {
        std::string n = fn->name.substr(fn->name.rfind('.') + 1);
        bool found;
        double r = builtinWarp(n, t, found);
        if (found)
            return r;
    }
    try {
        return PY.call(warper, {Value::real(t)}).num();
    } catch (PyError &e) {
        logf("warper error: %s %s", e.type.c_str(), e.msg.c_str());
        return t;
    }
}

// ------------------------------------------------------------------ transform state

static double num(const Value &v, double def = 0) { return v.isNum() ? v.num() : def; }

static Value tuple2(const Value &a, const Value &b) { return mkTuple({a, b}); }

static Value tupleAt(const Value &t, size_t i) {
    ListObj *l = t.list();
    return (l && i < l->v.size()) ? l->v[i] : Value();
}

void TransformState::takeState(const TransformState &o) {
    alpha = o.alpha;
    additive = o.additive;
    rotate = o.rotate;
    rotatePad = o.rotatePad;
    transformAnchor = o.transformAnchor;
    zoom = o.zoom;
    xzoom = o.xzoom;
    yzoom = o.yzoom;
    xaround = o.xaround;
    yaround = o.yaround;
    xanchoraround = o.xanchoraround;
    yanchoraround = o.yanchoraround;
    subpixel = o.subpixel;
    crop = o.crop;
    corner1 = o.corner1;
    corner2 = o.corner2;
    size = o.size;
    // get_placement of the other state
    defaultXpos = o.xpos.set ? o.xpos : o.defaultXpos;
    defaultYpos = o.ypos.set ? o.ypos : o.defaultYpos;
    defaultXanchor = o.xanchor.set ? o.xanchor : o.defaultXanchor;
    defaultYanchor = o.yanchor.set ? o.yanchor : o.defaultYanchor;
    xoffset = o.xoffset;
    yoffset = o.yoffset;
}

Value TransformState::get(const std::string &p) const {
    if (p == "alpha")
        return Value::real(alpha);
    if (p == "additive")
        return Value::real(additive);
    if (p == "rotate")
        return rotate;
    if (p == "rotate_pad")
        return Value::boolean(rotatePad);
    if (p == "transform_anchor")
        return Value::boolean(transformAnchor);
    if (p == "zoom")
        return Value::real(zoom);
    if (p == "xzoom")
        return Value::real(xzoom);
    if (p == "yzoom")
        return Value::real(yzoom);
    if (p == "xpos")
        return xpos.toValue();
    if (p == "ypos")
        return ypos.toValue();
    if (p == "xanchor")
        return xanchor.toValue();
    if (p == "yanchor")
        return yanchor.toValue();
    if (p == "xalign")
        return xpos.toValue();
    if (p == "yalign")
        return ypos.toValue();
    if (p == "xoffset")
        return Value::real(xoffset);
    if (p == "yoffset")
        return Value::real(yoffset);
    if (p == "xaround")
        return xaround.toValue();
    if (p == "yaround")
        return yaround.toValue();
    if (p == "xanchoraround")
        return xanchoraround;
    if (p == "yanchoraround")
        return yanchoraround;
    if (p == "subpixel")
        return Value::boolean(subpixel);
    if (p == "crop")
        return crop;
    if (p == "corner1")
        return corner1;
    if (p == "corner2")
        return corner2;
    if (p == "size")
        return size;
    if (p == "delay")
        return Value::real(delay);
    if (p == "pos")
        return tuple2(xpos.toValue(), ypos.toValue());
    if (p == "anchor")
        return tuple2(xanchor.toValue(), yanchor.toValue());
    if (p == "align")
        return tuple2(xpos.toValue(), ypos.toValue());
    if (p == "offset")
        return tuple2(Value::real(xoffset), Value::real(yoffset));
    if (p == "around" || p == "alignaround")
        return tuple2(xaround.toValue(), yaround.toValue());
    if (p == "xcenter")
        return xpos.toValue();
    if (p == "ycenter")
        return ypos.toValue();
    if (p == "default_xpos")
        return defaultXpos.toValue();
    if (p == "default_ypos")
        return defaultYpos.toValue();
    if (p == "default_xanchor")
        return defaultXanchor.toValue();
    if (p == "default_yanchor")
        return defaultYanchor.toValue();
    return Value();
}

void TransformState::set(const std::string &p, const Value &v) {
    if (p == "alpha")
        alpha = num(v, 1);
    else if (p == "additive")
        additive = num(v, 0);
    else if (p == "rotate")
        rotate = v;
    else if (p == "rotate_pad")
        rotatePad = v.truthy();
    else if (p == "transform_anchor")
        transformAnchor = v.truthy();
    else if (p == "zoom")
        zoom = num(v, 1);
    else if (p == "xzoom")
        xzoom = num(v, 1);
    else if (p == "yzoom")
        yzoom = num(v, 1);
    else if (p == "xpos")
        xpos = Pos::from(v);
    else if (p == "ypos")
        ypos = Pos::from(v);
    else if (p == "xanchor")
        xanchor = Pos::from(v);
    else if (p == "yanchor")
        yanchor = Pos::from(v);
    else if (p == "xalign") {
        xpos = Pos::from(v);
        xanchor = Pos::from(v);
    } else if (p == "yalign") {
        ypos = Pos::from(v);
        yanchor = Pos::from(v);
    } else if (p == "xoffset")
        xoffset = num(v, 0);
    else if (p == "yoffset")
        yoffset = num(v, 0);
    else if (p == "xaround")
        xaround = Pos::from(v);
    else if (p == "yaround")
        yaround = Pos::from(v);
    else if (p == "xanchoraround")
        xanchoraround = v;
    else if (p == "yanchoraround")
        yanchoraround = v;
    else if (p == "subpixel")
        subpixel = v.truthy();
    else if (p == "crop")
        crop = v;
    else if (p == "corner1")
        corner1 = v;
    else if (p == "corner2")
        corner2 = v;
    else if (p == "size")
        size = v;
    else if (p == "delay")
        delay = num(v, 0);
    else if (p == "pos") {
        xpos = Pos::from(tupleAt(v, 0));
        ypos = Pos::from(tupleAt(v, 1));
    } else if (p == "anchor") {
        xanchor = Pos::from(tupleAt(v, 0));
        yanchor = Pos::from(tupleAt(v, 1));
    } else if (p == "align") {
        xpos = xanchor = Pos::from(tupleAt(v, 0));
        ypos = yanchor = Pos::from(tupleAt(v, 1));
    } else if (p == "offset") {
        xoffset = num(tupleAt(v, 0));
        yoffset = num(tupleAt(v, 1));
    } else if (p == "around") {
        xaround = Pos::from(tupleAt(v, 0));
        yaround = Pos::from(tupleAt(v, 1));
        xanchoraround = yanchoraround = Value();
    } else if (p == "alignaround") {
        xaround = Pos::from(tupleAt(v, 0));
        yaround = Pos::from(tupleAt(v, 1));
        xanchoraround = tupleAt(v, 0);
        yanchoraround = tupleAt(v, 1);
    } else if (p == "xcenter") {
        xpos = Pos::from(v);
        xanchor = Pos::relative(0.5);
    } else if (p == "ycenter") {
        ypos = Pos::from(v);
        yanchor = Pos::relative(0.5);
    } else if (p == "default_xpos")
        defaultXpos = Pos::from(v);
    else if (p == "default_ypos")
        defaultYpos = Pos::from(v);
    else if (p == "default_xanchor")
        defaultXanchor = Pos::from(v);
    else if (p == "default_yanchor")
        defaultYanchor = Pos::from(v);
}

// property types for interpolation
enum PType { PT_POS, PT_FLOAT, PT_INT, PT_BOOL };
static void propTypes(const std::string &p, std::vector<PType> &out) {
    out.clear();
    if (p == "xpos" || p == "ypos" || p == "xanchor" || p == "yanchor" || p == "xaround" || p == "yaround" ||
        p == "xcenter" || p == "ycenter")
        out = {PT_POS};
    else if (p == "pos" || p == "anchor" || p == "around")
        out = {PT_POS, PT_POS};
    else if (p == "align" || p == "alignaround" || p == "corner1" || p == "corner2")
        out = {PT_FLOAT, PT_FLOAT};
    else if (p == "crop")
        out = {PT_FLOAT, PT_FLOAT, PT_FLOAT, PT_FLOAT};
    else if (p == "size" || p == "offset")
        out = {PT_INT, PT_INT};
    else if (p == "rotate_pad" || p == "transform_anchor" || p == "subpixel")
        out = {PT_BOOL};
    else
        out = {PT_FLOAT};
}

static Value interpOne(double t, const Value &a, const Value &b, PType ty) {
    if (t >= 1.0)
        return b;
    if (b.isNone() || b.t == T::Bool)
        return a;
    double av = a.isNum() ? a.num() : 0.0;
    double v = av + t * (b.num() - av);
    switch (ty) {
    case PT_POS:
        return b.t == T::Float ? Value::real(v) : Value::integer((int64_t)v);
    case PT_INT:
        return Value::integer((int64_t)v);
    case PT_BOOL:
        return a;
    default:
        return Value::real(v);
    }
}

static Value interpolate(double t, const Value &a, const Value &b, const std::vector<PType> &types) {
    if (t >= 1.0)
        return b;
    if (ListObj *bl = b.list()) {
        ListObj *al = a.list();
        std::vector<Value> out;
        for (size_t i = 0; i < bl->v.size(); i++) {
            Value ai = (al && i < al->v.size()) ? al->v[i] : Value();
            out.push_back(interpOne(t, ai, bl->v[i], i < types.size() ? types[i] : PT_FLOAT));
        }
        return mkTuple(std::move(out));
    }
    return interpOne(t, a, b, types.empty() ? PT_FLOAT : types[0]);
}

// Ren'Py TransformState.diff: properties that differ between old (this) and new
static void stateDiff(const TransformState &o, const TransformState &n,
                      std::vector<std::tuple<std::string, Value, Value>> &out) {
    auto d2 = [&](const char *p) {
        Value a = o.get(p), b = n.get(p);
        bool same = valueEq(a, b) && a.t == b.t;
        if (a.isNum() && b.isNum() && a.t != b.t)
            same = false;
        if (!same)
            out.emplace_back(p, a, b);
    };
    auto d4 = [&](const char *p, const Pos &nv, const Pos &nd, const Pos &ov, const Pos &od) {
        Pos nn = nv.set ? nv : nd, oo = ov.set ? ov : od;
        if (nn.set != oo.set || nn.v != oo.v || nn.rel != oo.rel)
            out.emplace_back(p, oo.toValue(), nn.toValue());
    };
    d2("alpha");
    d2("additive");
    d2("rotate");
    d2("rotate_pad");
    d2("transform_anchor");
    d2("zoom");
    d2("xzoom");
    d2("yzoom");
    d2("xaround");
    d2("yaround");
    d2("xanchoraround");
    d2("yanchoraround");
    d2("subpixel");
    d2("crop");
    d2("corner1");
    d2("corner2");
    d2("size");
    d4("xpos", n.xpos, n.defaultXpos, o.xpos, o.defaultXpos);
    d4("xanchor", n.xanchor, n.defaultXanchor, o.xanchor, o.defaultXanchor);
    d2("xoffset");
    d4("ypos", n.ypos, n.defaultYpos, o.ypos, o.defaultYpos);
    d4("yanchor", n.yanchor, n.defaultYanchor, o.yanchor, o.defaultYanchor);
    d2("yoffset");
}

// ------------------------------------------------------------------ ATL

struct AtlStmt;
using AtlStmtP = std::shared_ptr<AtlStmt>;

struct AtlBlock {
    std::vector<AtlStmtP> stmts;
    std::vector<std::pair<double, int>> times;
};
using AtlBlockP = std::shared_ptr<AtlBlock>;

struct AtlStmt {
    enum K { INTERP, CHILD, REPEAT, PARALLEL, CHOICE, FUNCTION, TIME, ON, EVENT, BLOCK } k = INTERP;
    // INTERP
    Value warper;
    double duration = 0;
    std::vector<std::pair<std::string, Value>> props;
    std::string revolution;
    double circles = 0;
    std::vector<std::pair<std::string, std::vector<Value>>> splines;
    // CHILD
    Value childValue;
    Value transition;
    // REPEAT
    Value repeats;
    // PARALLEL / BLOCK
    std::vector<AtlBlockP> blocks;
    // CHOICE
    std::vector<std::pair<double, AtlBlockP>> choices;
    // FUNCTION
    Value function;
    // TIME
    double time = 0;
    // ON
    std::map<std::string, AtlBlockP> handlers;
    std::string eventName;
};

struct AtlState {
    // block
    int index = 0;
    double start = 0, loopStart = 0;
    int repeats = 0;
    std::vector<std::pair<double, int>> times;
    std::shared_ptr<AtlState> child;
    bool blockInit = false;
    // interpolation
    bool interpInit = false;
    std::vector<std::tuple<std::string, Value, Value>> linear;
    std::vector<std::pair<std::string, std::vector<Value>>> splines;
    // parallel
    std::vector<std::pair<AtlBlockP, std::shared_ptr<AtlState>>> par;
    bool parInit = false;
    // choice
    AtlBlockP choice;
    // on
    std::string onName;
    double onStart = 0;
};
using AtlStateP = std::shared_ptr<AtlState>;

enum AtlAction { A_CONTINUE, A_NEXT, A_REPEAT, A_EVENT };
struct AtlResult {
    AtlAction action = A_NEXT;
    double arg = 0;      // NEXT: leftover time ; REPEAT: st
    Value repeatCount;   // REPEAT
    AtlStateP state;     // CONTINUE
    double pause = -1;   // -1 = None
    std::string event;
};

struct AtlContext {
    Value context; // dict
};

static Value atlEval(const Value &code, const Value &context) {
    if (code.isNone())
        return Value();
    if (code.t != T::Code)
        return code;
    auto frame = std::make_shared<Frame>();
    static FuncInfo ctxInfo;
    frame->fi = &ctxInfo;
    if (DictObj *d = context.dict())
        for (auto &kv : d->items)
            frame->locals[kv.first.s()] = kv.second;
    return PY.eval((int)code.i, frame);
}

static AtlBlockP compileBlock(const Value &raw, const Value &context);

static bool isAtlTransformValue(const Value &v) {
    InstObj *in = v.inst();
    if (!in)
        return false;
    if (in->cls == "renpy.display.motion.ATLTransform")
        return true;
    if (DispP d = valueDisp(v))
        if (d->isTransform())
            return std::static_pointer_cast<TransformDisp>(d)->atlRaw.inst() != nullptr;
    return false;
}

static void atlOf(const Value &v, Value &atl, Value &ctx) {
    InstObj *in = v.inst();
    if (in->cls == "renpy.display.motion.ATLTransform") {
        atl = in->get("atl");
        Value c = in->get("context");
        ctx = c.inst() ? c.inst()->get("context") : c;
        return;
    }
    auto t = std::static_pointer_cast<TransformDisp>(valueDisp(v));
    atl = t->atlRaw;
    ctx = t->context;
}

static AtlStmtP compileStmt(const Value &raw, const Value &context) {
    InstObj *in = raw.inst();
    auto s = std::make_shared<AtlStmt>();
    if (!in)
        return nullptr;
    const std::string &c = in->cls;
    if (c == "atl.Block") {
        s->k = AtlStmt::BLOCK;
        s->blocks.push_back(compileBlock(raw, context));
        return s;
    }
    if (c == "atl.Multi") {
        ListObj *props = in->get("properties").list();
        ListObj *exprs = in->get("expressions").list();
        ListObj *splines = in->get("splines").list();
        bool simple = in->get("warper").isNone() && in->get("warp_function").isNone() && (!props || props->v.empty()) &&
                      (!splines || splines->v.empty()) && exprs && exprs->v.size() == 1;
        if (simple) {
            Value e = tupleAt(exprs->v[0], 0), w = tupleAt(exprs->v[0], 1);
            Value child = atlEval(e, context);
            if (child.isNum()) {
                s->k = AtlStmt::INTERP;
                s->warper = Value::str("pause");
                s->duration = child.num();
                return s;
            }
            if (isAtlTransformValue(child)) {
                Value atl, ctx;
                atlOf(child, atl, ctx);
                s->k = AtlStmt::BLOCK;
                s->blocks.push_back(compileBlock(atl, ctx));
                return s;
            }
            s->k = AtlStmt::CHILD;
            s->childValue = child;
            s->transition = w.isNone() ? Value() : atlEval(w, context);
            return s;
        }
        s->k = AtlStmt::INTERP;
        Value wf = in->get("warp_function");
        if (!wf.isNone())
            s->warper = atlEval(wf, context);
        else
            s->warper = in->get("warper").isNone() ? Value::str("pause") : in->get("warper");
        if (props)
            for (auto &p : props->v)
                s->props.emplace_back(tupleAt(p, 0).s(), atlEval(tupleAt(p, 1), context));
        if (splines)
            for (auto &sp : splines->v) {
                std::vector<Value> vals;
                if (ListObj *ex = tupleAt(sp, 1).list())
                    for (auto &x : ex->v)
                        vals.push_back(atlEval(x, context));
                s->splines.emplace_back(tupleAt(sp, 0).s(), vals);
            }
        if (exprs)
            for (auto &ew : exprs->v) {
                Value val = atlEval(tupleAt(ew, 0), context);
                if (isAtlTransformValue(val)) {
                    Value atl, ctx;
                    atlOf(val, atl, ctx);
                    AtlBlockP b = compileBlock(atl, ctx);
                    if (b->stmts.size() == 1 && b->stmts[0]->k == AtlStmt::INTERP && b->stmts[0]->duration == 0)
                        for (auto &pp : b->stmts[0]->props)
                            s->props.push_back(pp);
                }
            }
        s->duration = num(atlEval(in->get("duration"), context), 0);
        s->revolution = in->get("revolution").isStr() ? in->get("revolution").s() : "";
        s->circles = num(atlEval(in->get("circles"), context), 0);
        return s;
    }
    if (c == "atl.Repeat") {
        s->k = AtlStmt::REPEAT;
        s->repeats = atlEval(in->get("repeats"), context);
        return s;
    }
    if (c == "atl.Parallel") {
        s->k = AtlStmt::PARALLEL;
        if (ListObj *bl = in->get("blocks").list())
            for (auto &b : bl->v)
                s->blocks.push_back(compileBlock(b, context));
        return s;
    }
    if (c == "atl.Choice") {
        s->k = AtlStmt::CHOICE;
        if (ListObj *cl = in->get("choices").list())
            for (auto &ch : cl->v)
                s->choices.emplace_back(num(atlEval(tupleAt(ch, 0), context), 1), compileBlock(tupleAt(ch, 1), context));
        return s;
    }
    if (c == "atl.Function") {
        s->k = AtlStmt::FUNCTION;
        s->function = atlEval(in->get("expr"), context);
        return s;
    }
    if (c == "atl.Time") {
        s->k = AtlStmt::TIME;
        s->time = num(atlEval(in->get("time"), context), 0);
        return s;
    }
    if (c == "atl.On") {
        s->k = AtlStmt::ON;
        if (DictObj *h = in->get("handlers").dict())
            for (auto &kv : h->items)
                s->handlers[kv.first.s()] = compileBlock(kv.second, context);
        return s;
    }
    if (c == "atl.Event") {
        s->k = AtlStmt::EVENT;
        s->eventName = in->get("name").s();
        return s;
    }
    if (c == "atl.Child" || c == "atl.ContainsExpr") {
        s->k = AtlStmt::CHILD;
        if (c == "atl.ContainsExpr")
            s->childValue = atlEval(in->get("expression"), context);
        return s;
    }
    logf("ATL: unsupported statement %s", c.c_str());
    return nullptr;
}

static AtlBlockP compileBlock(const Value &raw, const Value &context) {
    auto b = std::make_shared<AtlBlock>();
    InstObj *in = raw.inst();
    if (!in)
        return b;
    if (ListObj *st = in->get("stmts").list())
        for (auto &x : st->v) {
            try {
                if (AtlStmtP s = compileStmt(x, context)) {
                    if (s->k == AtlStmt::TIME)
                        b->times.emplace_back(s->time, (int)b->stmts.size() + 1);
                    b->stmts.push_back(s);
                }
            } catch (PyError &e) {
                logf("ATL compile error: %s %s", e.type.c_str(), e.msg.c_str());
            }
        }
    std::sort(b->times.begin(), b->times.end());
    return b;
}

static AtlResult execStmt(TransformDisp &trans, const AtlStmtP &s, double st, const AtlStateP &state,
                          const std::string &event);

static AtlResult execBlock(TransformDisp &trans, AtlBlock &blk, double st, AtlStateP state, const std::string &event) {
    AtlStateP S;
    if (state) {
        S = std::make_shared<AtlState>(*state);
    } else {
        S = std::make_shared<AtlState>();
        S->times = blk.times;
    }
    AtlResult res;
    res.action = A_CONTINUE;
    int guard = 0;
    while (res.action == A_CONTINUE) {
        double target, maxPause;
        if (!S->times.empty()) {
            double time = S->times[0].first;
            target = std::min(time, st);
            maxPause = time - target;
        } else {
            target = st;
            maxPause = 15;
        }
        while (true) {
            if (++guard > 100000) {
                logf("ATL: runaway block");
                AtlResult r;
                r.action = A_NEXT;
                return r;
            }
            if (S->index >= (int)blk.stmts.size()) {
                AtlResult r;
                r.action = A_NEXT;
                r.arg = target - S->start;
                return r;
            }
            AtlResult r = execStmt(trans, blk.stmts[S->index], target - S->start, S->child, event);
            if (r.action == A_CONTINUE) {
                double pause = r.pause < 0 ? maxPause : std::min(maxPause, r.pause);
                S->child = r.state;
                res.action = A_CONTINUE;
                res.state = S;
                res.pause = pause;
                break;
            } else if (r.action == A_EVENT) {
                return r;
            } else if (r.action == A_NEXT) {
                S->index++;
                S->start = target - r.arg;
                S->child = nullptr;
            } else if (r.action == A_REPEAT) {
                double loopEnd = target - r.arg;
                double duration = loopEnd - S->loopStart;
                if (duration <= 0) {
                    // Ren'Py raises; stop the block instead of looping forever
                    AtlResult nr;
                    nr.action = A_CONTINUE;
                    nr.state = S;
                    nr.pause = 0;
                    return nr;
                }
                int newRepeats = (int)((target - S->loopStart) / duration);
                if (!r.repeatCount.isNone()) {
                    int count = (int)r.repeatCount.asInt();
                    if (S->repeats + newRepeats >= count) {
                        newRepeats = count - S->repeats;
                        S->loopStart += newRepeats * duration;
                        AtlResult nr;
                        nr.action = A_NEXT;
                        nr.arg = target - S->loopStart;
                        return nr;
                    }
                }
                S->repeats += newRepeats;
                S->loopStart = S->loopStart + newRepeats * duration;
                S->start = S->loopStart;
                S->index = 0;
                S->child = nullptr;
            }
        }
        if (!S->times.empty()) {
            double time = S->times[0].first;
            int tindex = S->times[0].second;
            if (time <= target) {
                S->times.erase(S->times.begin());
                S->index = tindex;
                S->start = time;
                S->child = nullptr;
                res.action = A_CONTINUE;
                continue;
            }
        }
        return res;
    }
    return res;
}

static Value splineValue(double t, const std::vector<Value> &sp) {
    if (sp.empty())
        return Value();
    if (sp.back().list()) {
        size_t n = sp.back().list()->v.size();
        std::vector<Value> out;
        for (size_t i = 0; i < n; i++) {
            std::vector<Value> comp;
            for (auto &x : sp)
                comp.push_back(tupleAt(x, i));
            out.push_back(splineValue(t, comp));
        }
        return mkTuple(out);
    }
    if (sp[0].isNone())
        return sp.back();
    double rv;
    if (sp.size() == 2)
        rv = (1 - t) * sp[0].num() + t * sp[1].num();
    else if (sp.size() == 3)
        rv = (1 - t) * (1 - t) * sp[0].num() + 2 * t * (1 - t) * sp[1].num() + t * t * sp[2].num();
    else
        rv = std::pow(1 - t, 3) * sp[0].num() + 3 * t * std::pow(1 - t, 2) * sp[1].num() +
             3 * t * t * (1 - t) * sp[2].num() + t * t * t * sp[3].num();
    return sp.back().t == T::Float ? Value::real(rv) : Value::integer((int64_t)rv);
}

static AtlResult execStmt(TransformDisp &trans, const AtlStmtP &s, double st, const AtlStateP &state,
                          const std::string &event) {
    AtlResult r;
    switch (s->k) {
    case AtlStmt::BLOCK:
        return execBlock(trans, *s->blocks[0], st, state, event);
    case AtlStmt::INTERP: {
        double complete = s->duration > 0 ? std::min(1.0, st / s->duration) : 1.0;
        complete = callWarper(s->warper, complete);
        AtlStateP S = state;
        if (!S || !S->interpInit) {
            S = std::make_shared<AtlState>();
            S->interpInit = true;
            TransformState newts;
            newts.takeState(trans.state);
            // take_state copies placement into defaults; mirror Ren'Py: the new state's positions are
            // those set by the properties, defaults are the old placement
            for (auto &p : s->props)
                newts.set(p.first, p.second);
            stateDiff(trans.state, newts, S->linear);
            for (auto &sp : s->splines) {
                std::vector<Value> vals{trans.state.get(sp.first)};
                vals.insert(vals.end(), sp.second.begin(), sp.second.end());
                S->splines.emplace_back(sp.first, vals);
            }
            for (auto &p : s->props) {
                bool inLinear = false;
                for (auto &l : S->linear)
                    if (std::get<0>(l) == p.first)
                        inLinear = true;
                if (!inLinear)
                    trans.state.set(p.first, p.second);
            }
        }
        std::vector<PType> types;
        for (auto &l : S->linear) {
            propTypes(std::get<0>(l), types);
            trans.state.set(std::get<0>(l), interpolate(complete, std::get<1>(l), std::get<2>(l), types));
        }
        for (auto &sp : S->splines)
            trans.state.set(sp.first, splineValue(complete, sp.second));
        if (st >= s->duration) {
            r.action = A_NEXT;
            r.arg = st - s->duration;
        } else {
            r.action = A_CONTINUE;
            r.state = S;
            r.pause = (s->props.empty() && s->splines.empty()) ? s->duration - st : 0;
        }
        return r;
    }
    case AtlStmt::CHILD: {
        DispP newChild = toDisp(s->childValue);
        DispP oldChild = trans.rawChild;
        DispP child = newChild;
        if (oldChild && !s->transition.isNone()) {
            auto tr = makeTransition(s->transition, oldChild, newChild);
            if (tr)
                child = tr;
        }
        trans.setChild(child);
        trans.rawChild = newChild;
        r.action = A_NEXT;
        r.arg = st;
        return r;
    }
    case AtlStmt::REPEAT:
        r.action = A_REPEAT;
        r.repeatCount = s->repeats;
        r.arg = st;
        r.pause = 0;
        return r;
    case AtlStmt::PARALLEL: {
        AtlStateP S = state;
        if (!S || !S->parInit) {
            S = std::make_shared<AtlState>();
            S->parInit = true;
            for (auto &b : s->blocks)
                S->par.emplace_back(b, nullptr);
        } else {
            S = std::make_shared<AtlState>(*state);
        }
        std::vector<std::pair<AtlBlockP, AtlStateP>> newState;
        double minPause = 1e9, minLeft = 1e9;
        bool anyPause = false;
        for (auto &pb : S->par) {
            AtlResult cr = execBlock(trans, *pb.first, st, pb.second, event);
            if (cr.pause >= 0) {
                minPause = std::min(minPause, cr.pause);
                anyPause = true;
            }
            if (cr.action == A_CONTINUE)
                newState.emplace_back(pb.first, cr.state);
            else if (cr.action == A_NEXT)
                minLeft = std::min(minLeft, cr.arg);
            else if (cr.action == A_EVENT)
                return cr;
        }
        if (!newState.empty()) {
            auto NS = std::make_shared<AtlState>();
            NS->parInit = true;
            NS->par = newState;
            r.action = A_CONTINUE;
            r.state = NS;
            r.pause = anyPause ? minPause : -1;
        } else {
            r.action = A_NEXT;
            r.arg = minLeft < 1e9 ? minLeft : 0;
        }
        return r;
    }
    case AtlStmt::CHOICE: {
        AtlBlockP choice;
        AtlStateP cstate;
        if (!state || !state->choice) {
            double total = 0;
            for (auto &ch : s->choices)
                total += ch.first;
            static std::mt19937 g(4242);
            double n = std::uniform_real_distribution<double>(0, total)(g);
            for (auto &ch : s->choices) {
                choice = ch.second;
                if (n < ch.first)
                    break;
                n -= ch.first;
            }
        } else {
            choice = state->choice;
            cstate = state->child;
        }
        if (!choice) {
            r.action = A_NEXT;
            r.arg = st;
            return r;
        }
        AtlResult cr = execBlock(trans, *choice, st, cstate, event);
        if (cr.action == A_CONTINUE) {
            auto NS = std::make_shared<AtlState>();
            NS->choice = choice;
            NS->child = cr.state;
            r.action = A_CONTINUE;
            r.state = NS;
            r.pause = cr.pause;
            return r;
        }
        cr.pause = -1;
        return cr;
    }
    case AtlStmt::FUNCTION: {
        Value fr;
        try {
            trans.syncToProxy();
            fr = PY.call(s->function, {trans.proxy, Value::real(st), Value::real(trans.at)});
            trans.syncFromProxy();
        } catch (PyError &e) {
            logf("ATL function error: %s %s", e.type.c_str(), e.msg.c_str());
        }
        if (!fr.isNone()) {
            r.action = A_CONTINUE;
            r.pause = fr.num();
            r.state = std::make_shared<AtlState>();
        } else {
            r.action = A_NEXT;
            r.arg = 0;
        }
        return r;
    }
    case AtlStmt::TIME:
        r.action = A_CONTINUE;
        r.state = std::make_shared<AtlState>();
        return r;
    case AtlStmt::EVENT:
        r.action = A_EVENT;
        r.event = s->eventName;
        r.arg = st;
        return r;
    case AtlStmt::ON: {
        std::string name = state ? state->onName : "start";
        double start = state ? state->onStart : st;
        AtlStateP cstate = state ? state->child : nullptr;
        if (!event.empty() && s->handlers.count(event) && name != "hide") {
            name = event;
            start = st;
            cstate = nullptr;
        }
        for (int guard = 0; guard < 100; guard++) {
            auto it = s->handlers.find(name);
            if (it == s->handlers.end()) {
                auto NS = std::make_shared<AtlState>();
                NS->onName = name;
                NS->onStart = start;
                NS->child = cstate;
                r.action = A_CONTINUE;
                r.state = NS;
                return r;
            }
            AtlResult cr = execBlock(trans, *it->second, st - start, cstate, event);
            if (cr.action == A_CONTINUE) {
                auto NS = std::make_shared<AtlState>();
                NS->onName = name;
                NS->onStart = start;
                NS->child = cr.state;
                r.action = A_CONTINUE;
                r.state = NS;
                r.pause = cr.pause;
                return r;
            } else if (cr.action == A_NEXT) {
                name = (name == "default" || name == "hide" || name == "replaced") ? std::string("#none") : "default";
                start = st - cr.arg;
                cstate = nullptr;
            } else if (cr.action == A_EVENT) {
                name = cr.event;
                if (s->handlers.count(name)) {
                    start = std::max(st - cr.arg, st - 30);
                    cstate = nullptr;
                    continue;
                }
                return cr;
            } else {
                break;
            }
        }
        r.action = A_NEXT;
        return r;
    }
    }
    r.action = A_NEXT;
    return r;
}

// ------------------------------------------------------------------ transform displayable

static std::vector<std::string> kProxyProps = {"alpha", "rotate", "zoom", "xzoom", "yzoom", "xpos", "ypos", "xanchor",
                                               "yanchor", "xoffset", "yoffset", "subpixel", "crop", "size", "xalign",
                                               "yalign", "additive", "rotate_pad", "corner1", "corner2"};

void TransformDisp::syncToProxy() {
    if (proxy.isNone()) {
        proxy = mkInst("#transform_proxy");
    }
    InstObj *p = proxy.inst();
    p->attrs.clear();
    p->native = std::shared_ptr<TransformDisp>(this, [](TransformDisp *) {});
    p->attrs["st"] = Value::real(st);
    p->attrs["at"] = Value::real(at);
}

void TransformDisp::syncFromProxy() {}

static void registerProxy() {
    auto &m = PY.methods["#transform_proxy"];
    m["__setattr__"] = [](Interp &, CallArgs &a) {
        InstObj *p = a.pos[0].inst();
        auto t = std::static_pointer_cast<TransformDisp>(p->native);
        const std::string &name = a.pos[1].s();
        if (t) {
            if (name == "child")
                t->setChild(toDisp(a.pos[2]));
            else
                t->state.set(name, a.pos[2]);
        }
        return Value();
    };
    m["__getattr__"] = [](Interp &, CallArgs &a) {
        InstObj *p = a.pos[0].inst();
        auto t = std::static_pointer_cast<TransformDisp>(p->native);
        const std::string &name = a.pos[1].s();
        if (!t)
            return Value();
        if (name == "st")
            return Value::real(t->st);
        if (name == "at")
            return Value::real(t->at);
        return t->state.get(name);
    };
}

void TransformDisp::setChild(const DispP &c) {
    child = c;
    childStBase = st;
}

void TransformDisp::updateState() {
    if (block || !atlRaw.isNone()) {
        if (!atlDone) {
            if (!block)
                block = compileBlock(atlRaw, context);
            double timebase = atlAnimation ? at : st;
            AtlResult r = execBlock(*this, *block, timebase, atlState, "");
            if (r.action == A_CONTINUE)
                atlState = r.state;
            else
                atlDone = true;
        }
    } else if (!function.isNone()) {
        try {
            syncToProxy();
            PY.call(function, {proxy, Value::real(st), Value::real(at)});
        } catch (PyError &e) {
            logf("transform function error: %s %s", e.type.c_str(), e.msg.c_str());
        }
    } else {
        for (auto &kw : kwargs)
            state.set(kw.first, kw.second);
    }
    active = true;
    if (child) {
        Placement p = child->placement();
        if (p.xpos.set)
            state.defaultXpos = p.xpos;
        if (p.xanchor.set)
            state.defaultXanchor = p.xanchor;
        if (p.ypos.set)
            state.defaultYpos = p.ypos;
        if (p.yanchor.set)
            state.defaultYanchor = p.yanchor;
        state.subpixel = state.subpixel || p.subpixel;
    }
}

Placement TransformDisp::placement() {
    if (!active)
        updateState();
    double cxo = 0, cyo = 0;
    if (child) {
        Placement cp = child->placement();
        cxo = cp.xoffset;
        cyo = cp.yoffset;
    }
    Placement p;
    p.xpos = state.xpos.set ? state.xpos : state.defaultXpos;
    p.ypos = state.ypos.set ? state.ypos : state.defaultYpos;
    p.xanchor = state.xanchor.set ? state.xanchor : state.defaultXanchor;
    p.yanchor = state.yanchor.set ? state.yanchor : state.defaultYanchor;
    p.xoffset = state.xoffset + cxo;
    p.yoffset = state.yoffset + cyo;
    p.subpixel = state.subpixel;
    return p;
}

RenderP TransformDisp::render(float widtho, float heighto, double st_, double at_) {
    if (st_ + stOffset <= st)
        stOffset = st - st_;
    if (at_ + atOffset <= at)
        atOffset = at - at_;
    st = st_ + stOffset;
    at = at_ + atOffset;
    updateState();
    DispP c = child ? child : makeNull();
    if (!child)
        child = c;
    if (ListObj *sz = state.size.list()) {
        widtho = (float)tupleAt(state.size, 0).num();
        heighto = (float)tupleAt(state.size, 1).num();
        (void)sz;
    }
    RenderP cr = renderChild(c, widtho, heighto, st - childStBase, at);
    double width = cr->w, height = cr->h;
    childW = (float)width;
    childH = (float)height;
    double rxdx = 1, rxdy = 0, rydx = 0, rydy = 1;
    double xo = 0, yo = 0;
    bool clipping = false;

    Value crop = state.crop;
    if (crop.isNone() && state.corner1.list() && state.corner2.list()) {
        double x1 = tupleAt(state.corner1, 0).num(), y1 = tupleAt(state.corner1, 1).num();
        double x2 = tupleAt(state.corner2, 0).num(), y2 = tupleAt(state.corner2, 1).num();
        if (x1 > x2)
            std::swap(x1, x2);
        if (y1 > y2)
            std::swap(y1, y2);
        crop = mkTuple({Value::real(x1), Value::real(y1), Value::real(x2 - x1), Value::real(y2 - y1)});
    }
    if (crop.list()) {
        double nx = tupleAt(crop, 0).num(), ny = tupleAt(crop, 1).num();
        width = tupleAt(crop, 2).num();
        height = tupleAt(crop, 3).num();
        if (state.rotate.isNum() && state.rotate.num() != 0) {
            auto clip = Render::make((float)width, (float)height);
            clip->blit(cr, (float)-nx, (float)-ny);
            clip->clipping = true;
            cr = clip;
        } else {
            xo = -nx;
            yo = -ny;
            clipping = true;
        }
    }
    if (state.size.list()) {
        double nw = tupleAt(state.size, 0).num(), nh = tupleAt(state.size, 1).num();
        if (nw != width || nh != height) {
            double xz = width > 0 ? nw / width : 1, yz = height > 0 ? nh / height : 1;
            rxdx = xz;
            rydy = yz;
            xo *= xz;
            yo *= yz;
            width = nw;
            height = nh;
        }
    }
    double xzoom = state.zoom * state.xzoom, yzoom = state.zoom * state.yzoom;
    if (xzoom != 1) {
        rxdx *= xzoom;
        width *= std::fabs(xzoom);
        xo *= xzoom;
        if (xzoom < 0)
            xo += width;
    }
    if (yzoom != 1) {
        rydy *= yzoom;
        height *= std::fabs(yzoom);
        yo *= yzoom;
        if (yzoom < 0)
            yo += height;
    }
    if (state.rotate.isNum()) {
        double cw = width, ch = height;
        double angle = state.rotate.num() * 3.1415926535897931 / 180;
        double cosa = std::cos(angle), sina = std::sin(angle);
        rxdy = rydy * -sina;
        rydx = rxdx * sina;
        rxdx *= cosa;
        rydy *= cosa;
        double px = cw / 2.0, py = ch / 2.0;
        if (xzoom < 0)
            px = -px;
        if (yzoom < 0)
            py = -py;
        if (state.rotatePad) {
            width = height = std::hypot(cw, ch);
            xo = -px * cosa + py * sina;
            yo = -px * sina - py * cosa;
        } else {
            xo = -px * cosa + py * sina;
            yo = -px * sina - py * cosa;
            double x2 = -px * cosa - py * sina, y2 = -px * sina + py * cosa;
            double x3 = px * cosa - py * sina, y3 = px * sina + py * cosa;
            double x4 = px * cosa + py * sina, y4 = px * sina - py * cosa;
            width = std::max({xo, x2, x3, x4}) - std::min({xo, x2, x3, x4});
            height = std::max({yo, y2, y3, y4}) - std::min({yo, y2, y3, y4});
        }
        xo += width / 2.0;
        yo += height / 2.0;
    }
    auto rv = Render::make((float)width, (float)height);
    if (!(rxdx == 1 && rxdy == 0 && rydx == 0 && rydy == 1)) {
        rv->hasReverse = true;
        rv->rxdx = (float)rxdx;
        rv->rxdy = (float)rxdy;
        rv->rydx = (float)rydx;
        rv->rydy = (float)rydy;
    }
    revXdx = (float)rxdx;
    revXdy = (float)rxdy;
    revYdx = (float)rydx;
    revYdy = (float)rydy;
    rv->alpha = (float)state.alpha;
    rv->clipping = clipping;
    if (state.subpixel)
        rv->blit(cr, (float)xo, (float)yo);
    else
        rv->blit(cr, (float)std::floor(xo + 0.5), (float)std::floor(yo + 0.5));
    renderW = (float)width;
    renderH = (float)height;
    return rv;
}

std::shared_ptr<TransformDisp> TransformDisp::copyWith(const DispP &newChild) const {
    auto t = std::make_shared<TransformDisp>();
    t->kind = kind;
    t->style = style;
    t->child = newChild ? newChild : child;
    t->function = function;
    t->kwargs = kwargs;
    t->atlRaw = atlRaw;
    t->context = context;
    t->parameters = parameters;
    t->atlAnimation = atlAnimation;
    t->state.takeState(state);
    return t;
}

// Transform / ATLTransform instance from the data files
static TransformP templateFromInst(InstObj *in) {
    auto t = std::make_shared<TransformDisp>();
    if (in->cls == "renpy.display.motion.ATLTransform") {
        t->kind = "ATLTransform";
        t->atlRaw = in->get("atl");
        Value ctx = in->get("context");
        t->context = ctx.inst() ? ctx.inst()->get("context") : ctx;
        if (t->context.isNone())
            t->context = mkDict();
        t->parameters = in->get("parameters");
        if (InstObj *a = t->atlRaw.inst())
            t->atlAnimation = a->get("animation").truthy();
    } else {
        t->kind = "Transform";
        t->function = in->get("function");
        if (DictObj *kw = in->get("kwargs").dict())
            for (auto &kv : kw->items)
                t->kwargs.emplace_back(kv.first.s(), kv.second);
    }
    Value child = in->get("child");
    if (!child.isNone())
        t->child = toDisp(child);
    return t;
}

DispP makeTransformFromInst(InstObj *in, const Value &) {
    TransformP t = templateFromInst(in);
    return t;
}

DispP applyTransform(const Value &transform, const DispP &child) {
    if (DispP d = valueDisp(transform)) {
        if (d->isTransform())
            return std::static_pointer_cast<TransformDisp>(d)->copyWith(child);
    }
    InstObj *in = transform.inst();
    if (in && (in->cls == "renpy.display.motion.ATLTransform" || in->cls == "renpy.display.motion.Transform")) {
        TransformP tmpl = templateFromInst(in);
        return tmpl->copyWith(child);
    }
    try {
        Value r = PY.call(transform, {dispValue(child)});
        DispP d = toDisp(r);
        return d ? d : child;
    } catch (PyError &e) {
        logf("applyTransform error: %s %s", e.type.c_str(), e.msg.c_str());
        return child;
    }
}

// ATLTransform.__call__(*args, **kwargs): binds parameters, returns a new transform
static Value callAtlTransform(const TransformP &base, CallArgs &a) {
    auto t = base->copyWith(nullptr);
    Value ctx = mkDict();
    if (DictObj *d = base->context.dict())
        for (auto &kv : d->items)
            ctx.dict()->set(kv.first, kv.second);
    std::vector<std::string> positional;
    if (InstObj *pi = base->parameters.inst()) {
        if (ListObj *pl = pi->get("parameters").list())
            for (auto &p : pl->v) {
                Value name = tupleAt(p, 0), def = tupleAt(p, 1);
                if (!def.isNone())
                    ctx.dict()->set(name, def.t == T::Code ? PY.eval((int)def.i) : def);
            }
        if (ListObj *pos = pi->get("positional").list())
            for (auto &p : pos->v)
                positional.push_back(p.isStr() ? p.s() : valueStr(tupleAt(p, 0)));
    }
    std::vector<Value> args(a.pos.begin() + 1, a.pos.end()); // pos[0] = self
    DispP child;
    if (positional.empty() && !args.empty()) {
        child = toDisp(args[0]);
        args.erase(args.begin());
    }
    while (!positional.empty() && !args.empty()) {
        ctx.dict()->set(Value::str(positional.front()), args.front());
        positional.erase(positional.begin());
        args.erase(args.begin());
    }
    for (auto &kw : a.kw) {
        auto it = std::find(positional.begin(), positional.end(), kw.first);
        if (it != positional.end()) {
            positional.erase(it);
            ctx.dict()->set(Value::str(kw.first), kw.second);
        } else if (kw.first == "child") {
            child = toDisp(kw.second);
        } else {
            ctx.dict()->set(Value::str(kw.first), kw.second);
        }
    }
    t->context = ctx;
    Value np = mkInst("ParameterInfo");
    std::vector<Value> posv;
    for (auto &p : positional)
        posv.push_back(Value::str(p));
    np.inst()->attrs["positional"] = mkList(posv);
    t->parameters = np;
    if (child)
        t->child = child;
    else if (base->child)
        t->child = base->child;
    return dispValue(t, "renpy.display.motion.ATLTransform#native");
}

static Value callTransform(const TransformP &base, CallArgs &a) {
    DispP child;
    if (a.pos.size() > 1)
        child = toDisp(a.pos[1]);
    for (auto &kw : a.kw)
        if (kw.first == "child")
            child = toDisp(kw.second);
    return dispValue(base->copyWith(child), "renpy.display.motion.Transform#native");
}

void registerTransformNatives(Interp &I) {
    registerProxy();
    I.instCall["renpy.display.motion.ATLTransform"] = [](Interp &, CallArgs &a) {
        TransformP base = templateFromInst(a.pos[0].inst());
        return callAtlTransform(base, a);
    };
    auto nativeCall = [](Interp &, CallArgs &a) {
        auto t = std::static_pointer_cast<TransformDisp>(valueDisp(a.pos[0]));
        if (!t)
            return Value();
        if (!t->atlRaw.isNone())
            return callAtlTransform(t, a);
        return callTransform(t, a);
    };
    I.instCall["renpy.display.motion.ATLTransform#native"] = nativeCall;
    I.instCall["renpy.display.motion.Transform#native"] = nativeCall;
    I.instCall["renpy.display.motion.Transform"] = [](Interp &, CallArgs &a) {
        TransformP base = templateFromInst(a.pos[0].inst());
        return callTransform(base, a);
    };
    // Transform(child=None, function=None, **kwargs)
    I.registerNative("renpy.display.motion.Transform", [](Interp &, CallArgs &a) {
        auto t = std::make_shared<TransformDisp>();
        t->kind = "Transform";
        if (a.pos.size() > 0)
            t->child = toDisp(a.pos[0]);
        if (a.pos.size() > 1)
            t->function = a.pos[1];
        for (auto &kw : a.kw) {
            if (kw.first == "child")
                t->child = toDisp(kw.second);
            else if (kw.first == "function")
                t->function = kw.second;
            else if (kw.first != "style" && kw.first != "focus" && kw.first != "default")
                t->kwargs.emplace_back(kw.first, kw.second);
        }
        for (auto &kw : t->kwargs)
            t->state.set(kw.first, kw.second);
        return dispValue(t, "renpy.display.motion.Transform#native");
    });
    I.natives["renpy.display.layout.Position"] = I.natives["renpy.display.motion.Transform"];
}
