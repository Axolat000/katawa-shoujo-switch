#pragma once
#include "disp.h"

struct TextStyle {
    std::string font = "font/playtime.ttf";
    double size = 22;
    float r = 1, g = 1, b = 1, a = 1;
    bool bold = false, italic = false, underline = false, strike = false;
    bool hasShadow = false;
    double shadowX = 0, shadowY = 0;
    float sr = 0, sg = 0, sb = 0, sa = 1;
    struct Outline {
        double size, xo, yo;
        float r, g, b, a;
    };
    std::vector<Outline> outlines;
    double lineSpacing = 0, lineLeading = 0;
    double firstIndent = 0, restIndent = 0;
    double textAlign = 0;
    std::string layout = "greedy";
    std::string language = "western";
    double minWidth = 0;
    Value xmaximum;
    StyleProps box; // placement/size of the text widget

    // applies a style property value (Ren'Py names)
    void apply(const std::string &k, const Value &v);
};

// Builds a text style from a named style (current language) plus overrides (dict or kwargs).
TextStyle textStyleFor(const std::string &styleName);
void applyTextOverrides(TextStyle &st, const Value &dictOrNone);

struct TextLayoutP;
struct TextLayout;
using TextLayoutRef = std::shared_ptr<TextLayout>;

// Laid out text: total size and number of revealable characters.
struct TextLayout {
    float w = 0, h = 0;
    int chars = 0;
    std::vector<int> pauseChars; // {w}/{p} positions (character index)
    bool noWait = false;         // {nw}
    int fastChar = 0;            // {fast} position
    struct Run {
        std::string text;
        TextStyle st;
        float x, y, w, h, ascent;
        int firstChar, nchars;
        unsigned tex = 0, otex = 0;
        int tw = 0, th = 0, otw = 0, oth = 0;
        std::vector<unsigned> outlineTex;
        std::vector<std::pair<int, int>> outlineSize;
        std::vector<float> adv; // cumulative advances (virtual px)
        DispP image;            // inline {image}
        std::string link;       // {a=}
    };
    std::vector<Run> runs;
    float scale = 1;
    uint64_t lastUse = 0;
    void draw(const Mat &m, float alpha, int reveal) ;
    ~TextLayout();
};

namespace text {
bool init();
void beginFrame();
// Lays out text (with Ren'Py text tags) for a maximum width.
TextLayoutRef layout(const std::string &s, const TextStyle &st, float maxWidth);
// Strips text tags (for history / logs)
std::string stripTags(const std::string &s);
float lineHeight(const TextStyle &st);
} // namespace text

struct TextDisp : Displayable {
    std::string text;
    TextStyle st;
    int reveal = -1;          // characters shown (-1 all)
    TextLayoutRef lastLayout; // for size queries
    RenderP render(float w, float h, double st, double at) override;
};
std::shared_ptr<TextDisp> makeText(const std::string &s, const TextStyle &st);
