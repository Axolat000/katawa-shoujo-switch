#include "text.h"
#include <SDL.h>
#include <SDL_ttf.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

Value styleProperty(const std::string &style, const std::string &prop); // disp.cpp

// ------------------------------------------------------------------ styles

static bool colorOf(const Value &v, float &r, float &g, float &b, float &a) {
    if (v.isStr()) {
        Color c = hexColor(v.s().c_str());
        r = c.r;
        g = c.g;
        b = c.b;
        a = c.a;
        return true;
    }
    if (ListObj *l = v.list()) {
        auto at = [&](size_t i, double d) { return i < l->v.size() && l->v[i].isNum() ? l->v[i].num() : d; };
        r = (float)(at(0, 255) / 255.0);
        g = (float)(at(1, 255) / 255.0);
        b = (float)(at(2, 255) / 255.0);
        a = (float)(at(3, 255) / 255.0);
        return true;
    }
    return false;
}

void TextStyle::apply(const std::string &k, const Value &v) {
    if (k == "font") {
        if (v.isStr())
            font = v.s();
    } else if (k == "size") {
        if (v.isNum())
            size = v.num();
    } else if (k == "color") {
        colorOf(v, r, g, b, a);
    } else if (k == "bold")
        bold = v.truthy();
    else if (k == "italic")
        italic = v.truthy();
    else if (k == "underline")
        underline = v.truthy();
    else if (k == "strikethrough")
        strike = v.truthy();
    else if (k == "drop_shadow") {
        if (ListObj *l = v.list()) {
            hasShadow = true;
            shadowX = l->v.size() > 0 ? l->v[0].num() : 0;
            shadowY = l->v.size() > 1 ? l->v[1].num() : 0;
        } else {
            hasShadow = false;
        }
    } else if (k == "drop_shadow_color") {
        colorOf(v, sr, sg, sb, sa);
    } else if (k == "outlines") {
        outlines.clear();
        if (ListObj *l = v.list())
            for (auto &o : l->v) {
                ListObj *ol = o.list();
                if (!ol || ol->v.size() < 2)
                    continue;
                Outline out;
                out.size = ol->v[0].num();
                out.r = out.g = out.b = 0;
                out.a = 1;
                colorOf(ol->v[1], out.r, out.g, out.b, out.a);
                out.xo = ol->v.size() > 2 ? ol->v[2].num() : 0;
                out.yo = ol->v.size() > 3 ? ol->v[3].num() : 0;
                outlines.push_back(out);
            }
    } else if (k == "line_spacing")
        lineSpacing = v.isNum() ? v.num() : 0;
    else if (k == "line_leading")
        lineLeading = v.isNum() ? v.num() : 0;
    else if (k == "first_indent")
        firstIndent = v.isNum() ? v.num() : 0;
    else if (k == "rest_indent")
        restIndent = v.isNum() ? v.num() : 0;
    else if (k == "text_align")
        textAlign = v.isNum() ? v.num() : 0;
    else if (k == "layout") {
        if (v.isStr())
            layout = v.s();
    } else if (k == "language") {
        if (v.isStr())
            language = v.s();
    } else if (k == "min_width")
        minWidth = v.isNum() ? v.num() : 0;
    else if (k == "xmaximum") {
        xmaximum = v;
        box.apply(k, v);
    } else {
        box.apply(k, v);
    }
}

static const char *kTextProps[] = {"font", "size", "color", "bold", "italic", "underline", "strikethrough", "drop_shadow",
                                   "drop_shadow_color", "outlines", "line_spacing", "line_leading", "first_indent",
                                   "rest_indent", "text_align", "layout", "language", "min_width", "xpos", "ypos",
                                   "xanchor", "yanchor", "xoffset", "yoffset", "xmaximum", "ymaximum", "xminimum",
                                   "yminimum", "xfill", "yfill", "subpixel"};

TextStyle textStyleFor(const std::string &name) {
    TextStyle st;
    for (const char *p : kTextProps) {
        Value v = styleProperty(name, p);
        if (!v.isNone() || !strcmp(p, "drop_shadow"))
            st.apply(p, v);
    }
    return st;
}

void applyTextOverrides(TextStyle &st, const Value &d) {
    if (DictObj *dd = d.dict())
        for (auto &kv : dd->items)
            if (kv.first.isStr())
                st.apply(kv.first.s(), kv.second);
}

// ------------------------------------------------------------------ fonts

namespace text {

static std::unordered_map<std::string, std::vector<uint8_t>> g_fontData;
static std::map<std::string, TTF_Font *> g_fonts;
static std::unordered_map<std::string, std::weak_ptr<TextLayout>> g_layoutCache;
static std::vector<std::pair<uint64_t, TextLayoutRef>> g_keepAlive;
static uint64_t g_frame = 1;

bool init() {
    if (TTF_Init() != 0) {
        logf("TTF_Init: %s", TTF_GetError());
        return false;
    }
    return true;
}

static std::vector<uint8_t> *fontData(const std::string &file) {
    auto it = g_fontData.find(file);
    if (it != g_fontData.end())
        return it->second.empty() ? nullptr : &it->second;
    std::vector<uint8_t> data;
    if (!readFile(romfsPath("game/" + file), data))
        logf("font missing: %s", file.c_str());
    g_fontData[file] = std::move(data);
    auto &v = g_fontData[file];
    return v.empty() ? nullptr : &v;
}

static TTF_Font *font(std::string file, int px, bool bold, bool italic, int outline) {
    int styleBits = 0;
    if (bold) {
        if (file == "font/playtime.ttf")
            file = "font/playtime_bold.ttf";
        else
            styleBits |= TTF_STYLE_BOLD;
    }
    if (italic)
        styleBits |= TTF_STYLE_ITALIC;
    char key[512];
    snprintf(key, sizeof key, "%s|%d|%d|%d", file.c_str(), px, styleBits, outline);
    auto it = g_fonts.find(key);
    if (it != g_fonts.end())
        return it->second;
    std::vector<uint8_t> *data = fontData(file);
    if (!data && file != "font/playtime.ttf")
        data = fontData("font/playtime.ttf");
    TTF_Font *f = nullptr;
    if (data) {
        SDL_RWops *rw = SDL_RWFromConstMem(data->data(), (int)data->size());
        f = TTF_OpenFontRW(rw, 1, std::max(1, px));
        if (f) {
            TTF_SetFontStyle(f, styleBits);
            if (outline)
                TTF_SetFontOutline(f, outline);
            TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
        }
    }
    g_fonts[key] = f;
    return f;
}

static float scaleNow() { return std::max(0.5f, gfx::pixelScale()); }

float lineHeight(const TextStyle &st) {
    float sc = scaleNow();
    TTF_Font *f = font(st.font, (int)std::lround(st.size * sc), st.bold, st.italic, 0);
    return f ? TTF_FontHeight(f) / sc : (float)st.size * 1.3f;
}

std::string stripTags(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '{') {
            if (i + 1 < s.size() && s[i + 1] == '{') {
                out += '{';
                i++;
                continue;
            }
            size_t e = s.find('}', i);
            if (e == std::string::npos)
                break;
            i = e;
            continue;
        }
        out += s[i];
    }
    return out;
}

// ------------------------------------------------------------------ layout

struct Piece {
    std::string text; // one "word" or a single space / CJK char
    TextStyle st;
    std::string link;
    bool space = false;
    bool newline = false;
    double fixedWidth = -1; // {space=N}
    DispP image;
    int chars = 0;
    int firstChar = 0;
};

static std::string styleKey(const TextStyle &st) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s|%.2f|%d%d%d%d|%.3f,%.3f,%.3f,%.3f|%d|%zu", st.font.c_str(), st.size, st.bold, st.italic,
             st.underline, st.strike, st.r, st.g, st.b, st.a, st.hasShadow, st.outlines.size());
    std::string k = buf;
    if (st.hasShadow) {
        snprintf(buf, sizeof buf, "|s%.1f,%.1f,%.2f,%.2f,%.2f,%.2f", st.shadowX, st.shadowY, st.sr, st.sg, st.sb, st.sa);
        k += buf;
    }
    for (auto &o : st.outlines) {
        snprintf(buf, sizeof buf, "|o%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,%.2f", o.size, o.xo, o.yo, o.r, o.g, o.b, o.a);
        k += buf;
    }
    snprintf(buf, sizeof buf, "|%.1f|%.1f|%.1f|%.1f|%.2f|%s|%s|%.1f", st.lineSpacing, st.lineLeading, st.firstIndent,
             st.restIndent, st.textAlign, st.layout.c_str(), st.language.c_str(), st.minWidth);
    return k + buf;
}

static bool isBreakableCJK(uint32_t cp) { return isCJK(cp); }

static double parseSizeTag(const std::string &v, double cur) {
    if (v.empty())
        return cur;
    if (v[0] == '+')
        return cur + atof(v.c_str() + 1);
    if (v[0] == '-')
        return cur - atof(v.c_str() + 1);
    if (v[0] == '*')
        return cur * atof(v.c_str() + 1);
    return atof(v.c_str());
}

TextLayoutRef layout(const std::string &s, const TextStyle &base, float maxWidth) {
    float sc = scaleNow();
    std::string key = styleKey(base) + "|" + std::to_string((int)maxWidth) + "|" + std::to_string(sc) + "|" + s;
    auto cit = g_layoutCache.find(key);
    if (cit != g_layoutCache.end())
        if (auto sp = cit->second.lock()) {
            sp->lastUse = g_frame;
            g_keepAlive.emplace_back(g_frame, sp);
            return sp;
        }
    auto L = std::make_shared<TextLayout>();
    L->scale = sc;

    // ---- parse tags into pieces
    std::vector<Piece> pieces;
    std::vector<TextStyle> stack{base};
    std::vector<std::string> linkStack;
    int charIndex = 0;
    std::string word;
    auto flushWord = [&]() {
        if (word.empty())
            return;
        Piece p;
        p.text = word;
        p.st = stack.back();
        p.link = linkStack.empty() ? "" : linkStack.back();
        p.chars = (int)utf8Count(word);
        p.firstChar = charIndex;
        charIndex += p.chars;
        pieces.push_back(p);
        word.clear();
    };
    for (size_t i = 0; i < s.size();) {
        char c = s[i];
        if (c == '{') {
            if (i + 1 < s.size() && s[i + 1] == '{') {
                word += '{';
                i += 2;
                continue;
            }
            size_t e = s.find('}', i);
            if (e == std::string::npos) {
                word += s.substr(i);
                break;
            }
            std::string tag = s.substr(i + 1, e - i - 1);
            i = e + 1;
            std::string name = tag, val;
            size_t eq = tag.find('=');
            if (eq != std::string::npos) {
                name = tag.substr(0, eq);
                val = tag.substr(eq + 1);
            }
            flushWord();
            if (!name.empty() && name[0] == '/') {
                if (name == "/a") {
                    if (!linkStack.empty())
                        linkStack.pop_back();
                } else if (stack.size() > 1) {
                    stack.pop_back();
                }
                continue;
            }
            TextStyle ns = stack.back();
            if (name == "b") {
                ns.bold = true;
                stack.push_back(ns);
            } else if (name == "i") {
                ns.italic = true;
                stack.push_back(ns);
            } else if (name == "u") {
                ns.underline = true;
                stack.push_back(ns);
            } else if (name == "s") {
                ns.strike = true;
                stack.push_back(ns);
            } else if (name == "plain") {
                ns.bold = ns.italic = ns.underline = ns.strike = false;
                stack.push_back(ns);
            } else if (name == "color") {
                std::string cv = val;
                if (!cv.empty() && cv[0] != '#')
                    cv = "#" + cv;
                Color col = hexColor(cv.c_str());
                ns.r = col.r;
                ns.g = col.g;
                ns.b = col.b;
                ns.a = col.a;
                stack.push_back(ns);
            } else if (name == "alpha") {
                ns.a *= (float)atof(val.c_str());
                stack.push_back(ns);
            } else if (name == "size") {
                ns.size = parseSizeTag(val, ns.size);
                stack.push_back(ns);
            } else if (name == "font") {
                ns.font = val;
                stack.push_back(ns);
            } else if (name == "a") {
                linkStack.push_back(val);
            } else if (name == "w" || name == "p") {
                L->pauseChars.push_back(charIndex);
                if (name == "p") {
                    Piece nl;
                    nl.newline = true;
                    nl.st = stack.back();
                    nl.firstChar = charIndex;
                    pieces.push_back(nl);
                }
            } else if (name == "nw") {
                L->noWait = true;
            } else if (name == "fast") {
                L->fastChar = charIndex;
                L->pauseChars.clear();
                L->noWait = false;
            } else if (name == "space") {
                Piece sp;
                sp.fixedWidth = atof(val.c_str());
                sp.st = stack.back();
                sp.firstChar = charIndex;
                pieces.push_back(sp);
            } else if (name == "image") {
                Piece ip;
                ip.image = toDisp(Value::str(val));
                ip.st = stack.back();
                ip.chars = 1;
                ip.firstChar = charIndex++;
                pieces.push_back(ip);
            }
            continue;
        }
        if (c == '}' && i + 1 < s.size() && s[i + 1] == '}') {
            word += '}';
            i += 2;
            continue;
        }
        if (c == '\n') {
            flushWord();
            Piece nl;
            nl.newline = true;
            nl.st = stack.back();
            nl.firstChar = charIndex;
            pieces.push_back(nl);
            i++;
            continue;
        }
        size_t st = i;
        uint32_t cp = utf8Decode(s, i);
        if (cp == ' ' || cp == 0x3000) {
            flushWord();
            Piece sp;
            sp.text = s.substr(st, i - st);
            sp.space = true;
            sp.st = stack.back();
            sp.chars = 1;
            sp.firstChar = charIndex++;
            pieces.push_back(sp);
            continue;
        }
        if (isBreakableCJK(cp)) {
            flushWord();
            word = s.substr(st, i - st);
            flushWord();
            continue;
        }
        word += s.substr(st, i - st);
    }
    flushWord();
    L->chars = charIndex;

    // ---- measure
    auto measure = [&](const Piece &p) -> double {
        if (p.fixedWidth >= 0)
            return p.fixedWidth;
        if (p.image) {
            RenderP r = p.image->render(1000, 1000, 0, 0);
            return r->w;
        }
        if (p.text.empty())
            return 0;
        TTF_Font *f = font(p.st.font, (int)std::lround(p.st.size * sc), p.st.bold, p.st.italic, 0);
        int w = 0, h = 0;
        if (f)
            TTF_SizeUTF8(f, p.text.c_str(), &w, &h);
        return w / sc;
    };
    std::vector<double> widths(pieces.size());
    for (size_t i = 0; i < pieces.size(); i++)
        widths[i] = measure(pieces[i]);

    // ---- line breaking (greedy)
    struct Line {
        std::vector<size_t> items;
        double width = 0;
    };
    std::vector<Line> lines(1);
    double wrap = maxWidth > 0 ? maxWidth : 1e9;
    if (base.layout == "subtitle")
        wrap = std::min(wrap, (double)VW * 0.9);
    if (base.layout == "nobreak")
        wrap = 1e9;
    auto indentFor = [&](size_t lineIdx) { return lineIdx == 0 ? base.firstIndent : base.restIndent; };
    for (size_t i = 0; i < pieces.size(); i++) {
        Piece &p = pieces[i];
        if (p.newline) {
            lines.emplace_back();
            continue;
        }
        Line &cur = lines.back();
        double avail = wrap - indentFor(lines.size() - 1);
        if (!p.space && !cur.items.empty() && cur.width + widths[i] > avail) {
            // trim trailing spaces
            while (!cur.items.empty() && pieces[cur.items.back()].space) {
                cur.width -= widths[cur.items.back()];
                cur.items.pop_back();
            }
            lines.emplace_back();
        }
        Line &ln = lines.back();
        if (p.space && ln.items.empty() && lines.size() > 1)
            continue;
        ln.items.push_back(i);
        ln.width += widths[i];
    }
    for (auto &ln : lines)
        while (!ln.items.empty() && pieces[ln.items.back()].space) {
            ln.width -= widths[ln.items.back()];
            ln.items.pop_back();
        }

    // ---- positions
    double maxW = 0;
    for (size_t li = 0; li < lines.size(); li++)
        maxW = std::max(maxW, lines[li].width + indentFor(li));
    double layoutW = std::max(maxW, base.minWidth);
    double y = 0;
    for (size_t li = 0; li < lines.size(); li++) {
        Line &ln = lines[li];
        double asc = 0, lineH = 0;
        TTF_Font *bf = font(base.font, (int)std::lround(base.size * sc), base.bold, base.italic, 0);
        if (bf) {
            asc = TTF_FontAscent(bf) / sc;
            lineH = TTF_FontHeight(bf) / sc;
        } else {
            asc = base.size;
            lineH = base.size * 1.3;
        }
        for (size_t idx : ln.items) {
            const Piece &p = pieces[idx];
            TTF_Font *f = font(p.st.font, (int)std::lround(p.st.size * sc), p.st.bold, p.st.italic, 0);
            if (f) {
                asc = std::max(asc, (double)TTF_FontAscent(f) / sc);
                lineH = std::max(lineH, (double)TTF_FontHeight(f) / sc);
            }
        }
        y += base.lineLeading;
        double x = indentFor(li) + (layoutW - ln.width - indentFor(li)) * base.textAlign;
        // merge pieces into runs of identical style
        for (size_t k = 0; k < ln.items.size(); k++) {
            const Piece &p = pieces[ln.items[k]];
            if (p.fixedWidth >= 0) {
                x += widths[ln.items[k]];
                continue;
            }
            TextLayout::Run run;
            run.st = p.st;
            run.link = p.link;
            run.firstChar = p.firstChar;
            run.image = p.image;
            run.x = (float)x;
            run.text = p.text;
            run.nchars = p.chars;
            double w = widths[ln.items[k]];
            if (!p.image) {
                while (k + 1 < ln.items.size()) {
                    const Piece &q = pieces[ln.items[k + 1]];
                    if (q.image || q.fixedWidth >= 0 || styleKey(q.st) != styleKey(p.st) || q.link != p.link)
                        break;
                    run.text += q.text;
                    run.nchars += q.chars;
                    w += widths[ln.items[k + 1]];
                    k++;
                }
                // re-measure merged run (kerning across words)
                TTF_Font *f = font(run.st.font, (int)std::lround(run.st.size * sc), run.st.bold, run.st.italic, 0);
                if (f) {
                    int mw = 0, mh = 0;
                    TTF_SizeUTF8(f, run.text.c_str(), &mw, &mh);
                    w = mw / sc;
                    run.ascent = TTF_FontAscent(f) / sc;
                    run.h = TTF_FontHeight(f) / sc;
                }
                run.y = (float)(y + asc - run.ascent);
            } else {
                RenderP r = p.image->render(1000, 1000, 0, 0);
                run.h = r->h;
                run.y = (float)(y + asc - r->h);
            }
            run.w = (float)w;
            L->runs.push_back(run);
            x += w;
        }
        y += lineH;
        if (li + 1 < lines.size())
            y += base.lineSpacing;
    }
    L->w = (float)layoutW;
    L->h = (float)y;
    L->lastUse = g_frame;
    g_layoutCache[key] = L;
    g_keepAlive.emplace_back(g_frame, L);
    return L;
}

static unsigned surfaceTex(SDL_Surface *s, int &tw, int &th) {
    if (!s)
        return 0;
    SDL_Surface *c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    SDL_FreeSurface(s);
    if (!c)
        return 0;
    SDL_LockSurface(c);
    std::vector<uint8_t> tight((size_t)c->w * c->h * 4);
    for (int y = 0; y < c->h; y++)
        memcpy(&tight[(size_t)y * c->w * 4], (uint8_t *)c->pixels + y * c->pitch, (size_t)c->w * 4);
    SDL_UnlockSurface(c);
    unsigned t = gfx::uploadTexture(tight.data(), c->w, c->h, false, false);
    tw = c->w;
    th = c->h;
    SDL_FreeSurface(c);
    return t;
}

void beginFrame() {
    g_frame++;
    if (g_frame % 30 == 0) {
        // keep layouts used in the last ~3 seconds alive
        g_keepAlive.erase(std::remove_if(g_keepAlive.begin(), g_keepAlive.end(),
                                         [](const std::pair<uint64_t, TextLayoutRef> &p) {
                                             return p.first + 180 < g_frame;
                                         }),
                          g_keepAlive.end());
        for (auto it = g_layoutCache.begin(); it != g_layoutCache.end();) {
            if (it->second.expired())
                it = g_layoutCache.erase(it);
            else
                ++it;
        }
    }
}

} // namespace text

TextLayout::~TextLayout() {
    for (auto &r : runs) {
        if (r.tex)
            gfx::deleteTexture(r.tex);
        for (unsigned t : r.outlineTex)
            if (t)
                gfx::deleteTexture(t);
    }
}

void TextLayout::draw(const Mat &m, float alpha, int reveal) {
    lastUse = text::g_frame;
    for (auto &run : runs) {
        if (reveal >= 0 && run.firstChar >= reveal)
            break;
        if (run.image) {
            RenderP r = run.image->render(1000, 1000, 0, 0);
            drawRender(r, matMul(m, matTranslate(run.x, run.y)), alpha);
            continue;
        }
        if (run.text.empty())
            continue;
        int px = (int)std::lround(run.st.size * scale);
        if (!run.tex) {
            TTF_Font *f = text::font(run.st.font, px, run.st.bold, run.st.italic, 0);
            SDL_Color white{255, 255, 255, 255};
            if (f) {
                int style = TTF_GetFontStyle(f);
                int extra = (run.st.underline ? TTF_STYLE_UNDERLINE : 0) | (run.st.strike ? TTF_STYLE_STRIKETHROUGH : 0);
                if (extra)
                    TTF_SetFontStyle(f, style | extra);
                run.tex = text::surfaceTex(TTF_RenderUTF8_Blended(f, run.text.c_str(), white), run.tw, run.th);
                if (extra)
                    TTF_SetFontStyle(f, style);
                for (auto &o : run.st.outlines) {
                    int ow = std::max(1, (int)std::lround(o.size * scale));
                    TTF_Font *fo = text::font(run.st.font, px, run.st.bold, run.st.italic, ow);
                    int tw = 0, th = 0;
                    unsigned t = fo ? text::surfaceTex(TTF_RenderUTF8_Blended(fo, run.text.c_str(), white), tw, th) : 0;
                    run.outlineTex.push_back(t);
                    run.outlineSize.emplace_back(tw, th);
                }
                // cumulative advances for partial reveal
                run.adv.clear();
                for (size_t i = 0; i < run.text.size();) {
                    utf8Decode(run.text, i);
                    int w = 0, h = 0;
                    TTF_SizeUTF8(f, run.text.substr(0, i).c_str(), &w, &h);
                    run.adv.push_back(w / scale);
                }
            }
        }
        float inv = 1.0f / scale;
        float clipW = -1;
        if (reveal >= 0 && reveal < run.firstChar + run.nchars) {
            int shown = reveal - run.firstChar;
            clipW = shown > 0 && shown <= (int)run.adv.size() ? run.adv[shown - 1] : 0;
            if (clipW <= 0)
                continue;
        }
        auto blit = [&](unsigned tex, int tw, int th, float dx, float dy, float cr, float cg, float cb, float ca,
                        float extraW) {
            if (!tex)
                return;
            float w = tw * inv, h = th * inv;
            CMat cm;
            double v[20] = {0};
            v[0] = cr;
            v[6] = cg;
            v[12] = cb;
            v[18] = ca;
            cm = CMat::fromList(v, 20);
            if (clipW >= 0) {
                float cw = std::min(w, clipW + extraW);
                if (cw <= 0)
                    return;
                gfx::drawTexture(tex, matMul(m, matRect(dx, dy, cw, h)), 0, 0, cw / w, 1, alpha, &cm);
            } else {
                gfx::drawTexture(tex, matMul(m, matRect(dx, dy, w, h)), 0, 0, 1, 1, alpha, &cm);
            }
        };
        for (size_t i = run.outlineTex.size(); i-- > 0;) {
            auto &o = run.st.outlines[i];
            float ow = std::max(1.0f, std::round((float)o.size * scale)) * inv;
            blit(run.outlineTex[i], run.outlineSize[i].first, run.outlineSize[i].second, run.x - ow + (float)o.xo,
                 run.y - ow + (float)o.yo, o.r, o.g, o.b, o.a * run.st.a, ow * 2);
        }
        if (run.st.hasShadow)
            blit(run.tex, run.tw, run.th, run.x + (float)run.st.shadowX, run.y + (float)run.st.shadowY, run.st.sr,
                 run.st.sg, run.st.sb, run.st.sa * run.st.a, 0);
        blit(run.tex, run.tw, run.th, run.x, run.y, run.st.r, run.st.g, run.st.b, run.st.a, 0);
    }
}

RenderP TextDisp::render(float w, float, double, double) {
    float maxW = w;
    if (st.xmaximum.isNum())
        maxW = st.xmaximum.t == T::Float ? (float)(w * st.xmaximum.f) : std::min(w, (float)st.xmaximum.num());
    auto L = text::layout(text, st, maxW);
    lastLayout = L;
    auto rv = Render::make(L->w, L->h);
    rv->op = Render::CUSTOM;
    int rev = reveal;
    rv->draw = [L, rev](const Mat &m, float alpha) { L->draw(m, alpha, rev); };
    return rv;
}

std::shared_ptr<TextDisp> makeText(const std::string &s, const TextStyle &st) {
    auto t = std::make_shared<TextDisp>();
    t->kind = "Text";
    t->text = s;
    t->st = st;
    t->style = st.box;
    return t;
}

// renpy.text.text.Text / ParameterizedText instances
DispP makeTextDisp(const Value &v) {
    InstObj *in = v.inst();
    TextStyle st = textStyleFor("default");
    std::string s;
    Value textv = in->get("text");
    if (ListObj *l = textv.list()) {
        for (auto &x : l->v)
            if (x.isStr())
                s += x.s();
    } else if (textv.isStr()) {
        s = textv.s();
    }
    Value style = in->get("style");
    if (InstObj *si = style.inst()) {
        Value parent = si->get("parent");
        if (ListObj *pl = parent.list())
            if (!pl->v.empty())
                st = textStyleFor(pl->v[0].s());
        if (ListObj *props = si->get("properties").list())
            for (auto &p : props->v)
                applyTextOverrides(st, p);
    }
    if (DictObj *pd = in->get("properties").dict()) {
        Value sname;
        if (const Value *sv = pd->find(Value::str("style")))
            if (sv->isStr())
                st = textStyleFor(sv->s());
        applyTextOverrides(st, in->get("properties"));
    }
    return makeText(s, st);
}
