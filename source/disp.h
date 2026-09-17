#pragma once
#include "gfx.h"
#include "py.h"
#include "texture.h"
#include <limits>
#include <memory>

// ------------------------------------------------------------------ render tree (like renpy.display.render.Render)

struct Render;
using RenderP = std::shared_ptr<Render>;

struct Render {
    enum Op : uint8_t { NODE, TEX, SOLID, DISSOLVE, IMAGEDISSOLVE, TEXT, CUSTOM };
    Op op = NODE;
    float w = 0, h = 0;
    struct Child {
        RenderP r;
        float x, y;
    };
    std::vector<Child> children;
    bool hasReverse = false;
    float rxdx = 1, rxdy = 0, rydx = 0, rydy = 1; // child pixel -> parent (Ren'Py "reverse")
    float alpha = 1;
    bool clipping = false;

    // TEX: texture quad of size w x h
    TexImage *tex = nullptr;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    CMat cm;
    // SOLID
    float r = 0, g = 0, b = 0, a = 1;
    // DISSOLVE / IMAGEDISSOLVE: children[0] old, [1] new, [2] control
    float complete = 0;
    int ramp = 8;
    bool opAlpha = false;
    // TEXT / CUSTOM draw callback (drawn with the accumulated matrix)
    std::function<void(const Mat &, float alpha)> draw;

    static RenderP make(float w, float h) {
        auto r = std::make_shared<Render>();
        r->w = w;
        r->h = h;
        return r;
    }
    void blit(const RenderP &c, float x, float y) {
        if (c)
            children.push_back({c, x, y});
    }
};

// Draws a render tree into the currently bound target.
void drawRender(const RenderP &r, const Mat &m, float alpha);
// Renders a tree into a pooled target (w x h virtual pixels); caller releases the target.
RenderTarget *renderToTarget(const RenderP &r, bool opaque);

// ------------------------------------------------------------------ placement

struct Pos {
    double v = 0;
    bool set = false;
    bool rel = false; // float (relative) vs int (absolute)
    static Pos none() { return Pos(); }
    static Pos absolute(double x) {
        Pos p;
        p.v = x;
        p.set = true;
        return p;
    }
    static Pos relative(double x) {
        Pos p;
        p.v = x;
        p.set = true;
        p.rel = true;
        return p;
    }
    static Pos from(const Value &v);
    Value toValue() const;
    double resolve(double size) const { return set ? (rel ? v * size : v) : 0; }
};

struct Placement {
    Pos xpos, ypos, xanchor, yanchor;
    double xoffset = 0, yoffset = 0;
    bool subpixel = false;
};

// ------------------------------------------------------------------ displayables

struct Displayable;
using DispP = std::shared_ptr<Displayable>;

struct StyleProps {
    Placement place;
    Value xmaximum, ymaximum, xminimum, yminimum;
    bool xfill = false, yfill = false;
    double spacing = 0;
    Value background;
    double leftPad = 0, rightPad = 0, topPad = 0, bottomPad = 0;
    double leftMargin = 0, rightMargin = 0, topMargin = 0, bottomMargin = 0;
    void apply(const std::string &name, const Value &v);
};

struct Displayable : std::enable_shared_from_this<Displayable> {
    StyleProps style;
    std::string kind;
    virtual ~Displayable() {}
    virtual RenderP render(float w, float h, double st, double at) = 0;
    virtual Placement placement() { return style.place; }
    virtual void children(std::vector<DispP> &out) {}
    virtual void predictFiles(std::vector<std::string> &out);
    virtual bool isTransform() const { return false; }
    // Place a rendered child inside an area; returns the offset.
    void place(Render &dest, float x, float y, float w, float h, const RenderP &surf);
};

// Converts a value (string, image manipulator instance, native displayable...) into a fresh displayable tree.
DispP toDisp(const Value &v);
// Wraps a native displayable into a script value (and back).
Value dispValue(const DispP &d, const std::string &cls = "#disp");
DispP valueDisp(const Value &v); // null if the value does not hold a native displayable

// Image registry
void registerImage(const std::string &name, const Value &v);
bool hasImage(const std::string &name);
Value imageValue(const std::string &name);
const std::vector<std::string> &imageNamesWithTag(const std::string &tag);

// Common displayables used by the engine
DispP makeSolid(float r, float g, float b, float a);
DispP makeNull(float w = 0, float h = 0);
DispP makeFixed(const std::vector<DispP> &children);
DispP makeImageFile(const std::string &file);
DispP makeFixedSize(float w, float h, const std::vector<std::pair<std::pair<float, float>, DispP>> &children);

// ------------------------------------------------------------------ transforms / ATL

struct TransformState {
    double alpha = 1, additive = 0;
    Value rotate;          // None or float
    bool rotatePad = true;
    bool transformAnchor = false;
    double zoom = 1, xzoom = 1, yzoom = 1;
    Pos xpos, ypos, xanchor, yanchor;
    double xoffset = 0, yoffset = 0;
    Pos xaround = Pos::relative(0), yaround = Pos::relative(0);
    Value xanchoraround = Value::real(0), yanchoraround = Value::real(0);
    bool subpixel = false;
    Value crop, corner1, corner2, size; // None or tuples
    double delay = 0;
    Pos defaultXpos, defaultYpos, defaultXanchor, defaultYanchor;

    void takeState(const TransformState &o);
    Value get(const std::string &prop) const;
    void set(const std::string &prop, const Value &v);
};

struct AtlBlock;
struct AtlState;

struct TransformDisp : Displayable {
    DispP child;
    TransformState state;
    Value function;             // callable(trans, st, at) or None
    std::vector<std::pair<std::string, Value>> kwargs;
    // ATL
    std::shared_ptr<AtlBlock> block;
    Value atlRaw;               // atl.Block instance
    Value context;              // dict
    Value parameters;           // ParameterInfo (positional names)
    std::shared_ptr<AtlState> atlState;
    bool atlDone = false;
    bool atlAnimation = false;
    DispP rawChild;
    double st = 0, at = 0, stOffset = 0, atOffset = 0, childStBase = 0;
    bool active = false;
    float childW = 0, childH = 0, renderW = 0, renderH = 0;
    float revXdx = 1, revXdy = 0, revYdx = 0, revYdy = 1;
    Value proxy; // python-facing object for function transforms

    bool isTransform() const override { return true; }
    RenderP render(float w, float h, double st, double at) override;
    Placement placement() override;
    void children(std::vector<DispP> &out) override {
        if (child)
            out.push_back(child);
    }
    void updateState();
    void setChild(const DispP &c);
    std::shared_ptr<TransformDisp> copyWith(const DispP &newChild) const; // Transform.__call__
    void syncFromProxy();
    void syncToProxy();
};
using TransformP = std::shared_ptr<TransformDisp>;

// Builds a transform from a Transform / ATLTransform value applied to a child (Ren'Py `at`).
DispP applyTransform(const Value &transform, const DispP &child);

// ------------------------------------------------------------------ transitions

struct TransitionDisp : Displayable {
    double delay = 0;
    DispP oldW, newW;
    bool events = false;
};
// Calls a transition value with old/new widgets. Returns null for None.
std::shared_ptr<TransitionDisp> makeTransition(const Value &trans, const DispP &oldW, const DispP &newW);

// ------------------------------------------------------------------ registration of natives

void registerDisplayNatives(Interp &I);
double callWarper(const Value &warper, double t);
double builtinWarp(const std::string &name, double t, bool &found);

// ------------------------------------------------------------------ layout containers (renpy.display.layout)

struct WindowDisp : Displayable {
    DispP child;
    DispP background; // resolved from style.background
    RenderP render(float w, float h, double st, double at) override;
    void children(std::vector<DispP> &out) override {
        if (child)
            out.push_back(child);
        if (background)
            out.push_back(background);
    }
};

struct BoxDisp : Displayable {
    bool vertical = true;
    std::vector<DispP> kids;
    RenderP render(float w, float h, double st, double at) override;
    void children(std::vector<DispP> &out) override { out.insert(out.end(), kids.begin(), kids.end()); }
};

StyleProps namedStyleProps(const std::string &name);
std::shared_ptr<WindowDisp> makeWindow(const std::string &style, const DispP &child, const Value &overrides = Value());
std::shared_ptr<BoxDisp> makeBox(const std::string &style, bool vertical, const std::vector<DispP> &kids,
                                 const Value &overrides = Value());
