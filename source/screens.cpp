#include "audio.h"
#include "game.h"
#include "input.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <dirent.h>
#include <sys/stat.h>

void runLabelToEnd(const std::string &label);
Value tupleAtStr(const Value &t, size_t i);
extern bool g_hideTransient;
bool playMovie(const std::string &file, bool skippable);

// ------------------------------------------------------------------ drawing helpers

namespace kui {

static float fadeAlpha = 1.0f;

void image(const std::string &file, float x, float y, float alpha = 1.0f, const CMat *cm = nullptr, float w = -1,
           float h = -1) {
    TexImage *t = texture::get(file);
    if (!texture::ensure(t))
        return;
    float dw = w < 0 ? (float)t->w : w, dh = h < 0 ? (float)t->h : h;
    gfx::drawTexture(t->tex, matRect(x, y, dw, dh), 0, 0, 1, 1, alpha * fadeAlpha, cm);
}

TexImage *tex(const std::string &file) {
    TexImage *t = texture::get(file);
    texture::ensure(t);
    return t;
}

TextStyle style(const std::string &name, double size = -1) {
    TextStyle st = textStyleFor(name);
    applyTextOverrides(st, G.displayString("styleoverrides"));
    if (size > 0)
        st.size = size;
    return st;
}

TextStyle colored(TextStyle st, const char *hex) {
    Color c = hexColor(hex);
    st.r = c.r;
    st.g = c.g;
    st.b = c.b;
    st.a = c.a;
    return st;
}

// Draws text with its top-left at (x,y) (or anchored by xalign on x). Returns size.
std::pair<float, float> text(const std::string &s, const TextStyle &st, float x, float y, float xalign = 0,
                             float maxW = 0, float alpha = 1) {
    auto L = text::layout(s, st, maxW > 0 ? maxW : 2000);
    L->draw(matTranslate(x - L->w * xalign, y), alpha * fadeAlpha, -1);
    return {L->w, L->h};
}

std::pair<float, float> measure(const std::string &s, const TextStyle &st, float maxW = 0) {
    auto L = text::layout(s, st, maxW > 0 ? maxW : 2000);
    return {L->w, L->h};
}

// ---- focus & buttons
struct Btn {
    std::string id;
    float x, y, w, h;
};
static std::vector<Btn> g_prev, g_cur;
static std::string g_focus;
static bool g_pointerMode = false;

void beginFrame() {
    g_prev = g_cur;
    g_cur.clear();
    const Input &in = g_input;
    if (in.pointerMoved || in.pointerPressed)
        g_pointerMode = true;
    if (in.up || in.down || in.left || in.right) {
        g_pointerMode = false;
        if (g_prev.empty())
            return;
        auto it = std::find_if(g_prev.begin(), g_prev.end(), [](const Btn &b) { return b.id == g_focus; });
        if (it == g_prev.end()) {
            g_focus = g_prev.front().id;
            return;
        }
        float cx = it->x + it->w / 2, cy = it->y + it->h / 2;
        float best = 1e9;
        std::string bestId;
        for (auto &b : g_prev) {
            if (b.id == it->id)
                continue;
            float bx = b.x + b.w / 2, by = b.y + b.h / 2;
            float dx = bx - cx, dy = by - cy;
            bool ok = (in.up && dy < -1) || (in.down && dy > 1) || (in.left && dx < -1) || (in.right && dx > 1);
            if (!ok)
                continue;
            float primary = (in.up || in.down) ? std::fabs(dy) : std::fabs(dx);
            float secondary = (in.up || in.down) ? std::fabs(dx) : std::fabs(dy);
            float score = primary + secondary * 2.5f;
            if (score < best) {
                best = score;
                bestId = b.id;
            }
        }
        if (!bestId.empty())
            g_focus = bestId;
    }
}

bool focused(const std::string &id) { return g_focus == id; }
void focus(const std::string &id) { g_focus = id; }

// Registers a focusable rect. Returns true when activated.
bool button(const std::string &id, float x, float y, float w, float h, bool sensitive, bool *hovered = nullptr) {
    const Input &in = g_input;
    bool inside = in.px >= x && in.px < x + w && in.py >= y && in.py < y + h;
    if (sensitive) {
        g_cur.push_back({id, x, y, w, h});
        if (g_pointerMode && inside && (in.pointerMoved || in.pointerPressed))
            g_focus = id;
    }
    bool hov = sensitive && (g_focus == id) && (!g_pointerMode || inside);
    if (hovered)
        *hovered = hov;
    if (!sensitive)
        return false;
    if (in.pointerReleased && inside) {
        g_focus = id;
        return true;
    }
    if (in.accept && g_focus == id && !g_pointerMode)
        return true;
    if (in.accept && g_focus == id && g_pointerMode && inside)
        return true;
    return false;
}

void ensureFocus(const std::string &id) {
    bool exists = false;
    for (auto &b : g_prev)
        if (b.id == g_focus)
            exists = true;
    if (!exists && !g_pointerMode)
        g_focus = id;
}

// mm-style text button (black text, 40% idle)
bool textButton(const std::string &id, const std::string &label, float x, float y, bool sensitive = true,
                float xalign = 0, bool selected = false, double size = 22, float *outW = nullptr) {
    TextStyle st = style("mm_button_text", size);
    auto sz = measure(label, st);
    float bx = x - sz.first * xalign;
    bool hov = false;
    bool act = button(id, bx - 4, y - 2, sz.first + 8, sz.second + 4, sensitive, &hov);
    const char *col = !sensitive ? (selected ? "#000000" : "#00000019") : (hov || selected) ? "#000000" : "#00000066";
    text(label, colored(st, col), bx, y);
    if (outW)
        *outW = sz.first;
    return act;
}

static CMat opacity(float o) {
    double v[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, o, 0};
    return CMat::fromList(v, 20);
}

// widget_button: image + label (prefs_label style)
bool widgetButton(const std::string &id, const std::string &label, const std::string &img, float x, float y,
                  float xsize = 220, float ysize = 30, float widgetYOffset = 3, float textXOffset = 30,
                  const std::string &state = "button") {
    bool sensitive = state == "button";
    bool hov = false;
    bool act = button(id, x, y, xsize, ysize, sensitive, &hov);
    bool lit = hov || state == "active";
    CMat cm = opacity(state == "disabled" ? 0.1f : (lit ? 1.0f : 0.4f));
    image(img, x, y + widgetYOffset, 1.0f, &cm);
    TextStyle st = style("prefs_label");
    const char *col = state == "disabled" ? "#00000019" : lit ? "#000000" : "#00000066";
    text(label, colored(st, col), x + textXOffset, y);
    return act;
}

bool returnButton(const std::string &label, float x = 540, float y = 450) {
    return widgetButton("return", label, "ui/bt-return.png", x, y, 100);
}

void dim(float a = 0.5f) { gfx::drawSolid(matRect(0, 0, VW, VH), 0, 0, 0, a * fadeAlpha); }

} // namespace kui

// ------------------------------------------------------------------ notifications & overlays

static std::string g_notify;
static double g_notifyTime = -100;
static bool g_hideOverlay = false;

void nativeNotify(const std::string &msg) {
    g_notify = msg;
    g_notifyTime = nowSeconds();
}

void drawOverlays() {
    Game &g = G;
    if (g_hideOverlay)
        return;
    float x = 790;
    auto icon = [&](const char *file) {
        TexImage *t = kui::tex(file);
        if (!t->tex)
            return;
        x -= t->w;
        kui::image(file, x, 10);
        x -= 4;
    };
    if (!g.inMainMenuContext) {
        if (PY.getAttr(g.preferencesV, "afm_time").num() != 0)
            icon("ui/sd-auto.png");
        if ((!g.configV.inst()->get("skipping").isNone() || g.skipHeld) && g.configV.inst()->get("allow_skipping").truthy())
            icon("ui/sd-skip.png");
    }
    if (g.prefs.muted)
        icon("ui/sd-mute.png");
    if (g.storeGet("show_skipcredits_button").truthy()) {
        TextStyle st = kui::style("default", 18);
        auto sz = kui::measure(g.ds("skipcredits_button"), st);
        float bx = 795 - sz.first - 12, by = 595 - sz.second - 6;
        gfx::drawSolid(matRect(bx, by, sz.first + 12, sz.second + 6), 0.19f, 0.19f, 0.19f, 1);
        kui::text(g.ds("skipcredits_button"), kui::colored(st, "#000"), bx + 6, by + 3);
    }
    double age = nowSeconds() - g_notifyTime;
    if (age < 5 && !g_notify.empty()) {
        float a = age < 4 ? 1.0f : (float)(5 - age);
        TextStyle st = kui::style("default", 18);
        kui::text(g_notify, st, 10, 10, 0, 780, a);
    }
}

bool nativeScreenActive() { return false; }

// ------------------------------------------------------------------ screen loop

struct ScreenResult {
    bool done = false;
    Value value;
};

// Runs a native screen: body is called every frame (draw + logic); it sets r.done to leave.
static Value runScreen(const std::function<void(ScreenResult &)> &body, bool keepScene = true) {
    Game &g = G;
    ScreenResult r;
    Interaction in;
    in.type = IType::Screen;
    in.suppressWindow = true;
    in.draw = [&]() {
        kui::beginFrame();
        body(r);
    };
    in.update = [&]() { return r.done; };
    (void)keepScene;
    g.interact(in);
    return r.value;
}

// Yes/no prompt (_yesno_prompt). Returns true for yes.
bool confirmPrompt(const std::string &message, const std::string &sdImage, float sdx, float sdy, bool isYesNo = true,
                   const std::function<void()> &background = nullptr) {
    Value r = runScreen([&](ScreenResult &res) {
        if (background)
            background();
        kui::dim();
        TexImage *popup = kui::tex("ui/bg-popup.png");
        float pw = (float)popup->w, ph = (float)popup->h;
        float px = (VW - pw) / 2, py = (VH - ph) / 2;
        kui::image("ui/bg-popup.png", px, py);
        TextStyle st = kui::colored(kui::style("yesno_prompt"), "#00000066");
        st.textAlign = 0.5;
        auto sz = kui::measure(message, st);
        kui::text(message, st, VW / 2, py + ph * 0.3f - sz.second / 2, 0.5f);
        float by = py + ph * 0.62f;
        if (isYesNo) {
            kui::ensureFocus("no");
            if (kui::textButton("yes", G.ds("yesno_yes"), VW / 2 - 60, by, true, 0.5f)) {
                res.value = Value::boolean(true);
                res.done = true;
            }
            if (kui::textButton("no", G.ds("yesno_no"), VW / 2 + 60, by, true, 0.5f)) {
                res.value = Value::boolean(false);
                res.done = true;
            }
            if (g_input.cancel || g_input.menu) {
                res.value = Value::boolean(false);
                res.done = true;
            }
        } else {
            kui::ensureFocus("ok");
            if (kui::textButton("ok", G.ds("yesno_okay"), VW / 2, by, true, 0.5f) || g_input.cancel || g_input.menu) {
                res.value = Value::boolean(true);
                res.done = true;
            }
        }
        if (!sdImage.empty())
            kui::image(sdImage, sdx, sdy);
    });
    return r.truthy();
}

// ------------------------------------------------------------------ main menu background

static std::string g_lastPortraitState;

static void drawMainMenuBackground(bool animated, double t) {
    Game &g = G;
    kui::image("ui/main/bg-main.png", 0, 0);
    struct W {
        const char *trigger;
        float x, y;
        const char *file;
    };
    static const W widgets[] = {{"tc_act4_shizune", 677, 308, "16_tc4-shizune.png"}, {"tc_act3_shizune", 681, 383, "15_tc3-shizune.png"},
                                {"tc_act2_shizune", 620, 476, "14_tc2-shizune.png"}, {"tc_act4_rin", 526, 190, "13_tc4-rin.png"},
                                {"tc_act3_rin", 590, 232, "12_tc3-rin-rin.png"},     {"tc_act3_rin", 637, 295, "11_tc3-rin-hisao.png"},
                                {"tc_act2_rin", 629, 402, "10_tc2-rin.png"},         {"tc_act4_lilly", 464, 201, "09_tc4-lilly.png"},
                                {"tc_act3_lilly", 529, 279, "08_tc3-lilly.png"},     {"tc_act2_lilly", 582, 375, "07_tc2-lilly.png"},
                                {"tc_act4_hanako", 382, 279, "06_tc4-hanako.png"},   {"tc_act4_emi", 375, 370, "05_tc4-emi.png"},
                                {"tc_act3_emi", 429, 453, "04_tc3-emi.png"},         {"tc_act2_emi", 463, 498, "03_tc2-emi.png"},
                                {"tc_act3_hanako", 469, 312, "02_tc3-hanako.png"},   {"tc_act2_hanako", 493, 380, "01_tc2-hanako.png"},
                                {"tc_act1", 552, 473, "00_tc1-hisao.png"}};
    Value seen = g.persistentGet("seen_labels");
    std::vector<const W *> shown;
    for (auto &w : widgets) {
        bool s = false;
        if (ListObj *l = seen.list())
            for (auto &x : l->v)
                if (x.isStr() && x.s() == w.trigger)
                    s = true;
        if (s)
            shown.push_back(&w);
    }
    const double base = 0.07;
    double delay = base * shown.size();
    for (auto *w : shown) {
        std::string file = std::string("ui/main/") + w->file;
        if (animated) {
            // mm_widget_in: xalign 0 ypos 0.2 alpha 0 -> delay -> easein 0.3 ypos 0 alpha 1 (inside a 220x200 fixed)
            double k = std::max(0.0, std::min(1.0, (t - delay) / 0.3));
            k = std::cos((1.0 - k) * M_PI / 2.0);
            float yoff = (float)((1.0 - k) * 0.2 * 200);
            kui::image(file, w->x, w->y + yoff, (float)k);
        } else {
            kui::image(file, w->x, w->y);
        }
        delay -= base;
    }
    TextStyle vs = kui::colored(kui::style("default", 9), "#00000080");
    vs.font = "font/playtime.ttf";
    kui::text(valueStr(g.storeGet("game_version")) + "\nRen'Py 6.16.5.525", vs, 720, 568);
}

static bool g_animateMM = true;

static void mmStaticBackground() { drawMainMenuBackground(false, 100); }

// ------------------------------------------------------------------ saves listing

struct SaveInfo {
    std::string slot;
    std::string extra;
    int64_t mtime = 0;
    unsigned thumb = 0;
    int tw = 0, th = 0;
};

static std::vector<SaveInfo> g_saves;
static bool g_savesDirty = true;


static void refreshSaves() {
    if (!g_savesDirty)
        return;
    g_savesDirty = false;
    for (auto &s : g_saves)
        if (s.thumb)
            gfx::deleteTexture(s.thumb);
    g_saves.clear();
    std::string dir = userPath("saves");
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (struct dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() < 6 || n.substr(n.size() - 5) != ".save")
            continue;
        std::vector<uint8_t> data;
        if (!readFile(dir + "/" + n, data))
            continue;
        ValueFile vf;
        if (!vf.load(data))
            continue;
        SaveInfo si;
        si.slot = n.substr(0, n.size() - 5);
        if (DictObj *root = vf.root.dict()) {
            if (const Value *v = root->find(Value::str("extraInfo")))
                si.extra = valueStr(*v);
            if (const Value *v = root->find(Value::str("mtime")))
                si.mtime = v->asInt();
            if (const Value *v = root->find(Value::str("thumb"))) {
                // raw thumbnail: 2 bytes width, 2 bytes height, RGBA pixels
                const std::string &raw = v->s();
                if (raw.size() > 4) {
                    int tw = (uint8_t)raw[0] | ((uint8_t)raw[1] << 8), th = (uint8_t)raw[2] | ((uint8_t)raw[3] << 8);
                    if (tw > 0 && th > 0 && raw.size() == 4 + (size_t)tw * th * 4) {
                        si.thumb = gfx::uploadTexture((const uint8_t *)raw.data() + 4, tw, th, false, false);
                        si.tw = tw;
                        si.th = th;
                    }
                }
            }
        }
        g_saves.push_back(si);
    }
    closedir(d);
    std::sort(g_saves.begin(), g_saves.end(), [](const SaveInfo &a, const SaveInfo &b) {
        if (a.slot.size() != b.slot.size())
            return a.slot.size() < b.slot.size();
        return a.slot < b.slot;
    });
}

Value listSavedGames() {
    refreshSaves();
    std::vector<Value> r;
    for (auto &s : g_saves)
        r.push_back(mkTuple({Value::str(s.slot), Value::str(s.extra), Value(), Value::integer(s.mtime)}));
    return mkList(r);
}

static bool haveSaves() {
    refreshSaves();
    return !g_saves.empty();
}

// ------------------------------------------------------------------ common page chrome

static void pageBackground(bool mm) {
    if (mm)
        mmStaticBackground();
    kui::dim(0.5f);
    kui::image("ui/bg-config.png", 150, 100);
}

void pageBackgroundMM() { pageBackground(G.inMainMenuContext); }
bool kuiFocused(const std::string &id) { return kui::focused(id); }

static std::string sceneNameFromLabel(const std::string &label) {
    try {
        Value v = PY.call(G.storeGet("name_from_label"), {Value::str(label)});
        if (v.isStr())
            return v.s();
    } catch (PyError &) {
    }
    return "";
}

static std::string timeFromSeconds(double s) {
    int mins = (int)s / 60;
    char buf[32];
    snprintf(buf, sizeof buf, "%d:%02d", mins / 60, mins % 60);
    return buf;
}

// ------------------------------------------------------------------ file picker (save / load)

static float g_saveScroll = 0;

// Returns slot name to act on, "" for return; sets deleteSlot when deleting.
static std::string filePicker(bool save, bool mm, std::string &deleteSlot, bool &newSlot) {
    Game &g = G;
    newSlot = false;
    deleteSlot.clear();
    g_savesDirty = true;
    Value r = runScreen([&](ScreenResult &res) {
        refreshSaves();
        pageBackground(mm);
        TextStyle cap = kui::style("page_caption");
        cap.bold = true;
        kui::text(g.ds(save ? "save_page_caption" : "load_page_caption"), kui::colored(cap, "#00000066"), 180, 120);
        const float listX = 180, listY = 150, listW = 400, listH = 296, entryH = 59;
        float maxScroll = std::max(0.0f, g_saves.size() * entryH - listH);
        g_saveScroll = std::max(0.0f, std::min(g_saveScroll + g_input.scroll, maxScroll));
        gfx::pushClip(listX, listY, listX + listW + 40, listY + listH);
        TextStyle info = kui::style("file_picker_extra_info");
        for (size_t i = 0; i < g_saves.size(); i++) {
            const SaveInfo &s = g_saves[i];
            float y = listY + i * entryH - g_saveScroll;
            if (y + entryH < listY || y > listY + listH)
                continue;
            std::string id = "slot" + s.slot;
            bool hov = false;
            bool act = kui::button(id, listX, y, 380, entryH - 3, true, &hov);
            if (hov) {
                // keep focused entries visible
                if (y < listY)
                    g_saveScroll -= listY - y;
                if (y + entryH > listY + listH)
                    g_saveScroll += y + entryH - (listY + listH);
            }
            CMat cm = kui::opacity(hov ? 1.0f : 0.4f);
            kui::image("ui/bt-scribble.png", listX, y + 3, 1.0f, &cm);
            if (s.thumb)
                gfx::drawTexture(s.thumb, matRect(listX + 4, y + 6, 66, 50), 0, 0, 1, 1, 1.0f);
            std::string savename = s.extra.substr(0, s.extra.find('#'));
            double playtime = s.extra.find('#') != std::string::npos ? atof(s.extra.c_str() + s.extra.find('#') + 1) : 0;
            std::string when = formatTime(s.mtime, valueStr(g.displayString("timeformat")).c_str());
            std::string line = when + " // " + g.ds("play_time_label") + ": " + timeFromSeconds(playtime) + "\n" +
                               sceneNameFromLabel(savename);
            kui::text(line, kui::colored(info, hov ? "#000000" : "#00000066"), listX + 80, y + 8, 0, 300);
            if (act) {
                res.value = Value::str(s.slot);
                res.done = true;
            }
            if (hov && (g_input.hide) ) {
                deleteSlot = s.slot;
                res.done = true;
            }
            if (kui::button("del" + s.slot, listX + 385, y + 10, 24, 24, true, &hov)) {
                deleteSlot = s.slot;
                res.done = true;
            }
            CMat dm = kui::opacity(hov ? 1.0f : 0.4f);
            kui::image("ui/bt-del.png", listX + 385, y + 10, 1.0f, &dm);
        }
        gfx::popClip();
        if (save) {
            if (kui::widgetButton("newsave", g.ds("new_save_button"), "ui/bt-star.png", 180, 450, 340, 30, 0)) {
                newSlot = true;
                res.done = true;
            }
            kui::ensureFocus("newsave");
        } else if (!g_saves.empty()) {
            kui::ensureFocus("slot" + g_saves.front().slot);
        }
        if (kui::returnButton(g.ds("return_button_text")) || g_input.cancel || g_input.menu)
            res.done = true;
    });
    return r.isStr() ? r.s() : "";
}

void captureSaveThumbnail(std::vector<uint8_t> &jpeg);

static void saveScreen() {
    Game &g = G;
    while (true) {
        std::string del;
        bool newSlot = false;
        std::string slot = filePicker(true, false, del, newSlot);
        if (!del.empty()) {
            if (confirmPrompt(g.ds("yesno_delete_savegame"), "ui/sd-emi.png", 510, 275)) {
                removeFile(userPath("saves/" + del + ".save"));
                g_savesDirty = true;
            }
            continue;
        }
        if (newSlot) {
            int maxN = 0;
            for (auto &s : g_saves)
                maxN = std::max(maxN, atoi(s.slot.c_str()));
            slot = std::to_string(maxN + 1);
        } else if (slot.empty()) {
            return;
        } else if (!confirmPrompt(g.ds("yesno_save_overwrite"), "ui/sd-lilly.png", 195, 275)) {
            continue;
        }
        std::string extra = valueStr(g.storeGet("save_name")) + "#" + std::to_string(g.playTime);
        if (g.saveSlot(slot, extra)) {
            g.storeSet("statechangesincesave", Value::boolean(false));
            g_savesDirty = true;
            confirmPrompt(g.ds("yesno_savesuccess"), "ui/sd-rin.png", 510, 165, false);
        }
        return;
    }
}

// Returns true if a game was loaded (the caller must leave its loop).
static bool loadScreen(bool mm) {
    Game &g = G;
    while (true) {
        std::string del;
        bool newSlot = false;
        std::string slot = filePicker(false, mm, del, newSlot);
        if (!del.empty()) {
            if (confirmPrompt(g.ds("yesno_delete_savegame"), "ui/sd-emi.png", 510, 275, true, mm ? mmStaticBackground : std::function<void()>())) {
                removeFile(userPath("saves/" + del + ".save"));
                g_savesDirty = true;
            }
            continue;
        }
        if (slot.empty())
            return false;
        if (!mm && g.storeGet("playthroughflag").truthy() && g.storeGet("statechangesincesave").truthy()) {
            if (!confirmPrompt(g.ds("yesno_load_in_game"), "ui/sd-emi.png", 510, 275))
                continue;
        }
        LoadSignal s;
        s.slot = slot;
        throw s;
    }
}

// ------------------------------------------------------------------ preferences

static double faderInverse(double v) { return (2.0 / M_PI) * std::acos(1.0 - v); }
static double fader(double v) { return 1.0 - std::cos(v * M_PI / 2.0); }

static void languageScreen(bool mm);

static bool slider(const std::string &id, float x, float y, float w, double &value) {
    bool hov = false;
    kui::button(id, x, y, w, 20, true, &hov);
    const Input &in = g_input;
    bool changed = false;
    if (hov && !kui::g_pointerMode) {
        if (in.left) {
            value = std::max(0.0, value - 0.05);
            changed = true;
        }
        if (in.right) {
            value = std::min(1.0, value + 0.05);
            changed = true;
        }
    }
    if (in.pointerDown && in.px >= x - 6 && in.px <= x + w + 6 && in.py >= y - 4 && in.py <= y + 24) {
        value = std::max(0.0, std::min(1.0, (double)(in.px - x - 12) / (w - 24)));
        changed = true;
    }
    CMat cm = kui::opacity(hov ? 1.0f : 0.4f);
    TexImage *left = kui::tex("ui/bt-cf-bar-left.png");
    TexImage *right = kui::tex("ui/bt-cf-bar-right.png");
    TexImage *thumb = kui::tex("ui/bt-cf-thumb.png");
    float filled = (float)(12 + (w - 24) * value);
    if (left->tex)
        gfx::drawTexture(left->tex, matRect(x, y, filled, (float)left->h), 0, 0, filled / left->w, 1, 1.0f, &cm);
    if (right->tex)
        gfx::drawTexture(right->tex, matRect(x + filled, y, w - filled, (float)right->h), filled / right->w, 0, 1, 1, 1.0f, &cm);
    if (thumb->tex)
        gfx::drawTexture(thumb->tex, matRect(x + filled - thumb->w / 2.0f, y - 3, (float)thumb->w, (float)thumb->h), 0, 0, 1, 1, 1.0f, &cm);
    return changed;
}

static void prefsScreen(bool mm) {
    Game &g = G;
    runScreen([&](ScreenResult &res) {
        pageBackground(mm);
        TextStyle cap = kui::colored(kui::style("page_caption"), "#00000066");
        cap.bold = true;
        kui::text(g.ds("config_page_caption"), cap, 180, 120);
        float x = 200, y = 148;
        InstObj *pr = g.preferencesV.inst();
        auto check = [&](const std::string &id, const std::string &label, bool value) {
            bool act = kui::widgetButton(id, label, value ? "ui/bt-cf-checked.png" : "ui/bt-cf-unchecked.png", x, y, 350, 30, 0);
            y += 33;
            return act;
        };
        kui::ensureFocus("skipunseen");
        y += 8;
        if (check("skipunseen", g.ds("config_skip_unseen_label"), pr->get("skip_unseen").truthy()))
            pr->attrs["skip_unseen"] = Value::boolean(!pr->get("skip_unseen").truthy());
        if (check("skipchoices", g.ds("config_skip_after_choice_label"), pr->get("skip_after_choices").truthy())) {
            pr->attrs["skip_after_choices"] = Value::boolean(!pr->get("skip_after_choices").truthy());
            g.prefs.skipAfterChoices = pr->get("skip_after_choices").truthy();
        }
        y += 8;
        TextStyle lab = kui::colored(kui::style("prefs_label"), "#00000066");
        // text speed (0 = instant shown as max)
        double cps = pr->get("text_cps").num();
        double cpsSlider = (cps == 0 ? 150 : cps - 1) / 150.0;
        if (slider("cps", x, y + 5, 200, cpsSlider)) {
            int v = (int)std::lround(cpsSlider * 150) + 1;
            if (v >= 151)
                v = 0;
            pr->attrs["text_cps"] = Value::integer(v);
        }
        kui::text(g.ds("config_textspeed_label"), lab, x + 215, y);
        y += 33;
        double afm = std::max(1.0, g.persistentGet("afm_time").num() - 1) / 40.0;
        if (slider("afm", x, y + 5, 200, afm)) {
            int v = std::min(40, (int)std::lround(afm * 40) + 1);
            if (pr->get("afm_time").num() > 0)
                pr->attrs["afm_time"] = Value::integer(v);
            g.persistentSet("afm_time", Value::integer(v));
        }
        kui::text(g.ds("config_afmspeed_label"), lab, x + 215, y);
        y += 41;
        double mv = faderInverse(g.prefs.musicVolume);
        if (slider("music", x, y + 5, 200, mv)) {
            g.prefs.musicVolume = fader(mv);
            audio::setMixerVolume("music", g.prefs.muted ? 0 : g.prefs.musicVolume);
        }
        kui::text(g.ds("config_musicvol_label"), lab, x + 215, y);
        y += 33;
        double sv = faderInverse(g.prefs.sfxVolume);
        if (slider("sfx", x, y + 5, 200, sv)) {
            g.prefs.sfxVolume = fader(sv);
            audio::setMixerVolume("sfx", g.prefs.muted ? 0 : g.prefs.sfxVolume);
        }
        kui::text(g.ds("config_sfxvol_label"), lab, x + 215, y);
        if (kui::widgetButton("sfxtest", g.ds("config_sfxtest_label"), "ui/bt-musicplay.png", x + 360, y, 100)) {
            static const char *tests[] = {"sfx_slide", "sfx_slide2", "sfx_draw"};
            audio::play("sound", valueStr(g.storeGet(tests[SDL_GetTicks() % 3])), 0, 0, 0);
        }
        y += 41;
        if (kui::widgetButton("language", g.ds("config_language_sel"), "ui/bt-language.png", x, y, 300, 30, 3)) {
            languageScreen(mm);
        }
        if (kui::returnButton(g.ds("return_button_text")) || g_input.cancel || g_input.menu) {
            g.savePersistent();
            res.done = true;
        }
    });
}

static void languageScreen(bool mm) {
    Game &g = G;
    static float scroll = 0;
    runScreen([&](ScreenResult &res) {
        pageBackground(mm);
        TextStyle cap = kui::colored(kui::style("page_caption"), "#00000066");
        cap.bold = true;
        kui::text(g.ds("config_language_caption"), cap, 180, 120);
        float y0 = 158, rowH = 30, listH = 270;
        float maxScroll = std::max(0.0f, (float)g.availableLanguages.size() * rowH - listH);
        scroll = std::max(0.0f, std::min(scroll + g_input.scroll, maxScroll));
        gfx::pushClip(150, y0, 650, y0 + listH);
        std::string current = g.lang();
        for (size_t i = 0; i < g.availableLanguages.size(); i++) {
            const std::string &l = g.availableLanguages[i];
            float y = y0 + i * rowH - scroll;
            std::string label;
            if (DictObj *langs = g.languagesV.dict())
                if (const Value *ld = langs->find(Value::str(l)))
                    if (DictObj *ldd = ld->dict())
                        if (const Value *dsv = ldd->find(Value::str("displayStrings")))
                            if (InstObj *di = dsv->inst())
                                label = valueStr(di->get("activeLanguage"));
            bool active = l == current;
            if (kui::focused("lang" + l)) {
                if (y < y0)
                    scroll -= y0 - y;
                if (y + rowH > y0 + listH)
                    scroll += y + rowH - (y0 + listH);
            }
            if (kui::widgetButton("lang" + l, label, active ? "ui/bt-language.png" : "ui/bt-blank.png", 190, y, 500, 30, 3,
                                  30, active ? "active" : "button")) {
                g.switchLanguage(l);
                g.savePersistent();
            }
        }
        gfx::popClip();
        kui::ensureFocus("lang" + g.availableLanguages.front());
        if (!mm) {
            TextStyle note = kui::style("default", 18);
            note.textAlign = 0.5;
            kui::text(g.ds("config_language_restart_note"), note, VW / 2, 548, 0.5f, 780);
        }
        if (kui::returnButton(g.ds("return_button_text")) || g_input.cancel || g_input.menu)
            res.done = true;
    });
}

// ------------------------------------------------------------------ text history

static void historyScreen() {
    Game &g = G;
    std::vector<std::pair<std::string, std::string>> lines;
    if (ListObj *rb = g.storeGet("readback_buffer").list())
        for (auto &e : rb->v)
            lines.emplace_back(valueStr(tupleAtStr(e, 0)), valueStr(tupleAtStr(e, 1)));
    Value cl = g.storeGet("current_line");
    if (cl.list()) {
        auto cur = std::make_pair(valueStr(tupleAtStr(cl, 0)), valueStr(tupleAtStr(cl, 1)));
        if (lines.empty() || lines.back() != cur)
            lines.push_back(cur);
    }
    TextStyle labSt = kui::style("readback_label");
    TextStyle txtSt = kui::style("readback_text");
    float listX = 180, listY = 150, listW = 415, listH = 296;
    float total = 0;
    std::vector<float> heights;
    for (auto &l : lines) {
        float h = 0;
        if (!l.first.empty())
            h += kui::measure(l.first, labSt, listW).second;
        h += kui::measure(l.second, txtSt, listW).second + 10;
        heights.push_back(h);
        total += h;
    }
    float scroll = std::max(0.0f, total - listH);
    runScreen([&](ScreenResult &res) {
        pageBackground(false);
        TextStyle cap = kui::colored(kui::style("page_caption"), "#00000066");
        cap.bold = true;
        kui::text(g.ds("text_history_caption"), cap, 180, 120);
        const Input &in = g_input;
        float step = 50;
        if (in.up || in.rollback)
            scroll -= step;
        if (in.down || in.rollforward)
            scroll += step;
        scroll += in.scroll;
        scroll = std::max(0.0f, std::min(scroll, std::max(0.0f, total - listH)));
        gfx::pushClip(listX, listY, listX + listW, listY + listH);
        float y = listY - scroll;
        for (size_t i = 0; i < lines.size(); i++) {
            if (y + heights[i] >= listY && y <= listY + listH) {
                float yy = y;
                if (!lines[i].first.empty())
                    yy += kui::text(lines[i].first, labSt, listX, yy, 0, listW).second;
                kui::text(lines[i].second, txtSt, listX, yy, 0, listW);
            }
            y += heights[i];
        }
        gfx::popClip();
        kui::ensureFocus("return");
        if (kui::returnButton(g.ds("return_button_text")) || in.cancel || in.menu || in.history)
            res.done = true;
    });
}

Value tupleAtStr(const Value &t, size_t i) {
    ListObj *l = t.list();
    return (l && i < l->v.size()) ? l->v[i] : Value();
}

// ------------------------------------------------------------------ in-game menu

void Game::gameMenu(const std::string &screen) {
    Game &g = *this;
    g.storeSet("gm_active", Value::boolean(true));
    g.configV.inst()->attrs["skipping"] = Value();
    if (screen == "hide") {
        // gm_image: show the scene without the window until a key is pressed
        extern bool g_hideTransient;
        g_hideTransient = true;
        g_hideOverlay = true;
        runScreen([&](ScreenResult &res) {
            const Input &in = g_input;
            if (in.accept || in.cancel || in.menu || in.hide || in.pointerReleased)
                res.done = true;
        });
        g_hideTransient = false;
        g_hideOverlay = false;
        return;
    }
    if (screen == "history") {
        historyScreen();
        return;
    }
    kui::focus("gm_return");
    while (true) {
        std::string choice;
        runScreen([&](ScreenResult &res) {
            kui::dim(0.5f);
            TexImage *bg = kui::tex("ui/bg-gm.png");
            float fw = std::max(200.0f, (float)bg->w), fh = std::max(306.0f, (float)bg->h);
            float fx = (VW - fw) * 0.5f, fy = (VH - fh) * 0.4f;
            kui::image("ui/bg-gm.png", fx, fy);
            struct Item {
                const char *id, *key;
                bool enabled;
            };
            bool mm = g.inMainMenuContext;
            Item items[] = {{"return", "game_menu_return", true},
                            {"image", "game_menu_show", true},
                            {"history", "game_menu_history", true},
                            {"skip", "game_menu_skip", g.configV.inst()->get("allow_skipping").truthy() && !mm},
                            {"auto", "game_menu_auto", true},
                            {"prefs", "game_menu_config", true},
                            {"save", "game_menu_save", g.storeGet("playthroughflag").truthy() && !mm},
                            {"load", "game_menu_load", haveSaves()},
                            {"mainmenu", "game_menu_main", !mm},
                            {"quit", "game_menu_quit", true}};
            float y = fy + 62;
            TextStyle st = kui::style("gm_nav_button_text");
            float lh = kui::measure("Ag", st).second + 1;
            float totalH = lh * 10;
            y = fy + 62 + (fh - 62 - totalH) / 2 - 10;
            for (auto &it : items) {
                if (kui::textButton(std::string("gm_") + it.id, g.ds(it.key), VW / 2, y, it.enabled, 0.5f)) {
                    choice = it.id;
                    res.done = true;
                }
                y += lh;
            }
            // footer
            std::string footer;
            std::string sn = sceneNameFromLabel(valueStr(g.storeGet("save_name")));
            if (!sn.empty()) {
                if (!g.storeGet("playthroughflag").truthy())
                    sn += " (" + g.ds("game_menu_replay_indicator") + ")";
                footer += g.ds("play_time_label") + ": " + timeFromSeconds(g.playTime) + "\n" +
                          g.ds("game_menu_current_scene") + ": " + sn;
            }
            std::string playing = audio::playing("music");
            if (ListObj *tracks = g.storeGet("ex_m_tracks").list())
                for (auto &t : tracks->v)
                    if (valueStr(tupleAtStr(t, 1)) == playing && !playing.empty())
                        footer += "\n" + g.ds("game_menu_current_music") + ": " + valueStr(tupleAtStr(t, 0));
            TextStyle fs = kui::style("default", 18);
            fs.textAlign = 0.5;
            auto sz = kui::measure(footer, fs, 780);
            kui::text(footer, fs, VW / 2, 600 * 0.98f - sz.second * 0.98f, 0.5f, 780);
            if (g_input.cancel || g_input.menu) {
                choice = "return";
                res.done = true;
            }
        });
        if (choice == "return")
            break;
        if (choice == "image") {
            gameMenu("hide");
            break;
        }
        if (choice == "history") {
            historyScreen();
            continue;
        }
        if (choice == "skip") {
            g.configV.inst()->attrs["skipping"] = Value::str("slow");
            break;
        }
        if (choice == "auto") {
            try {
                PY.call(g.storeGet("turn_afm_on"));
            } catch (PyError &) {
            }
            break;
        }
        if (choice == "prefs") {
            prefsScreen(false);
            continue;
        }
        if (choice == "save") {
            saveScreen();
            continue;
        }
        if (choice == "load") {
            loadScreen(false);
            continue;
        }
        if (choice == "mainmenu") {
            bool ok = !g.storeGet("playthroughflag").truthy() ||
                      confirmPrompt(g.ds("yesno_return_to_main"), "ui/sd-shizune.png", 195, 275);
            if (ok) {
                audio::stop("music", 0);
                throw FullRestartSignal();
            }
            continue;
        }
        if (choice == "quit") {
            if (!g.storeGet("ask_to_quit").truthy() ||
                confirmPrompt(g.ds("yesno_quit"), "ui/sd-hanako.png", 515, 305)) {
                g.savePersistent();
                throw QuitSignal();
            }
            continue;
        }
    }
    g.storeSet("gm_active", Value::boolean(false));
    g.storeSet("statechangesincesave", Value::boolean(true));
}

bool g_hideTransient = false;

// ------------------------------------------------------------------ choice menu (ingamebutton)

Value runChoiceMenu(std::vector<MenuChoice> &choices, bool hasCaption) {
    Game &g = G;
    (void)hasCaption;
    Value result;
    kui::focus("choice0");
    double start = nowSeconds();
    Interaction in;
    in.type = IType::Menu;
    in.draw = [&]() {
        kui::beginFrame();
        double t = nowSeconds() - start;
        // ingm_bg / ingm_btn: invisible 0.5s then fade in over 0.5s
        float k = (float)std::max(0.0, std::min(1.0, (t - 0.5) / 0.5));
        // menu window (menu_window style, ypadding 40) centered vertically
        float rowH = 35;
        float totalH = choices.size() * rowH;
        float y0 = (VH - totalH) / 2 - 40;
        TextStyle st = kui::style("menu_choice", 20);
        for (size_t i = 0; i < choices.size(); i++) {
            MenuChoice &c = choices[i];
            float x = VW / 2 - 304, y = y0 + i * rowH;
            bool hov = false;
            bool act = kui::button("choice" + std::to_string(i), x, y, 608, 35, k > 0.2f, &hov);
            kui::image("ui/bg-choice.png", x, y, k);
            if (c.chosenBefore) {
                CMat half = kui::opacity(0.5f);
                kui::image("ui/bt-cf-checked.png", x + 577, y + 2, k, &half);
            } else {
                CMat half = kui::opacity(0.5f);
                kui::image("ui/bt-cf-unchecked.png", x + 577, y + 2, k, &half);
            }
            TextStyle ts = kui::colored(st, "#000000");
            ts.textAlign = 0.5;
            auto sz = kui::measure(c.label, ts, 560);
            kui::text(c.label, ts, VW / 2, y + (35 - sz.second) / 2, 0.5f, 560, k * (hov ? 1.0f : 0.4f));
            if (act && k >= 0.99f) {
                result = c.value;
                in.done = true;
            }
        }
    };
    in.update = [&]() { return in.done; };
    g.scene.shownWindow = true;
    g.interact(in);
    return result;
}

// ------------------------------------------------------------------ extras

static void extrasScreen();

void Game::mainMenu() {
    Game &g = *this;
    double start = nowSeconds();
    bool animated = g_animateMM;
    g_animateMM = false;
    if (g.pendingNativeScreen == "scene_select") {
        g.pendingNativeScreen.clear();
        extern void sceneSelectScreen();
        sceneSelectScreen();
    }
    kui::focus("mm_start");
    while (true) {
        std::string choice;
        runScreen([&](ScreenResult &res) {
            drawMainMenuBackground(animated, nowSeconds() - start);
            float y = 540;
            struct Item {
                const char *id, *key;
                bool enabled;
            };
            bool extrasAvail = true;
            Item items[] = {{"start", "main_menu_start", true},
                            {"load", "main_menu_load", haveSaves()},
                            {"extra", "main_menu_extra", extrasAvail},
                            {"prefs", "main_menu_config", true},
                            {"quit", "main_menu_quit", true}};
            TextStyle st = kui::style("mm_button_text");
            float lh = kui::measure("Ag", st).second + 2;
            y -= lh * 5;
            for (auto &it : items) {
                if (kui::textButton(std::string("mm_") + it.id, g.ds(it.key), 65, y, it.enabled)) {
                    choice = it.id;
                    res.done = true;
                }
                y += lh;
            }
        });
        if (choice == "start") {
            g.inMainMenuContext = true;
            throw JumpSignal("start_from_mm");
        }
        if (choice == "load") {
            try {
                loadScreen(true);
            } catch (LoadSignal &l) {
                if (g.loadSlot(l.slot))
                    return;
            }
            continue;
        }
        if (choice == "extra") {
            extrasScreen();
            continue;
        }
        if (choice == "prefs") {
            prefsScreen(true);
            continue;
        }
        if (choice == "quit") {
            g.savePersistent();
            throw QuitSignal();
        }
    }
}

// extras are implemented in extras.cpp
extern void extrasMain();
static void extrasScreen() { extrasMain(); }

// ------------------------------------------------------------------ save thumbnails

void captureSaveThumbnail(std::vector<uint8_t> &jpeg) {
    jpeg.clear();
    extern bool encodeThumbnailJPEG(std::vector<uint8_t> & out);
    encodeThumbnailJPEG(jpeg);
}
