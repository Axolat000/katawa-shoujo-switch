#include "disp.h"
#include "text.h"
#include <algorithm>
#include <cmath>
#include <random>
#include <unordered_map>

static std::mt19937 &drng() {
    static std::mt19937 g(1234567);
    return g;
}
static double urand(double a, double b) { return a + (b - a) * std::uniform_real_distribution<double>(0, 1)(drng()); }

// ------------------------------------------------------------------ positions & styles

Pos Pos::from(const Value &v) {
    if (v.t == T::Float)
        return relative(v.f);
    if (v.isInt())
        return absolute((double)v.i);
    return none();
}

Value Pos::toValue() const {
    if (!set)
        return Value();
    return rel ? Value::real(v) : Value::integer((int64_t)std::llround(v));
}

static double numOr(const Value &v, double def) { return v.isNum() ? v.num() : def; }

void StyleProps::apply(const std::string &k, const Value &v) {
    auto tup = [&](size_t i) -> Value {
        ListObj *l = v.list();
        return (l && i < l->v.size()) ? l->v[i] : Value();
    };
    if (k == "xpos")
        place.xpos = Pos::from(v);
    else if (k == "ypos")
        place.ypos = Pos::from(v);
    else if (k == "xanchor")
        place.xanchor = Pos::from(v);
    else if (k == "yanchor")
        place.yanchor = Pos::from(v);
    else if (k == "xalign") {
        place.xpos = Pos::from(v);
        place.xanchor = Pos::from(v);
    } else if (k == "yalign") {
        place.ypos = Pos::from(v);
        place.yanchor = Pos::from(v);
    } else if (k == "align") {
        place.xpos = place.xanchor = Pos::from(tup(0));
        place.ypos = place.yanchor = Pos::from(tup(1));
    } else if (k == "pos") {
        place.xpos = Pos::from(tup(0));
        place.ypos = Pos::from(tup(1));
    } else if (k == "anchor") {
        place.xanchor = Pos::from(tup(0));
        place.yanchor = Pos::from(tup(1));
    } else if (k == "xcenter") {
        place.xpos = Pos::from(v);
        place.xanchor = Pos::relative(0.5);
    } else if (k == "ycenter") {
        place.ypos = Pos::from(v);
        place.yanchor = Pos::relative(0.5);
    } else if (k == "xoffset")
        place.xoffset = numOr(v, 0);
    else if (k == "yoffset")
        place.yoffset = numOr(v, 0);
    else if (k == "subpixel")
        place.subpixel = v.truthy();
    else if (k == "xmaximum")
        xmaximum = v;
    else if (k == "ymaximum")
        ymaximum = v;
    else if (k == "xminimum")
        xminimum = v;
    else if (k == "yminimum")
        yminimum = v;
    else if (k == "maximum") {
        xmaximum = tup(0);
        ymaximum = tup(1);
    } else if (k == "minimum") {
        xminimum = tup(0);
        yminimum = tup(1);
    } else if (k == "area") {
        place.xpos = Pos::from(tup(0));
        place.ypos = Pos::from(tup(1));
        place.xanchor = Pos::absolute(0);
        place.yanchor = Pos::absolute(0);
        xfill = yfill = true;
        xmaximum = xminimum = tup(2);
        ymaximum = yminimum = tup(3);
    } else if (k == "xfill")
        xfill = v.truthy();
    else if (k == "yfill")
        yfill = v.truthy();
    else if (k == "spacing" || k == "box_spacing")
        spacing = numOr(v, 0);
    else if (k == "background")
        background = v;
    else if (k == "left_padding")
        leftPad = numOr(v, 0);
    else if (k == "right_padding")
        rightPad = numOr(v, 0);
    else if (k == "top_padding")
        topPad = numOr(v, 0);
    else if (k == "bottom_padding")
        bottomPad = numOr(v, 0);
    else if (k == "xpadding")
        leftPad = rightPad = numOr(v, 0);
    else if (k == "ypadding")
        topPad = bottomPad = numOr(v, 0);
    else if (k == "left_margin")
        leftMargin = numOr(v, 0);
    else if (k == "right_margin")
        rightMargin = numOr(v, 0);
    else if (k == "top_margin")
        topMargin = numOr(v, 0);
    else if (k == "bottom_margin")
        bottomMargin = numOr(v, 0);
    else if (k == "xmargin")
        leftMargin = rightMargin = numOr(v, 0);
    else if (k == "ymargin")
        topMargin = bottomMargin = numOr(v, 0);
}

// named styles resolved for the current language (filled by the game at language switch)
static std::unordered_map<std::string, StyleProps> &styleTable() {
    static std::unordered_map<std::string, StyleProps> t;
    return t;
}

static std::unordered_map<std::string, Value> &styleRaw() {
    static std::unordered_map<std::string, Value> t;
    return t;
}

void setStyleTable(const Value &styles) {
    styleTable().clear();
    styleRaw().clear();
    DictObj *d = styles.dict();
    if (!d)
        return;
    for (auto &kv : d->items) {
        StyleProps sp;
        Value props;
        if (DictObj *sd = kv.second.dict())
            if (const Value *p = sd->find(Value::str("props")))
                props = *p;
        if (DictObj *pd = props.dict())
            for (auto &pk : pd->items)
                sp.apply(pk.first.s(), pk.second);
        styleTable()[kv.first.s()] = sp;
        styleRaw()[kv.first.s()] = props;
    }
}

Value styleProperty(const std::string &style, const std::string &prop) {
    auto it = styleRaw().find(style);
    if (it == styleRaw().end())
        return Value();
    if (DictObj *d = it->second.dict())
        if (const Value *v = d->find(Value::str(prop)))
            return *v;
    return Value();
}

static StyleProps namedStyle(const std::string &name) {
    auto it = styleTable().find(name);
    if (it != styleTable().end())
        return it->second;
    return StyleProps();
}

// Resolves a style value (Style instance, style name) plus keyword properties.
static StyleProps resolveStyle(const Value &style, const std::string &defaultName) {
    if (style.isStr())
        return namedStyle(style.s());
    InstObj *in = style.inst();
    if (!in)
        return namedStyle(defaultName);
    StyleProps sp;
    Value parent = in->get("parent");
    Value name = in->get("name");
    if (ListObj *pl = parent.list()) {
        if (!pl->v.empty())
            sp = namedStyle(pl->v[0].s());
    } else if (ListObj *nl = name.list()) {
        if (!nl->v.empty())
            sp = namedStyle(nl->v[0].s());
    } else {
        sp = namedStyle(defaultName);
    }
    if (ListObj *props = in->get("properties").list())
        for (auto &p : props->v)
            if (DictObj *d = p.dict())
                for (auto &kv : d->items)
                    sp.apply(kv.first.s(), kv.second);
    return sp;
}

// ------------------------------------------------------------------ render drawing

static void drawNode(const RenderP &r, const Mat &m, float alpha);

// Render-to-texture nests: remember the outer binding so we can restore it.
static std::vector<std::function<void()>> g_bindStack;

void pushBinding(std::function<void()> rebind) { g_bindStack.push_back(std::move(rebind)); }
void popBinding() {
    if (!g_bindStack.empty())
        g_bindStack.pop_back();
}

static void rebindCurrent() {
    if (!g_bindStack.empty())
        g_bindStack.back()();
}

RenderTarget *renderToTarget(const RenderP &r, bool opaque) {
    float ps = gfx::pixelScale();
    float vw = std::max(1.0f, r->w), vh = std::max(1.0f, r->h);
    int pw = std::max(1, (int)std::ceil(vw * ps)), ph = std::max(1, (int)std::ceil(vh * ps));
    RenderTarget *rt = gfx::acquireTarget(pw, ph);
    gfx::bindTarget(rt, vw, vh);
    if (opaque)
        gfx::clear(0, 0, 0, 1);
    else
        gfx::clear(0, 0, 0, 0);
    pushBinding([rt, vw, vh]() { gfx::bindTarget(rt, vw, vh); });
    drawNode(r, Mat(), 1.0f);
    popBinding();
    return rt;
}

static RenderTarget *childToTarget(const RenderP &r, bool opaque) {
    RenderTarget *rt = renderToTarget(r, opaque);
    rebindCurrent();
    return rt;
}

static void drawNode(const RenderP &r, const Mat &m, float alpha) {
    if (!r)
        return;
    switch (r->op) {
    case Render::TEX:
        if (r->tex && texture::ensure(r->tex))
            gfx::drawTexture(r->tex->tex, matMul(m, matRect(0, 0, r->w, r->h)), r->u0, r->v0, r->u1, r->v1,
                             alpha * r->alpha, &r->cm);
        return;
    case Render::SOLID:
        gfx::drawSolid(matMul(m, matRect(0, 0, r->w, r->h)), r->r, r->g, r->b, r->a * alpha * r->alpha);
        return;
    case Render::TEXT:
    case Render::CUSTOM:
        if (r->draw)
            r->draw(m, alpha * r->alpha);
        break;
    case Render::DISSOLVE:
    case Render::IMAGEDISSOLVE: {
        if (r->children.size() < 2)
            return;
        float a = alpha * r->alpha;
        // capture the current binding for restoration after offscreen passes
        Mat mm = m;
        RenderTarget *ta = childToTarget(r->children[0].r, !r->opAlpha);
        RenderTarget *tb = childToTarget(r->children[1].r, !r->opAlpha);
        RenderTarget *tc = nullptr;
        if (r->op == Render::IMAGEDISSOLVE && r->children.size() > 2)
            tc = childToTarget(r->children[2].r, false);
        Mat quad = matMul(mm, matRect(0, 0, r->w, r->h));
        if (r->op == Render::DISSOLVE)
            gfx::drawBlend(ta->tex, tb->tex, quad, a, r->complete);
        else if (tc)
            gfx::drawImageBlend(ta->tex, tb->tex, tc->tex, quad, a, r->complete, r->ramp);
        gfx::releaseTarget(ta);
        gfx::releaseTarget(tb);
        gfx::releaseTarget(tc);
        return;
    }
    default:
        break;
    }
    float a = alpha * r->alpha;
    if (a <= 0.003f)
        return;
    bool clipped = false;
    if (r->clipping) {
        if (matAxisAligned(m)) {
            float x0, y0, x1, y1;
            matApply(m, 0, 0, x0, y0);
            matApply(m, r->w, r->h, x1, y1);
            gfx::pushClip(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
            clipped = true;
        }
    }
    for (auto &c : r->children) {
        Mat cm = matMul(m, matTranslate(c.x, c.y));
        if (r->hasReverse)
            cm = matMul(cm, matLinear(r->rxdx, r->rxdy, r->rydx, r->rydy));
        drawNode(c.r, cm, a);
    }
    if (clipped)
        gfx::popClip();
}

void drawRender(const RenderP &r, const Mat &m, float alpha) { drawNode(r, m, alpha); }

// ------------------------------------------------------------------ helpers

static RenderP renderDisp(const DispP &d, float w, float h, double st, double at) {
    if (!d)
        return Render::make(0, 0);
    const StyleProps &s = d->style;
    if (s.xmaximum.isNum())
        w = s.xmaximum.t == T::Float ? w * (float)s.xmaximum.f : std::min(w, (float)s.xmaximum.num());
    if (s.ymaximum.isNum())
        h = s.ymaximum.t == T::Float ? h * (float)s.ymaximum.f : std::min(h, (float)s.ymaximum.num());
    w = std::max(0.0f, w);
    h = std::max(0.0f, h);
    return d->render(w, h, st, at);
}

RenderP renderChild(const DispP &d, float w, float h, double st, double at) { return renderDisp(d, w, h, st, at); }

void Displayable::place(Render &dest, float x, float y, float w, float h, const RenderP &surf) {
    Placement p = placement();
    double xpos = p.xpos.set ? (p.xpos.rel ? p.xpos.v * w : p.xpos.v) : 0;
    double xanchor = p.xanchor.set ? (p.xanchor.rel ? p.xanchor.v * surf->w : p.xanchor.v) : 0;
    double ypos = p.ypos.set ? (p.ypos.rel ? p.ypos.v * h : p.ypos.v) : 0;
    double yanchor = p.yanchor.set ? (p.yanchor.rel ? p.yanchor.v * surf->h : p.yanchor.v) : 0;
    double px = xpos + x + p.xoffset - xanchor;
    double py = ypos + y + p.yoffset - yanchor;
    if (!p.subpixel) {
        px = std::floor(px + 0.5);
        py = std::floor(py + 0.5);
    }
    dest.blit(surf, (float)px, (float)py);
}

void Displayable::predictFiles(std::vector<std::string> &out) {
    std::vector<DispP> kids;
    children(kids);
    for (auto &k : kids)
        if (k)
            k->predictFiles(out);
}

// ------------------------------------------------------------------ image manipulators

struct ImLayer {
    TexImage *tex = nullptr;
    Mat m; // unit quad -> manipulator pixels
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    CMat cm;
    bool solid = false;
    float sr = 0, sg = 0, sb = 0, sa = 0;
};

struct ImSpec {
    float w = 0, h = 0;
    std::vector<ImLayer> layers;
};
using ImSpecP = std::shared_ptr<ImSpec>;

static ImSpecP imSpec(const Value &v);

static std::unordered_map<std::string, ImSpecP> &fileSpecs() {
    static std::unordered_map<std::string, ImSpecP> m;
    return m;
}

static ImSpecP fileSpec(const std::string &file) {
    auto &cache = fileSpecs();
    auto it = cache.find(file);
    if (it != cache.end())
        return it->second;
    auto sp = std::make_shared<ImSpec>();
    ImLayer L;
    L.tex = texture::get(file);
    if (!L.tex->known && L.tex->state == 0)
        texture::ensure(L.tex);
    sp->w = (float)L.tex->w;
    sp->h = (float)L.tex->h;
    L.m = matRect(0, 0, sp->w, sp->h);
    sp->layers.push_back(L);
    cache[file] = sp;
    return sp;
}

static void layerBounds(const ImLayer &L, float &x0, float &y0, float &x1, float &y1) {
    float xs[4], ys[4];
    matApply(L.m, 0, 0, xs[0], ys[0]);
    matApply(L.m, 1, 0, xs[1], ys[1]);
    matApply(L.m, 0, 1, xs[2], ys[2]);
    matApply(L.m, 1, 1, xs[3], ys[3]);
    x0 = *std::min_element(xs, xs + 4);
    x1 = *std::max_element(xs, xs + 4);
    y0 = *std::min_element(ys, ys + 4);
    y1 = *std::max_element(ys, ys + 4);
}

static double tupNum(const Value &t, size_t i, double def = 0) {
    ListObj *l = t.list();
    return (l && i < l->v.size() && l->v[i].isNum()) ? l->v[i].num() : def;
}

static CMat matrixFromValue(const Value &mv) {
    std::vector<double> vals;
    if (ListObj *l = mv.list())
        for (auto &x : l->v)
            vals.push_back(x.num());
    return CMat::fromList(vals.data(), (int)vals.size());
}

static ImSpecP cropSpec(const ImSpecP &child, float cx, float cy, float cw, float ch) {
    auto sp = std::make_shared<ImSpec>();
    sp->w = cw;
    sp->h = ch;
    for (const ImLayer &L0 : child->layers) {
        ImLayer L = L0;
        if (matAxisAligned(L.m)) {
            float x0, y0, x1, y1;
            layerBounds(L, x0, y0, x1, y1);
            float nx0 = std::max(x0, cx), ny0 = std::max(y0, cy);
            float nx1 = std::min(x1, cx + cw), ny1 = std::min(y1, cy + ch);
            if (nx1 <= nx0 || ny1 <= ny0)
                continue;
            bool flipX = L.m.a < 0, flipY = L.m.d < 0;
            float fx0 = (nx0 - x0) / (x1 - x0), fx1 = (nx1 - x0) / (x1 - x0);
            float fy0 = (ny0 - y0) / (y1 - y0), fy1 = (ny1 - y0) / (y1 - y0);
            if (flipX) {
                float t0 = 1 - fx1, t1 = 1 - fx0;
                fx0 = t0;
                fx1 = t1;
            }
            if (flipY) {
                float t0 = 1 - fy1, t1 = 1 - fy0;
                fy0 = t0;
                fy1 = t1;
            }
            float du = L.u1 - L.u0, dv = L.v1 - L.v0;
            float nu0 = L.u0 + du * fx0, nu1 = L.u0 + du * fx1;
            float nv0 = L.v0 + dv * fy0, nv1 = L.v0 + dv * fy1;
            L.u0 = nu0;
            L.u1 = nu1;
            L.v0 = nv0;
            L.v1 = nv1;
            float w = nx1 - nx0, h = ny1 - ny0;
            L.m = matRect(nx0 - cx + (flipX ? w : 0), ny0 - cy + (flipY ? h : 0), flipX ? -w : w, flipY ? -h : h);
        } else {
            L.m = matMul(matTranslate(-cx, -cy), L.m);
        }
        sp->layers.push_back(L);
    }
    return sp;
}

static ImSpecP buildSpec(InstObj *in) {
    const std::string &c = in->cls;
    auto child = [&](const char *k) { return imSpec(in->get(k)); };
    if (c == "renpy.display.im.Image")
        return fileSpec(in->get("filename").s());
    if (c == "renpy.display.im.MatrixColor" || c == "renpy.display.im.Recolor" || c == "renpy.display.im.Map" ||
        c == "renpy.display.im.Twocolor") {
        ImSpecP ch = child("image");
        CMat m;
        if (c == "renpy.display.im.MatrixColor") {
            m = matrixFromValue(in->get("matrix"));
        } else if (c == "renpy.display.im.Recolor") {
            double v[20] = {0};
            v[0] = numOr(in->get("rmul"), 256) / 256.0;
            v[6] = numOr(in->get("gmul"), 256) / 256.0;
            v[12] = numOr(in->get("bmul"), 256) / 256.0;
            v[18] = numOr(in->get("amul"), 256) / 256.0;
            m = CMat::fromList(v, 20);
        } else if (c == "renpy.display.im.Map") {
            double v[20] = {0};
            const char *maps[4] = {"rmap", "gmap", "bmap", "amap"};
            for (int i = 0; i < 4; i++) {
                std::string s = in->get(maps[i]).s();
                std::vector<uint32_t> bytes;
                for (size_t p = 0; p < s.size();)
                    bytes.push_back(utf8Decode(s, p));
                double lo = bytes.size() > 0 ? bytes[0] / 255.0 : 0;
                double hi = bytes.size() > 255 ? bytes[255] / 255.0 : 1;
                v[i * 5 + i] = hi - lo;
                v[i * 5 + 4] = lo;
            }
            m = CMat::fromList(v, 20);
        } else {
            double v[20] = {0};
            Value wh = in->get("white"), bl = in->get("black");
            for (int i = 0; i < 4; i++) {
                double b = tupNum(bl, i, 0) / 255.0, w = tupNum(wh, i, 255) / 255.0;
                v[i * 5 + i] = w - b;
                v[i * 5 + 4] = b;
            }
            m = CMat::fromList(v, 20);
        }
        auto sp = std::make_shared<ImSpec>(*ch);
        for (auto &L : sp->layers)
            L.cm = m * L.cm;
        return sp;
    }
    if (c == "renpy.display.im.Crop") {
        ImSpecP ch = child("image");
        Value x = in->get("x");
        double cx, cy, cw, chh;
        if (ListObj *l = x.list()) {
            cx = tupNum(x, 0);
            cy = tupNum(x, 1);
            cw = tupNum(x, 2);
            chh = tupNum(x, 3);
            (void)l;
        } else {
            cx = numOr(x, 0);
            cy = numOr(in->get("y"), 0);
            cw = numOr(in->get("w"), ch->w);
            chh = numOr(in->get("h"), ch->h);
        }
        return cropSpec(ch, (float)cx, (float)cy, (float)cw, (float)chh);
    }
    if (c == "renpy.display.im.Composite") {
        auto sp = std::make_shared<ImSpec>();
        ListObj *imgs = in->get("images").list();
        Value positions = in->get("positions");
        Value size = in->get("size");
        if (imgs)
            for (size_t i = 0; i < imgs->v.size(); i++) {
                ImSpecP ch = imSpec(imgs->v[i]);
                if (i == 0) {
                    sp->w = ch->w;
                    sp->h = ch->h;
                }
                Value pos;
                if (ListObj *pl = positions.list())
                    if (i < pl->v.size())
                        pos = pl->v[i];
                float ox = (float)tupNum(pos, 0), oy = (float)tupNum(pos, 1);
                for (ImLayer L : ch->layers) {
                    L.m = matMul(matTranslate(ox, oy), L.m);
                    sp->layers.push_back(L);
                }
            }
        if (size.list()) {
            sp->w = (float)tupNum(size, 0);
            sp->h = (float)tupNum(size, 1);
            return cropSpec(sp, 0, 0, sp->w, sp->h);
        }
        return sp;
    }
    if (c == "renpy.display.im.Scale" || c == "renpy.display.im.FactorScale") {
        ImSpecP ch = child("image");
        double nw, nh;
        if (c == "renpy.display.im.Scale") {
            nw = numOr(in->get("width"), ch->w);
            nh = numOr(in->get("height"), ch->h);
        } else {
            nw = std::floor(ch->w * numOr(in->get("width"), 1));
            nh = std::floor(ch->h * numOr(in->get("height"), numOr(in->get("width"), 1)));
        }
        auto sp = std::make_shared<ImSpec>();
        sp->w = (float)nw;
        sp->h = (float)nh;
        Mat s = matScale(ch->w > 0 ? (float)(nw / ch->w) : 1, ch->h > 0 ? (float)(nh / ch->h) : 1);
        for (ImLayer L : ch->layers) {
            L.m = matMul(s, L.m);
            sp->layers.push_back(L);
        }
        return sp;
    }
    if (c == "renpy.display.im.Flip") {
        ImSpecP ch = child("image");
        bool hz = in->get("horizontal").truthy(), vt = in->get("vertical").truthy();
        auto sp = std::make_shared<ImSpec>(*ch);
        Mat f = matMul(matTranslate(hz ? ch->w : 0, vt ? ch->h : 0), matScale(hz ? -1 : 1, vt ? -1 : 1));
        for (auto &L : sp->layers)
            L.m = matMul(f, L.m);
        return sp;
    }
    if (c == "renpy.display.im.Rotozoom") {
        ImSpecP ch = child("image");
        double angle = numOr(in->get("angle"), 0), zoom = numOr(in->get("zoom"), 1);
        double rad = angle * M_PI / 180.0;
        double cw = ch->w * zoom, chh = ch->h * zoom;
        double ow = std::fabs(cw * std::cos(rad)) + std::fabs(chh * std::sin(rad));
        double oh = std::fabs(cw * std::sin(rad)) + std::fabs(chh * std::cos(rad));
        auto sp = std::make_shared<ImSpec>();
        sp->w = (float)std::ceil(ow);
        sp->h = (float)std::ceil(oh);
        // pygame rotates counterclockwise on screen (y down => negative angle)
        float cs = (float)std::cos(-rad), sn = (float)std::sin(-rad);
        Mat rot = matLinear(cs, -sn, sn, cs);
        Mat full = matMul(matTranslate(sp->w / 2, sp->h / 2),
                          matMul(rot, matMul(matScale((float)zoom, (float)zoom), matTranslate(-ch->w / 2, -ch->h / 2))));
        for (ImLayer L : ch->layers) {
            L.m = matMul(full, L.m);
            sp->layers.push_back(L);
        }
        return sp;
    }
    if (c == "renpy.display.im.Tile") {
        ImSpecP ch = child("image");
        Value size = in->get("size");
        float tw = size.list() ? (float)tupNum(size, 0) : VW, th = size.list() ? (float)tupNum(size, 1) : VH;
        auto sp = std::make_shared<ImSpec>();
        sp->w = tw;
        sp->h = th;
        if (ch->w >= 1 && ch->h >= 1) {
            for (float y = 0; y < th; y += ch->h)
                for (float x = 0; x < tw; x += ch->w)
                    for (ImLayer L : ch->layers) {
                        L.m = matMul(matTranslate(x, y), L.m);
                        sp->layers.push_back(L);
                    }
            return cropSpec(sp, 0, 0, tw, th);
        }
        return sp;
    }
    if (c == "renpy.display.im.AlphaMask")
        return imSpec(in->get("base"));
    logf("imSpec: unsupported manipulator %s", c.c_str());
    return std::make_shared<ImSpec>();
}

static ImSpecP imSpec(const Value &v) {
    if (v.isStr())
        return fileSpec(v.s());
    InstObj *in = v.inst();
    if (!in)
        return std::make_shared<ImSpec>();
    if (in->native) {
        auto sp = std::static_pointer_cast<ImSpec>(in->native);
        return sp;
    }
    ImSpecP sp = buildSpec(in);
    in->native = sp;
    return sp;
}

static bool isImageManipulator(const std::string &cls) {
    return cls.compare(0, 17, "renpy.display.im.") == 0 && cls != "renpy.display.im.matrix";
}

struct ImageDisp : Displayable {
    ImSpecP spec;
    RenderP cached;
    RenderP render(float, float, double, double) override {
        if (cached)
            return cached;
        auto r = Render::make(spec->w, spec->h);
        for (auto &L : spec->layers) {
            auto t = Render::make(1, 1);
            if (L.solid) {
                t->op = Render::SOLID;
                t->r = L.sr;
                t->g = L.sg;
                t->b = L.sb;
                t->a = L.sa;
            } else {
                t->op = Render::TEX;
                t->tex = L.tex;
                t->u0 = L.u0;
                t->v0 = L.v0;
                t->u1 = L.u1;
                t->v1 = L.v1;
                t->cm = L.cm;
            }
            auto holder = Render::make(1, 1);
            holder->hasReverse = true;
            holder->rxdx = L.m.a;
            holder->rxdy = L.m.c;
            holder->rydx = L.m.b;
            holder->rydy = L.m.d;
            holder->blit(t, 0, 0);
            r->blit(holder, L.m.tx, L.m.ty);
        }
        cached = r;
        return r;
    }
    void predictFiles(std::vector<std::string> &out) override {
        for (auto &L : spec->layers)
            if (L.tex)
                out.push_back(L.tex->file);
    }
};

DispP makeImageFile(const std::string &file) {
    auto d = std::make_shared<ImageDisp>();
    d->kind = "Image";
    d->spec = fileSpec(file);
    d->style = namedStyle("image");
    return d;
}

// ------------------------------------------------------------------ simple displayables

struct SolidDisp : Displayable {
    float r = 0, g = 0, b = 0, a = 1;
    RenderP render(float w, float h, double, double) override {
        auto rv = Render::make(w, h);
        rv->op = Render::SOLID;
        rv->r = r;
        rv->g = g;
        rv->b = b;
        rv->a = a;
        return rv;
    }
};

static bool parseColor(const Value &v, float &r, float &g, float &b, float &a) {
    if (v.isStr()) {
        Color c = hexColor(v.s().c_str());
        r = c.r;
        g = c.g;
        b = c.b;
        a = c.a;
        return true;
    }
    if (ListObj *l = v.list()) {
        r = (float)tupNum(v, 0) / 255.0f;
        g = (float)tupNum(v, 1) / 255.0f;
        b = (float)tupNum(v, 2) / 255.0f;
        a = l->v.size() > 3 ? (float)tupNum(v, 3) / 255.0f : 1.0f;
        return true;
    }
    return false;
}

DispP makeSolid(float r, float g, float b, float a) {
    auto d = std::make_shared<SolidDisp>();
    d->kind = "Solid";
    d->r = r;
    d->g = g;
    d->b = b;
    d->a = a;
    return d;
}

struct NullDisp : Displayable {
    float w = 0, h = 0;
    RenderP render(float, float, double, double) override { return Render::make(w, h); }
};

DispP makeNull(float w, float h) {
    auto d = std::make_shared<NullDisp>();
    d->kind = "Null";
    d->w = w;
    d->h = h;
    return d;
}

struct FrameDisp : Displayable {
    ImSpecP spec;
    float left = 0, top = 0, right = 0, bottom = 0;
    bool tile = false;
    RenderP render(float w, float h, double, double) override {
        auto rv = Render::make(w, h);
        if (spec->layers.empty() || !spec->layers[0].tex)
            return rv;
        const ImLayer &L = spec->layers[0];
        float iw = spec->w, ih = spec->h;
        float xs[4] = {0, left, w - right, w};
        float ys[4] = {0, top, h - bottom, h};
        float sx[4] = {0, left, iw - right, iw};
        float sy[4] = {0, top, ih - bottom, ih};
        for (int j = 0; j < 3; j++)
            for (int i = 0; i < 3; i++) {
                float qw = xs[i + 1] - xs[i], qh = ys[j + 1] - ys[j];
                float srcW = sx[i + 1] - sx[i], srcH = sy[j + 1] - sy[j];
                if (qw <= 0 || qh <= 0 || srcW <= 0 || srcH <= 0)
                    continue;
                auto emit = [&](float x, float y, float dw, float dh, float fu, float fv) {
                    auto t = Render::make(dw, dh);
                    t->op = Render::TEX;
                    t->tex = L.tex;
                    float du = L.u1 - L.u0, dv = L.v1 - L.v0;
                    t->u0 = L.u0 + du * (sx[i] / iw);
                    t->v0 = L.v0 + dv * (sy[j] / ih);
                    t->u1 = L.u0 + du * ((sx[i] + srcW * fu) / iw);
                    t->v1 = L.v0 + dv * ((sy[j] + srcH * fv) / ih);
                    t->cm = L.cm;
                    rv->blit(t, x, y);
                };
                if (tile && (i == 1 || j == 1)) {
                    for (float y = 0; y < qh; y += srcH)
                        for (float x = 0; x < qw; x += srcW) {
                            float dw = std::min(srcW, qw - x), dh = std::min(srcH, qh - y);
                            emit(xs[i] + x, ys[j] + y, dw, dh, dw / srcW, dh / srcH);
                        }
                } else {
                    emit(xs[i], ys[j], qw, qh, 1, 1);
                }
            }
        return rv;
    }
    void predictFiles(std::vector<std::string> &out) override {
        for (auto &L : spec->layers)
            if (L.tex)
                out.push_back(L.tex->file);
    }
};

// ------------------------------------------------------------------ containers

struct FixedDisp : Displayable {
    std::vector<DispP> kids;
    RenderP render(float w, float h, double st, double at) override {
        if (style.xminimum.isNum())
            w = std::max(w, style.xminimum.t == T::Float ? (float)(style.xminimum.f * w) : (float)style.xminimum.num());
        if (style.yminimum.isNum())
            h = std::max(h, style.yminimum.t == T::Float ? (float)(style.yminimum.f * h) : (float)style.yminimum.num());
        auto rv = Render::make(w, h);
        for (auto &k : kids) {
            RenderP surf = renderDisp(k, w, h, st, at);
            k->place(*rv, 0, 0, w, h, surf);
        }
        return rv;
    }
    void children(std::vector<DispP> &out) override { out.insert(out.end(), kids.begin(), kids.end()); }
};

DispP makeFixed(const std::vector<DispP> &children) {
    auto d = std::make_shared<FixedDisp>();
    d->kind = "Fixed";
    d->kids = children;
    return d;
}

struct PositionDisp : Displayable {
    DispP child;
    RenderP render(float w, float h, double st, double at) override {
        RenderP surf = renderDisp(child, w, h, st, at);
        auto rv = Render::make(surf->w, surf->h);
        rv->blit(surf, 0, 0);
        return rv;
    }
    Placement placement() override {
        Placement p = child ? child->placement() : Placement();
        const Placement &s = style.place;
        if (s.xpos.set)
            p.xpos = s.xpos;
        if (s.ypos.set)
            p.ypos = s.ypos;
        if (s.xanchor.set)
            p.xanchor = s.xanchor;
        if (s.yanchor.set)
            p.yanchor = s.yanchor;
        if (s.xoffset)
            p.xoffset = s.xoffset;
        if (s.yoffset)
            p.yoffset = s.yoffset;
        p.subpixel = p.subpixel || s.subpixel;
        return p;
    }
    void children(std::vector<DispP> &out) override {
        if (child)
            out.push_back(child);
    }
};

DispP makeFixedSize(float w, float h, const std::vector<std::pair<std::pair<float, float>, DispP>> &children) {
    auto f = std::make_shared<FixedDisp>();
    f->kind = "LiveComposite";
    f->style.xmaximum = f->style.xminimum = Value::integer((int64_t)w);
    f->style.ymaximum = f->style.yminimum = Value::integer((int64_t)h);
    for (auto &c : children) {
        auto p = std::make_shared<PositionDisp>();
        p->kind = "Position";
        p->child = c.second;
        p->style.place.xpos = Pos::absolute(c.first.first);
        p->style.place.ypos = Pos::absolute(c.first.second);
        p->style.place.xanchor = Pos::absolute(0);
        p->style.place.yanchor = Pos::absolute(0);
        f->kids.push_back(p);
    }
    return f;
}

// ------------------------------------------------------------------ image references

static std::unordered_map<std::string, Value> &imageRegistry() {
    static std::unordered_map<std::string, Value> r;
    return r;
}

static std::unordered_map<std::string, std::vector<std::string>> &tagIndex() {
    static std::unordered_map<std::string, std::vector<std::string>> t;
    return t;
}

void registerImage(const std::string &name, const Value &v) {
    auto &reg = imageRegistry();
    if (!reg.count(name)) {
        std::string tag = name.substr(0, name.find(' '));
        tagIndex()[tag].push_back(name);
    }
    reg[name] = v;
}
bool hasImage(const std::string &name) { return imageRegistry().count(name) != 0; }
Value imageValue(const std::string &name) {
    auto it = imageRegistry().find(name);
    return it == imageRegistry().end() ? Value() : it->second;
}
const std::vector<std::string> &imageNamesWithTag(const std::string &tag) {
    static const std::vector<std::string> empty;
    auto it = tagIndex().find(tag);
    return it == tagIndex().end() ? empty : it->second;
}

struct ImageRefDisp : Displayable {
    std::string name;
    DispP target;
    bool resolved = false;
    void resolve() {
        if (resolved)
            return;
        resolved = true;
        Value v = imageValue(name);
        if (v.isNone()) {
            logf("image not found: %s", name.c_str());
            target = makeNull();
            return;
        }
        target = toDisp(v);
    }
    RenderP render(float w, float h, double st, double at) override {
        resolve();
        return renderDisp(target, w, h, st, at);
    }
    Placement placement() override {
        resolve();
        Placement p = target->placement();
        // ImageReference style (image_placement) fills unset values
        if (!p.xpos.set)
            p.xpos = style.place.xpos;
        if (!p.ypos.set)
            p.ypos = style.place.ypos;
        if (!p.xanchor.set)
            p.xanchor = style.place.xanchor;
        if (!p.yanchor.set)
            p.yanchor = style.place.yanchor;
        return p;
    }
    void children(std::vector<DispP> &out) override {
        resolve();
        out.push_back(target);
    }
};

// ------------------------------------------------------------------ animations

struct BlinkDisp : Displayable {
    DispP image;
    double on = .5, off = .5, rise = .5, set = .5, high = 1, low = 0, offset = 0;
    bool animTimebase = false;
    RenderP render(float w, float h, double st, double at) override {
        double cycle = on + set + off + rise;
        double t = animTimebase ? at : st;
        double time = cycle > 0 ? std::fmod(offset + t, cycle) : 0;
        double alpha = high;
        if (time >= 0 && time < on)
            alpha = high;
        time -= on;
        if (time >= 0 && time < set)
            alpha = low * (time / set) + high * (1 - time / set);
        time -= set;
        if (time >= 0 && time < off)
            alpha = low;
        time -= off;
        if (time >= 0 && time < rise)
            alpha = high * (time / rise) + low * (1 - time / rise);
        RenderP rend = renderDisp(image, w, h, st, at);
        auto rv = Render::make(rend->w, rend->h);
        rv->blit(rend, 0, 0);
        rv->alpha = (float)alpha;
        return rv;
    }
    void children(std::vector<DispP> &out) override { out.push_back(image); }
};

struct AnimationDisp : Displayable {
    // frames with delays; optional transitions between frames (TransitionAnimation)
    std::vector<DispP> images;
    std::vector<double> delays;
    std::vector<Value> transitions;
    bool animTimebase = true;
    bool loop = true;
    RenderP render(float w, float h, double st, double at) override {
        double total = 0;
        for (double d : delays)
            total += d;
        double orig = animTimebase ? at : st;
        double t = total > 0 ? (loop ? std::fmod(orig, total) : std::min(orig, total - 1e-6)) : 0;
        for (size_t i = 0; i < images.size(); i++) {
            double delay = i < delays.size() ? delays[i] : 1e9;
            if (t < delay || i + 1 == images.size()) {
                DispP img = images[i];
                if (i < transitions.size() && !transitions[i].isNone() && orig >= (delays.empty() ? 0 : delays[0])) {
                    DispP prev = images[(i + images.size() - 1) % images.size()];
                    auto tr = makeTransition(transitions[i], prev, img);
                    if (tr)
                        img = tr;
                }
                RenderP r = renderDisp(img, w, h, t, at);
                auto rv = Render::make(r->w, r->h);
                rv->blit(r, 0, 0);
                return rv;
            }
            t -= delay;
        }
        return Render::make(0, 0);
    }
    void children(std::vector<DispP> &out) override { out.insert(out.end(), images.begin(), images.end()); }
};

struct ParticlesDisp : Displayable {
    DispP image;
    int count = 10;
    double border = 50, start = 0;
    double xs0 = 20, xs1 = 50, ys0 = 100, ys1 = 200;
    bool fast = false, rotate = false;
    struct P {
        double xspeed, yspeed, startT, xstart, ystart;
    };
    std::vector<P> parts;
    std::vector<double> starts;
    double lastSt = -1;
    void initStarts() {
        starts.clear();
        for (int i = 0; i < count; i++)
            starts.push_back(urand(0, start));
        starts.push_back(start);
        std::sort(starts.begin(), starts.end());
    }
    P make(double st, bool isFast) {
        P p;
        p.xspeed = urand(xs0, xs1);
        p.yspeed = urand(ys0, ys1);
        if (p.yspeed == 0)
            p.yspeed = 1;
        p.startT = st;
        double sw = rotate ? VH : VW, sh = rotate ? VW : VH;
        p.ystart = p.yspeed > 0 ? -border : sh + border;
        double travel = (2.0 * border + sh) / std::fabs(p.yspeed);
        double xdist = p.xspeed * travel;
        p.xstart = urand(std::min(-xdist, 0.0), std::max(sw + xdist, sw));
        if (isFast) {
            p.ystart = urand(-border, sh + border);
            p.xstart = urand(0, sw);
        }
        return p;
    }
    RenderP render(float w, float h, double st, double at) override {
        if (starts.empty())
            initStarts();
        if (st < lastSt || lastSt < 0)
            parts.clear();
        lastSt = st;
        double sh = rotate ? VW : VH;
        // update existing
        std::vector<P> alive;
        for (auto &p : parts) {
            double to = st - p.startT;
            double ypos = p.ystart + to * p.yspeed;
            if (ypos > sh + border || ypos < -border)
                continue;
            alive.push_back(p);
        }
        bool wasEmpty = parts.empty();
        parts.swap(alive);
        if (wasEmpty && fast) {
            for (int i = 0; i < count; i++)
                parts.push_back(make(st, true));
        } else if ((int)parts.size() < count) {
            if (parts.empty() || st >= starts[std::min(parts.size(), starts.size() - 1)])
                parts.push_back(make(st, false));
        }
        auto rv = Render::make(w, h);
        RenderP img = renderDisp(image, w, h, st, at);
        for (auto &p : parts) {
            double to = st - p.startT;
            double xpos = p.xstart + to * p.xspeed, ypos = p.ystart + to * p.yspeed;
            if (rotate)
                std::swap(xpos, ypos);
            rv->blit(img, (float)(int)xpos, (float)(int)ypos);
        }
        return rv;
    }
    void children(std::vector<DispP> &out) override { out.push_back(image); }
};

// ------------------------------------------------------------------ dynamic displayables (native functions)

using DynamicFn = std::function<DispP(double st, double at)>;
static std::unordered_map<std::string, std::function<DynamicFn(const Value &)>> &dynamicFactories() {
    static std::unordered_map<std::string, std::function<DynamicFn(const Value &)>> m;
    return m;
}
void registerDynamic(const std::string &fnName, std::function<DynamicFn(const Value &)> factory) {
    dynamicFactories()[fnName] = std::move(factory);
}

struct DynamicDisp : Displayable {
    Value function, args;
    DynamicFn fn;
    DispP last;
    RenderP render(float w, float h, double st, double at) override {
        if (fn) {
            last = fn(st, at);
        } else {
            try {
                CallArgs a;
                a.pos = {Value::real(st), Value::real(at)};
                if (ListObj *l = args.list())
                    a.pos.insert(a.pos.end(), l->v.begin(), l->v.end());
                Value r = PY.call(function, a);
                if (ListObj *l = r.list())
                    r = l->v.empty() ? Value() : l->v[0];
                last = toDisp(r);
            } catch (PyError &e) {
                logf("DynamicDisplayable error: %s %s", e.type.c_str(), e.msg.c_str());
                last = makeNull();
            }
        }
        return renderDisp(last, w, h, st, at);
    }
    Placement placement() override { return last ? last->placement() : style.place; }
};

// ------------------------------------------------------------------ conversion

Value dispValue(const DispP &d, const std::string &cls) {
    Value v = mkInst(cls);
    v.inst()->native = d;
    v.inst()->attrs["__native_disp__"] = Value::boolean(true);
    return v;
}

DispP valueDisp(const Value &v) {
    InstObj *in = v.inst();
    if (in && in->native && in->has("__native_disp__"))
        return std::static_pointer_cast<Displayable>(in->native);
    return nullptr;
}

DispP makeTextDisp(const Value &inst); // text.cpp
DispP makeDrugsDisp(const Value &inst); // natives

static DispP displayableFromString(const std::string &s) {
    if (!s.empty() && s[0] == '#') {
        Color c = hexColor(s.c_str());
        return makeSolid(c.r, c.g, c.b, c.a);
    }
    if (s.find('.') != std::string::npos)
        return makeImageFile(s);
    auto r = std::make_shared<ImageRefDisp>();
    r->kind = "ImageReference";
    r->name = s;
    r->style = namedStyle("image_placement");
    // collapse repeated spaces
    std::string norm;
    bool sp = false;
    for (char c : s) {
        if (c == ' ') {
            if (!sp && !norm.empty())
                norm += c;
            sp = true;
        } else {
            norm += c;
            sp = false;
        }
    }
    while (!norm.empty() && norm.back() == ' ')
        norm.pop_back();
    r->name = norm;
    return r;
}

DispP makeTransformFromInst(InstObj *in, const Value &v); // transform.cpp

DispP toDisp(const Value &v) {
    if (v.isNone())
        return nullptr;
    if (v.isStr())
        return displayableFromString(v.s());
    if (ListObj *l = v.list()) {
        // image name tuple
        std::string name;
        for (auto &x : l->v) {
            if (!name.empty())
                name += ' ';
            name += valueStr(x);
        }
        return displayableFromString(name);
    }
    InstObj *in = v.inst();
    if (!in) {
        logf("toDisp: not a displayable (%s)", typeName(v));
        return makeNull();
    }
    if (DispP d = valueDisp(v))
        return d;
    const std::string &c = in->cls;
    if (isImageManipulator(c)) {
        auto d = std::make_shared<ImageDisp>();
        d->kind = c.substr(17);
        d->spec = imSpec(v);
        d->style = resolveStyle(in->get("style"), "image");
        return d;
    }
    if (c == "renpy.display.imagelike.Solid") {
        auto d = std::make_shared<SolidDisp>();
        d->kind = "Solid";
        parseColor(in->get("color"), d->r, d->g, d->b, d->a);
        d->style = resolveStyle(in->get("style"), "default");
        return d;
    }
    if (c == "renpy.display.imagelike.Frame") {
        auto d = std::make_shared<FrameDisp>();
        d->kind = "Frame";
        d->spec = imSpec(in->get("image"));
        d->left = (float)numOr(in->get("left"), 0);
        d->top = (float)numOr(in->get("top"), 0);
        d->right = (float)numOr(in->get("right"), d->left);
        d->bottom = (float)numOr(in->get("bottom"), d->top);
        d->tile = in->get("tile").truthy();
        d->style = resolveStyle(in->get("style"), "default");
        return d;
    }
    if (c == "renpy.display.layout.Null") {
        auto d = std::make_shared<NullDisp>();
        d->kind = "Null";
        d->w = (float)numOr(in->get("width"), 0);
        d->h = (float)numOr(in->get("height"), 0);
        d->style = resolveStyle(in->get("style"), "default");
        return d;
    }
    if (c == "renpy.display.layout.MultiBox" || c == "renpy.display.layout.Fixed") {
        auto d = std::make_shared<FixedDisp>();
        d->kind = "Fixed";
        if (ListObj *kids = in->get("children").list())
            for (auto &k : kids->v)
                if (DispP kd = toDisp(k))
                    d->kids.push_back(kd);
        d->style = resolveStyle(in->get("style"), "default");
        return d;
    }
    if (c == "renpy.display.layout.Position") {
        auto d = std::make_shared<PositionDisp>();
        d->kind = "Position";
        d->child = toDisp(in->get("child"));
        d->style = resolveStyle(in->get("style"), "image_placement");
        return d;
    }
    if (c == "renpy.display.image.ImageReference") {
        auto d = std::static_pointer_cast<ImageRefDisp>(displayableFromString(" "));
        std::string name;
        if (ListObj *l = in->get("name").list())
            for (auto &x : l->v) {
                if (!name.empty())
                    name += ' ';
                name += valueStr(x);
            }
        d->name = name;
        return d;
    }
    if (c == "renpy.display.motion.Transform" || c == "renpy.display.motion.ATLTransform")
        return makeTransformFromInst(in, v);
    if (c == "renpy.display.anim.Blink") {
        auto d = std::make_shared<BlinkDisp>();
        d->kind = "Blink";
        d->image = toDisp(in->get("image"));
        d->on = numOr(in->get("on"), .5);
        d->off = numOr(in->get("off"), .5);
        d->rise = numOr(in->get("rise"), .5);
        d->set = numOr(in->get("set"), .5);
        d->high = numOr(in->get("high"), 1);
        d->low = numOr(in->get("low"), 0);
        d->offset = numOr(in->get("offset"), 0);
        d->animTimebase = in->get("anim_timebase").truthy();
        d->style = resolveStyle(in->get("style"), "default");
        return d;
    }
    if (c == "renpy.display.anim.TransitionAnimation") {
        auto d = std::make_shared<AnimationDisp>();
        d->kind = "TransitionAnimation";
        if (ListObj *l = in->get("images").list())
            for (auto &x : l->v)
                d->images.push_back(toDisp(x));
        if (ListObj *l = in->get("delays").list())
            for (auto &x : l->v)
                d->delays.push_back(numOr(x, 0));
        if (ListObj *l = in->get("transitions").list())
            d->transitions = l->v;
        d->animTimebase = in->get("anim_timebase").truthy();
        d->style = resolveStyle(in->get("style"), "animation");
        return d;
    }
    if (c == "renpy.display.anim.SMAnimation" || c == "renpy.display.anim.Animation" ||
        c == "renpy.display.anim.Filmstrip") {
        auto d = std::make_shared<AnimationDisp>();
        d->kind = "Animation";
        if (ListObj *l = in->get("images").list())
            for (auto &x : l->v)
                d->images.push_back(toDisp(x));
        if (ListObj *l = in->get("delays").list())
            for (auto &x : l->v)
                d->delays.push_back(numOr(x, 0));
        d->animTimebase = !in->has("anim_timebase") || in->get("anim_timebase").truthy();
        d->style = resolveStyle(in->get("style"), "animation");
        return d;
    }
    if (c == "renpy.display.particle.Particles") {
        auto d = std::make_shared<ParticlesDisp>();
        d->kind = "Particles";
        Value f = in->get("factory");
        if (InstObj *fi = f.inst()) {
            d->image = toDisp(fi->get("image"));
            d->count = (int)numOr(fi->get("count"), 10);
            d->border = numOr(fi->get("border"), 50);
            d->start = numOr(fi->get("start"), 0);
            d->fast = fi->get("fast").truthy();
            d->rotate = fi->get("rotate").truthy();
            Value xs = fi->get("xspeed"), ys = fi->get("yspeed");
            if (xs.list()) {
                d->xs0 = tupNum(xs, 0);
                d->xs1 = tupNum(xs, 1);
            } else {
                d->xs0 = d->xs1 = numOr(xs, 20);
            }
            if (ys.list()) {
                d->ys0 = tupNum(ys, 0);
                d->ys1 = tupNum(ys, 1);
            } else {
                d->ys0 = d->ys1 = numOr(ys, 100);
            }
        }
        d->style = resolveStyle(in->get("style"), "default");
        return d;
    }
    if (c == "renpy.display.layout.DynamicDisplayable") {
        auto d = std::make_shared<DynamicDisp>();
        d->kind = "DynamicDisplayable";
        d->function = in->get("function");
        d->args = in->get("args");
        std::string fname;
        if (FuncObj *fo = d->function.func())
            fname = fo->name;
        std::string shortName = fname.substr(fname.rfind('.') + 1);
        auto it = dynamicFactories().find(shortName);
        if (it != dynamicFactories().end())
            d->fn = it->second(v);
        d->style = resolveStyle(in->get("style"), "default");
        return d;
    }
    if (c == "renpy.text.text.Text" || c == "renpy.text.extras.ParameterizedText")
        return makeTextDisp(v);
    if (c == "store.drugsDisp")
        return makeDrugsDisp(v);
    logf("toDisp: unsupported class %s", c.c_str());
    return makeNull();
}

// ------------------------------------------------------------------ layout containers

StyleProps namedStyleProps(const std::string &name) { return namedStyle(name); }

static double scaleV(const Value &v, double base) {
    if (!v.isNum())
        return 0;
    return v.t == T::Float ? v.f * base : v.num();
}

RenderP WindowDisp::render(float width, float height, double st, double at) {
    const StyleProps &s = style;
    double xmin = scaleV(s.xminimum, width), ymin = scaleV(s.yminimum, height);
    double cxm = s.leftMargin + s.rightMargin, cym = s.topMargin + s.bottomMargin;
    double cxp = s.leftPad + s.rightPad, cyp = s.topPad + s.bottomPad;
    RenderP surf = child ? renderDisp(child, (float)(width - cxm - cxp), (float)(height - cym - cyp), st, at)
                         : Render::make(0, 0);
    double w = width, h = height;
    if (!s.xfill)
        w = std::max(cxm + cxp + surf->w, xmin);
    if (!s.yfill)
        h = std::max(cym + cyp + surf->h, ymin);
    auto rv = Render::make((float)w, (float)h);
    if (background) {
        float bw = (float)(w - cxm), bh = (float)(h - cym);
        RenderP back = renderDisp(background, bw, bh, st, at);
        background->place(*rv, (float)s.leftMargin, (float)s.topMargin, bw, bh, back);
    }
    if (child)
        child->place(*rv, (float)(s.leftMargin + s.leftPad), (float)(s.topMargin + s.topPad), (float)(w - cxm - cxp),
                     (float)(h - cym - cyp), surf);
    return rv;
}

RenderP BoxDisp::render(float width, float height, double st, double at) {
    struct Placed {
        DispP d;
        float x, y, w, h;
        RenderP surf;
    };
    std::vector<Placed> placements;
    double x = 0, y = 0, lineW = 0, lineH = 0, maxx = 0, maxy = 0;
    double rem = vertical ? height : width;
    for (size_t i = 0; i < kids.size(); i++) {
        const DispP &d = kids[i];
        RenderP surf = vertical ? renderDisp(d, (float)(width - x), (float)rem, st, at)
                                : renderDisp(d, (float)rem, (float)(height - y), st, at);
        double sw = surf->w, sh = surf->h;
        Placed p{d, (float)x, (float)y, 0, 0, surf};
        if (vertical) {
            p.w = (float)std::max(lineW, sw);
            p.h = (float)sh;
            lineW = std::max(lineW, sw);
            y += sh + style.spacing;
            rem -= sh + style.spacing;
        } else {
            p.w = (float)sw;
            p.h = (float)std::max(lineH, sh);
            lineH = std::max(lineH, sh);
            x += sw + style.spacing;
            rem -= sw + style.spacing;
        }
        maxx = std::max(maxx, (double)(p.x + p.w));
        maxy = std::max(maxy, (double)(p.y + p.h));
        placements.push_back(p);
    }
    double w = style.xfill ? width : maxx, h = style.yfill ? height : maxy;
    auto rv = Render::make((float)w, (float)h);
    for (auto &p : placements) {
        float pw = p.w, ph = p.h;
        if (vertical && style.xfill)
            pw = (float)w;
        if (!vertical && style.yfill)
            ph = (float)h;
        p.d->place(*rv, p.x, p.y, pw, ph, p.surf);
    }
    return rv;
}

std::shared_ptr<WindowDisp> makeWindow(const std::string &styleName, const DispP &child, const Value &overrides) {
    auto w = std::make_shared<WindowDisp>();
    w->kind = "Window";
    w->style = namedStyle(styleName);
    Value bg = styleProperty(styleName, "background");
    if (DictObj *od = overrides.dict()) {
        for (auto &kv : od->items) {
            if (!kv.first.isStr() || kv.first.s() == "style")
                continue;
            w->style.apply(kv.first.s(), kv.second);
            if (kv.first.s() == "background")
                bg = kv.second;
        }
    }
    if (!bg.isNone())
        w->background = toDisp(bg);
    w->child = child;
    return w;
}

std::shared_ptr<BoxDisp> makeBox(const std::string &styleName, bool vertical, const std::vector<DispP> &kids,
                                 const Value &overrides) {
    auto b = std::make_shared<BoxDisp>();
    b->kind = vertical ? "VBox" : "HBox";
    b->vertical = vertical;
    b->style = namedStyle(styleName);
    Value sp = styleProperty(styleName, "spacing");
    if (sp.isNum())
        b->style.spacing = sp.num();
    if (DictObj *od = overrides.dict())
        for (auto &kv : od->items)
            if (kv.first.isStr())
                b->style.apply(kv.first.s(), kv.second);
    b->kids = kids;
    return b;
}
