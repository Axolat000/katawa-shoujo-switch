#include "audio.h"
#include "game.h"
#include "input.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <random>

// shared UI helpers from screens.cpp
namespace kui {
void image(const std::string &file, float x, float y, float alpha, const CMat *cm, float w, float h);
TexImage *tex(const std::string &file);
TextStyle style(const std::string &name, double size);
TextStyle colored(TextStyle st, const char *hex);
std::pair<float, float> text(const std::string &s, const TextStyle &st, float x, float y, float xalign, float maxW, float alpha);
std::pair<float, float> measure(const std::string &s, const TextStyle &st, float maxW);
void beginFrame();
bool button(const std::string &id, float x, float y, float w, float h, bool sensitive, bool *hovered);
bool textButton(const std::string &id, const std::string &label, float x, float y, bool sensitive, float xalign,
                bool selected, double size, float *outW);
bool widgetButton(const std::string &id, const std::string &label, const std::string &img, float x, float y,
                  float xsize, float ysize, float widgetYOffset, float textXOffset, const std::string &state);
void ensureFocus(const std::string &id);
void focus(const std::string &id);
void dim(float a);
} // namespace kui

bool playMovie(const std::string &file, bool skippable);
Value tupleAtStr(const Value &t, size_t i);

static CMat opacityM(float o) {
    double v[20] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, o, 0};
    return CMat::fromList(v, 20);
}
static CMat desatM(float o) {
    double r = 0.2126, g = 0.7152, b = 0.0722;
    double v[20] = {r, g, b, 0, 0, r, g, b, 0, 0, r, g, b, 0, 0, 0, 0, 0, o, 0};
    return CMat::fromList(v, 20);
}

struct ExResult {
    bool done = false;
};

static void runPage(const std::function<void(ExResult &)> &body) {
    ExResult r;
    Interaction in;
    in.type = IType::Screen;
    in.suppressWindow = true;
    in.draw = [&]() {
        kui::beginFrame();
        body(r);
    };
    in.update = [&]() { return r.done; };
    G.interact(in);
}

static void background() {
    extern void pageBackgroundMM();
    pageBackgroundMM();
}

static void caption(const std::string &key) {
    TextStyle cap = kui::colored(kui::style("page_caption", -1), "#00000066");
    cap.bold = true;
    kui::text(G.ds(key), cap, 180, 120, 0, 0, 1);
}

static bool retButton() {
    return kui::widgetButton("return", G.ds("return_button_text"), "ui/bt-return.png", 540, 450, 100, 30, 3, 30, "button") ||
           g_input.cancel || g_input.menu;
}

static Value callStore(const char *name, std::vector<Value> args = {}, std::vector<std::pair<std::string, Value>> kw = {}) {
    try {
        CallArgs a;
        a.pos = args;
        a.kw = kw;
        return PY.call(G.storeGet(name), a);
    } catch (PyError &e) {
        logf("%s: %s %s", name, e.type.c_str(), e.msg.c_str());
        return Value();
    }
}

// ------------------------------------------------------------------ jukebox

static void musicRoom() {
    Game &g = G;
    float scroll = 0;
    runPage([&](ExResult &r) {
        background();
        caption("music_page_caption");
        Value avail = callStore("get_available_music");
        std::string playing = audio::playing("music");
        TextStyle lab = kui::colored(kui::style("prefs_label", -1), "#00000066");
        std::string nowName;
        ListObj *tracks = g.storeGet("ex_m_tracks").list();
        if (tracks)
            for (auto &t : tracks->v)
                if (tupleAtStr(t, 1).s() == playing && !playing.empty())
                    nowName = tupleAtStr(t, 0).s();
        kui::text(nowName.empty() ? " " : g.ds("music_now_playing") + ": " + nowName, lab, 180, 150, 0, 0, 1);
        float y0 = 180, rowH = 34, listH = 267;
        if (tracks) {
            float maxScroll = std::max(0.0f, tracks->v.size() * rowH - listH);
            scroll = std::max(0.0f, std::min(scroll + g_input.scroll, maxScroll));
            gfx::pushClip(180, y0, 620, y0 + listH);
            for (size_t i = 0; i < tracks->v.size(); i++) {
                std::string name = tupleAtStr(tracks->v[i], 0).s(), file = tupleAtStr(tracks->v[i], 1).s();
                float y = y0 + i * rowH - scroll;
                bool ok = avail.list() && PY.contains(avail, Value::str(file));
                std::string id = "track" + std::to_string(i);
                if (ok) {
                    if (kui::textButton(id, name, 184, y, true, 0, file == playing, 22, nullptr)) {
                        audio::play("music", file, 0, 0.5, -1, true);
                    }
                } else {
                    CMat cm = opacityM(0.1f);
                    kui::image("ui/bg-lockedtrack.png", 194, y + 1, 1, &cm, -1, -1);
                }
            }
            gfx::popClip();
        }
        kui::ensureFocus("track0");
        if (kui::widgetButton("stop", g.ds("music_stop_button_text"), "ui/bt-musicstop.png", 450, 449, 80, 30, 3, 30, "button"))
            audio::stop("music", 0.5);
        if (retButton())
            r.done = true;
    });
    audio::play("music", valueStr(G.storeGet("music_menus")), 5.0, 0.5, -1, true);
}

// ------------------------------------------------------------------ cinema

static void cinema() {
    Game &g = G;
    while (true) {
        std::string chosen;
        runPage([&](ExResult &r) {
            background();
            caption("video_page_caption");
            Value videos = g.displayString("videos");
            Value seen = g.persistentGet("seen_videos");
            if (ListObj *vl = videos.list())
                for (size_t i = 0; i < vl->v.size(); i++) {
                    std::string file = tupleAtStr(vl->v[i], 1).s();
                    std::string tn = file.substr(0, file.size() - 4) + "_tn.jpg";
                    float x = 215 + (i % 3) * 130, y = 175 + (i / 3) * 105;
                    bool unlocked = seen.list() && PY.contains(seen, Value::str(file));
                    std::string id = "vid" + std::to_string(i);
                    bool hov = false;
                    if (unlocked) {
                        if (kui::button(id, x, y, 110, 85, true, &hov)) {
                            chosen = file;
                            r.done = true;
                        }
                        CMat base = opacityM(hov ? 1.0f : 0.4f);
                        kui::image("ui/bt-cg-locked.png", x, y, 1, &base, -1, -1);
                        CMat ds = desatM(1);
                        kui::image(tn, x + 5, y + 5, 1, hov ? nullptr : &ds, 100, 75);
                    } else {
                        CMat dis = opacityM(0.1f);
                        kui::image("ui/bt-cg-locked.png", x, y, 1, &dis, -1, -1);
                    }
                }
            kui::ensureFocus("vid0");
            if (retButton())
                r.done = true;
        });
        if (chosen.empty())
            return;
        audio::stop("music", 0.5);
        playMovie(chosen, true);
        audio::play("music", valueStr(g.storeGet("music_menus")), 5.0, 0.5, -1, false);
    }
}

// ------------------------------------------------------------------ scene select (library)

void sceneSelectScreen() {
    Game &g = G;
    std::string filter = "Act 1";
    float scroll = 0;
    while (true) {
        std::string what, where;
        runPage([&](ExResult &r) {
            background();
            caption("scene_page_caption");
            static const char *buttons[] = {"Act 1", "Emi", "Hanako", "Lilly", "Rin", "Shizune"};
            static const char *keys[] = {"", "name_emi", "name_ha", "name_li", "name_rin", "name_shi"};
            float bx = 180;
            for (int i = 0; i < 6; i++) {
                std::string label = i == 0 ? g.ds("act_term") + " 1" : g.ds(keys[i]);
                bool active = filter == buttons[i];
                bool avail = callStore("get_available_scenes", {Value::str(buttons[i])}).truthy();
                float w = 0;
                if (kui::textButton(std::string("f") + buttons[i], label, bx, 150, !active && avail, 0, active, 22, &w)) {
                    filter = buttons[i];
                    scroll = 0;
                }
                bx += w + 18;
            }
            Value scenes = callStore("get_available_scenes", {Value::str(filter)},
                                     {{"include_locked", Value::boolean(true)}, {"include_acts", Value::boolean(true)}});
            std::vector<std::string> openActs;
            if (ListObj *sl = scenes.list())
                for (auto &s : sl->v)
                    if (tupleAtStr(s, 4).truthy())
                        openActs.push_back(valueStr(tupleAtStr(s, 3)));
            float y0 = 185, rowH = 34, listH = 270;
            std::string hoverDesc;
            if (ListObj *sl = scenes.list()) {
                float maxScroll = std::max(0.0f, sl->v.size() * rowH - listH);
                scroll = std::max(0.0f, std::min(scroll + g_input.scroll, maxScroll));
                gfx::pushClip(180, y0, 620, y0 + listH);
                std::string actmark = valueStr(g.storeGet("rp_actmark"));
                int row = 0;
                for (size_t i = 0; i < sl->v.size(); i++) {
                    const Value &s = sl->v[i];
                    std::string name = valueStr(tupleAtStr(s, 0)), label = valueStr(tupleAtStr(s, 1));
                    float y = y0 + row * rowH - scroll;
                    if (label == actmark) {
                        std::string path = valueStr(tupleAtStr(s, 3));
                        if (std::find(openActs.begin(), openActs.end(), path) != openActs.end()) {
                            TextStyle st = kui::colored(kui::style("mm_button_text", -1), "#00000066");
                            kui::text(name, st, 184, y, 0, 0, 1);
                            row++;
                        }
                        continue;
                    }
                    std::string id = "scene" + std::to_string(i);
                    if (tupleAtStr(s, 4).truthy()) {
                        if (kui::textButton(id, name, 199, y, true, 0, false, 22, nullptr)) {
                            what = name;
                            where = label;
                            r.done = true;
                        }
                        extern bool kuiFocused(const std::string &id);
                        if (kuiFocused(id)) {
                            hoverDesc = valueStr(tupleAtStr(s, 2));
                            if (y < y0)
                                scroll -= y0 - y;
                            if (y + rowH > y0 + listH)
                                scroll += y + rowH - (y0 + listH);
                        }
                    } else {
                        CMat cm = opacityM(0.1f);
                        kui::image("ui/bg-lockedtrack.png", 207, y + 1, 1, &cm, -1, -1);
                    }
                    row++;
                }
                gfx::popClip();
            }
            Value pct = callStore("get_completion_percentage");
            TextStyle lab = kui::colored(kui::style("prefs_label", -1), "#00000066");
            kui::text(pyFormat(g.ds("scene_completion_label"), pct), lab, 180, 450, 0, 0, 1);
            if (!hoverDesc.empty() && hoverDesc != "True") {
                TextStyle ds = kui::style("default", 18);
                ds.textAlign = 0.5;
                auto sz = kui::measure(hoverDesc, ds, 780);
                kui::text(hoverDesc, ds, 400, 588 - sz.second, 0.5f, 780, 1);
            }
            if (retButton()) {
                what = "";
                r.done = true;
            }
        });
        if (where.empty())
            return;
        // launch the scene (replay flow)
        g.storeSet("readback_buffer", mkList());
        callStore("init_vars");
        g.storeSet("last_scene_label", Value::str(where));
        g.storeSet("save_name", Value::str(where));
        audio::stop("music", 0.5);
        std::string target = where;
        if (!g.storeGet("playthroughflag").truthy() && g.hasLabel("replay_" + where))
            target = "replay_" + where;
        g.sceneClear("master");
        g.showName("black", {}, "master", "", 0, {}, nullptr, Value());
        g.rollbackLog.clear();
        g.inMainMenuContext = false;
        throw JumpSignal(target);
    }
}

// ------------------------------------------------------------------ gallery

static bool imageSeen(const std::string &name) {
    Value seen = G.persistentGet("_seen_images");
    DictObj *d = seen.dict();
    if (!d)
        return false;
    std::vector<Value> parts;
    size_t p = 0;
    while (p < name.size()) {
        size_t sp = name.find(' ', p);
        if (sp == std::string::npos)
            sp = name.size();
        if (sp > p)
            parts.push_back(Value::str(name.substr(p, sp - p)));
        p = sp + 1;
    }
    return d->find(mkTuple(parts)) != nullptr;
}

static bool isR18(const std::vector<std::string> &images) {
    static const char *keys[] = {"evh", "hanako_after", "rin_wet", "rin_pair", "shizu_couch"};
    for (auto &i : images)
        for (auto k : keys)
            if (i.find(k) != std::string::npos)
                return true;
    return false;
}

static void showGalleryImages(const std::vector<std::string> &images) {
    Game &g = G;
    std::vector<std::string> unlocked, locked;
    for (auto &i : images)
        (imageSeen(i) ? unlocked : locked).push_back(i);
    bool pillsBg = !images.empty() && images[0].size() >= 5 && images[0].substr(images[0].size() - 5) == "pills";
    for (auto &name : unlocked) {
        DispP d = toDisp(Value::str(name));
        bool stop = false;
        double start = nowSeconds();
        runPage([&](ExResult &r) {
            gfx::drawSolid(matRect(0, 0, VW, VH), 0, 0, 0, 1);
            if (pillsBg)
                kui::image("ui/tc-neutral.png", 0, 0, 1, nullptr, -1, -1);
            double t = nowSeconds() - start;
            RenderP surf = d->render(VW, VH, t, t);
            auto root = Render::make(VW, VH);
            d->place(*root, 0, 0, VW, VH, surf);
            drawRender(root, Mat(), (float)std::min(1.0, t / 0.5));
            if (g_input.accept || g_input.pointerReleased)
                r.done = true;
            if (g_input.cancel || g_input.menu) {
                stop = true;
                r.done = true;
            }
        });
        if (stop)
            return;
    }
    if (!locked.empty()) {
        std::string text = locked.size() == 1 ? g.ds("gallery_onelocked")
                                              : pyFormat(g.ds("gallery_manylocked"), Value::integer((int64_t)locked.size()));
        runPage([&](ExResult &r) {
            kui::image("ui/bg-ex-gallery-lockedimage.png", 0, 0, 1, nullptr, -1, -1);
            TextStyle st = kui::colored(kui::style("prefs_label", -1), "#00000066");
            kui::text(text, st, 400, 400, 0.5f, 0, 1);
            if (g_input.accept || g_input.pointerReleased || g_input.cancel || g_input.menu)
                r.done = true;
        });
    }
}

static void gallery() {
    Game &g = G;
    struct Btn {
        std::string thumb;
        std::vector<std::string> images;
    };
    std::vector<Btn> btns;
    if (ListObj *all = g.storeGet("ex_g_images").list())
        for (auto &e : all->v) {
            Btn b;
            if (ListObj *t = e.list()) {
                for (auto &x : t->v) {
                    std::string s = valueStr(x);
                    if (s.compare(0, 6, "thumb/") == 0)
                        b.thumb = "event/" + s;
                    else
                        b.images.push_back(s);
                }
            } else {
                b.images.push_back(valueStr(e));
            }
            btns.push_back(b);
        }
    int page = 0;
    int pages = std::max(1, (int)((btns.size() + 11) / 12));
    while (true) {
        int chosen = -1;
        runPage([&](ExResult &r) {
            background();
            TextStyle cap = kui::colored(kui::style("page_caption", -1), "#00000066");
            cap.bold = true;
            kui::text(g.ds("gallery_page_caption"), cap, 180, 120, 0, 0, 1);
            for (int i = 0; i < 12; i++) {
                size_t bi = page * 12 + i;
                if (bi >= btns.size())
                    break;
                const Btn &b = btns[bi];
                float x = 175 + (i % 4) * 115, y = 160 + (i / 4) * 90;
                bool r18 = isR18(b.images);
                bool any = false;
                for (auto &im : b.images)
                    any = any || imageSeen(im);
                std::string id = "cg" + std::to_string(bi);
                if (r18 || !any) {
                    CMat cm = r18 ? desatM(0.2f) : opacityM(0.1f);
                    kui::image("ui/bt-cg-locked.png", x, y, 1, &cm, -1, -1);
                    continue;
                }
                bool hov = false;
                if (kui::button(id, x, y, 110, 85, true, &hov)) {
                    chosen = (int)bi;
                    r.done = true;
                }
                CMat base = opacityM(hov ? 1.0f : 0.4f);
                kui::image("ui/bt-cg-locked.png", x, y, 1, &base, -1, -1);
                if (!b.thumb.empty()) {
                    CMat ds = desatM(1);
                    kui::image(b.thumb, x + 5, y + 5, 1, hov ? nullptr : &ds, 100, 75);
                }
            }
            if (pages > 1) {
                TextStyle pd = kui::colored(kui::style("mm_button_text", -1), "#00000066");
                auto sz = kui::text(g.ds("gallery_num_page_prefix") + ": ", pd, 180, 448, 0, 0, 1);
                float x = 180 + sz.first;
                float w = 0;
                if (kui::textButton("pgprev", "<", x, 448, page > 0, 0, false, 22, &w))
                    page--;
                x += w + 8;
                for (int p = 0; p < pages; p++) {
                    if (kui::textButton("pg" + std::to_string(p), std::to_string(p + 1), x, 448, true, 0, p == page, 22, &w))
                        page = p;
                    x += w + 8;
                }
                if (kui::textButton("pgnext", ">", x, 448, page < pages - 1, 0, false, 22, &w))
                    page++;
                if (g_input.rollback && page > 0)
                    page--;
                if (g_input.rollforward && page < pages - 1)
                    page++;
            }
            if (retButton())
                r.done = true;
        });
        if (chosen < 0)
            return;
        showGalleryImages(btns[chosen].images);
    }
}

// ------------------------------------------------------------------ extras menu

void extrasMain() {
    Game &g = G;
    audio::play("music", valueStr(g.storeGet("music_menus")), 0, 0.5, -1, true);
    while (true) {
        std::string choice;
        runPage([&](ExResult &r) {
            background();
            caption("extra_menu_caption");
            struct E {
                const char *id, *label, *img;
                bool enabled;
            };
            E items[] = {{"music", "extra_music_button_label", "ui/sd-lilly.png", callStore("get_available_music").truthy()},
                         {"gallery", "extra_gallery_button_label", "ui/sd-rin.png", callStore("get_available_images").truthy()},
                         {"scenes", "extra_scene_button_label", "ui/sd-shizune.png", callStore("get_available_scenes").truthy()},
                         {"cinema", "extra_opening_button_label", "ui/sd-emi.png", g.persistentGet("seen_videos").truthy()}};
            float x = 180;
            TextStyle lab = kui::style("prefs_label", -1);
            for (auto &e : items) {
                TexImage *t = kui::tex(e.img);
                float w = std::max(80.0f, (float)t->w);
                float bottom = 330;
                bool hov = false;
                bool act = kui::button(e.id, x, bottom - t->h - 30, w, (float)t->h + 30, e.enabled, &hov);
                std::string img = e.img;
                if (hov)
                    img = img.substr(0, img.size() - 4) + "-c.png";
                CMat cm = opacityM(e.enabled ? 1.0f : 0.4f);
                kui::image(img, x + (w - t->w) / 2, bottom - t->h - 30, 1, e.enabled ? nullptr : &cm, -1, -1);
                const char *col = !e.enabled ? "#00000019" : hov ? "#000000" : "#00000066";
                kui::text(g.ds(e.label), kui::colored(lab, col), x + w / 2, bottom - 26, 0.5f, 0, 1);
                if (act) {
                    choice = e.id;
                    r.done = true;
                }
                x += w + 15;
            }
            // return: sd-hanako at (540, 346)
            TexImage *t = kui::tex("ui/sd-hanako.png");
            bool hov = false;
            if (kui::button("return", 540, 346, (float)std::max(100, t->w), (float)t->h + 30, true, &hov) ||
                g_input.cancel || g_input.menu) {
                choice = "return";
                r.done = true;
            }
            kui::image(hov ? "ui/sd-hanako-c.png" : "ui/sd-hanako.png", 540, 346, 1, nullptr, -1, -1);
            CMat rb = opacityM(hov ? 1.0f : 0.4f);
            kui::image("ui/bt-return.png", 540, 346 + t->h + 3, 1, &rb, -1, -1);
            kui::text(g.ds("return_button_text"), kui::colored(lab, hov ? "#000000" : "#00000066"), 570, 346 + t->h, 0, 0, 1);
            kui::ensureFocus("return");
        });
        if (choice == "return")
            return;
        if (choice == "music")
            musicRoom();
        else if (choice == "gallery")
            gallery();
        else if (choice == "cinema")
            cinema();
        else if (choice == "scenes") {
            callStore("menu_init");
            sceneSelectScreen();
        }
    }
}

// ------------------------------------------------------------------ save thumbnail (raw RGBA 100x75)

bool encodeThumbnailJPEG(std::vector<uint8_t> &out) {
    Game &g = G;
    auto root = g.scene.computeScene();
    g_frameTime = nowSeconds();
    RenderP r = root->render(VW, VH, 0, 0);
    const int tw = 100, th = 75;
    RenderTarget *rt = gfx::acquireTarget(200, 150);
    gfx::bindTarget(rt, VW, VH);
    gfx::clear(0, 0, 0, 1);
    drawRender(r, Mat(), 1.0f);
    std::vector<uint8_t> px;
    gfx::readPixels(rt, px);
    gfx::releaseTarget(rt);
    gfx::bindScreen();
    out.resize(4 + tw * th * 4);
    out[0] = tw & 0xff;
    out[1] = tw >> 8;
    out[2] = th & 0xff;
    out[3] = th >> 8;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++)
            for (int c = 0; c < 4; c++) {
                int s = 0;
                for (int dy = 0; dy < 2; dy++)
                    for (int dx = 0; dx < 2; dx++)
                        s += px[((y * 2 + dy) * 200 + x * 2 + dx) * 4 + c];
                out[4 + (y * tw + x) * 4 + c] = (uint8_t)(c == 3 ? 255 : s / 4);
            }
    return true;
}

// ------------------------------------------------------------------ drugsDisp (words appearing on white)

struct DrugsDisp : Displayable {
    float w = 1600, h = 800;
    double length = 22, fadein = 1;
    struct Word {
        std::string text;
        float x, y, size;
    };
    std::vector<Word> words;
    double lastSt = -1;
    void make() {
        words.clear();
        std::vector<std::string> list;
        if (ListObj *l = G.displayString("drugs_wordlist").list())
            for (auto &x : l->v)
                list.push_back(valueStr(x));
        if (list.empty())
            return;
        std::vector<std::string> all;
        for (int i = 0; i < 5; i++) {
            std::shuffle(list.begin(), list.end(), std::mt19937((unsigned)(nowSeconds() * 1000) + i));
            all.insert(all.end(), list.begin(), list.end());
        }
        for (auto &t : all)
            words.push_back({t, (float)(rand() % (int)w), (float)(rand() % (int)h), (float)(30 + rand() % 70)});
    }
    RenderP render(float, float, double st, double) override {
        if (st < lastSt || words.empty())
            make();
        lastSt = st;
        auto rv = Render::make(w, h);
        rv->op = Render::CUSTOM;
        double per = words.empty() ? 1 : length / words.size();
        std::string font = G.ds("sayfont") != "font/playtime.ttf" ? G.ds("sayfont") : "font/gentium.ttf";
        auto snapshot = words;
        rv->draw = [snapshot, per, st, font, this](const Mat &m, float alpha) {
            gfx::drawSolid(matMul(m, matRect(0, 0, w, h)), 1, 1, 1, alpha);
            for (size_t i = 0; i < snapshot.size(); i++) {
                double a = (st - i * per) / fadein;
                if (a <= 0)
                    break;
                TextStyle ts;
                ts.font = font;
                ts.size = snapshot[i].size;
                ts.r = ts.g = ts.b = 0;
                auto L = text::layout(snapshot[i].text, ts, 2000);
                L->draw(matMul(m, matTranslate(snapshot[i].x - L->w / 2, snapshot[i].y - L->h / 2)), alpha * (float)std::min(1.0, a), -1);
            }
        };
        return rv;
    }
};

DispP makeDrugsDisp(const Value &v) {
    auto d = std::make_shared<DrugsDisp>();
    d->kind = "drugsDisp";
    if (InstObj *in = v.inst()) {
        if (in->get("width").isNum())
            d->w = (float)in->get("width").num();
        if (in->get("height").isNum())
            d->h = (float)in->get("height").num();
    }
    return d;
}
