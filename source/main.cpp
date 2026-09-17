#include "audio.h"
#include "game.h"
#include "input.h"
#include "platform.h"
#include <SDL.h>
#include <SDL_image.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

enum PadButton {
    PAD_A = 0,
    PAD_B = 1,
    PAD_X = 2,
    PAD_Y = 3,
    PAD_LSTICK = 4,
    PAD_RSTICK = 5,
    PAD_L = 6,
    PAD_R = 7,
    PAD_ZL = 8,
    PAD_ZR = 9,
    PAD_PLUS = 10,
    PAD_MINUS = 11,
    PAD_DLEFT = 12,
    PAD_DUP = 13,
    PAD_DRIGHT = 14,
    PAD_DDOWN = 15,
};

static SDL_Window *g_win = nullptr;
static SDL_Joystick *g_joy = nullptr;
static bool g_mouseDown = false, g_fingerDown = false;

struct Repeat {
    bool held = false;
    double next = 0;
    bool fire(bool down, double now) {
        if (!down) {
            held = false;
            return false;
        }
        if (!held) {
            held = true;
            next = now + 0.35;
            return true;
        }
        if (now >= next) {
            next = now + 0.08;
            return true;
        }
        return false;
    }
};
static Repeat rUp, rDown, rLeft, rRight;

static void toVirtual(float wx, float wy, float &vx, float &vy) {
    int ww, wh, dw, dh;
    SDL_GetWindowSize(g_win, &ww, &wh);
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    float sx = ww > 0 ? (float)dw / ww : 1, sy = wh > 0 ? (float)dh / wh : 1;
    float x, y, w, h;
    gfx::screenRect(x, y, w, h);
    vx = (wx * sx - x) * VW / w;
    vy = (wy * sy - y) * VH / h;
}

static void testHooks(bool keysPhase);
bool platformPump() {
    Input &in = g_input;
#ifdef __SWITCH__
    if (!appletMainLoop())
        return false;
#endif
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            return false;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
                int dw, dh;
                SDL_GL_GetDrawableSize(g_win, &dw, &dh);
                gfx::setOutputSize(dw, dh);
            }
            break;
        case SDL_JOYDEVICEADDED:
            if (!g_joy)
                g_joy = SDL_JoystickOpen(e.jdevice.which);
            break;
        case SDL_JOYBUTTONDOWN:
            switch (e.jbutton.button) {
            case PAD_A:
                in.accept = true;
                break;
            case PAD_B:
                in.cancel = true;
                in.menu = G.cur && G.cur->type != IType::Screen;
                break;
            case PAD_X:
                in.hide = true;
                break;
            case PAD_Y:
                in.history = true;
                break;
            case PAD_L:
                in.rollback = true;
                break;
            case PAD_R:
                in.skipToggle = true;
                break;
            case PAD_ZL:
                in.autoToggle = true;
                break;
            case PAD_PLUS:
                in.menu = true;
                break;
            case PAD_MINUS:
                in.rollforward = true;
                break;
            }
            break;
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
            case SDLK_SPACE:
                in.accept = true;
                break;
            case SDLK_ESCAPE:
                in.cancel = true;
                in.menu = true;
                break;
            case SDLK_h:
                in.hide = true;
                break;
            case SDLK_t:
                in.history = true;
                break;
            case SDLK_PAGEUP:
                in.rollback = true;
                break;
            case SDLK_PAGEDOWN:
                in.rollforward = true;
                break;
            case SDLK_TAB:
                in.skipToggle = true;
                break;
            case SDLK_a:
                in.autoToggle = true;
                break;
            }
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (e.button.which == SDL_TOUCH_MOUSEID)
                break;
            if (e.button.button == SDL_BUTTON_LEFT) {
                toVirtual((float)e.button.x, (float)e.button.y, in.px, in.py);
                in.pointerPressed = true;
                g_mouseDown = true;
            } else if (e.button.button == SDL_BUTTON_RIGHT) {
                in.menu = true;
                in.cancel = true;
            } else if (e.button.button == SDL_BUTTON_MIDDLE) {
                in.hide = true;
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e.button.which == SDL_TOUCH_MOUSEID)
                break;
            if (e.button.button == SDL_BUTTON_LEFT) {
                toVirtual((float)e.button.x, (float)e.button.y, in.px, in.py);
                in.pointerReleased = true;
                g_mouseDown = false;
            }
            break;
        case SDL_MOUSEMOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID)
                break;
            toVirtual((float)e.motion.x, (float)e.motion.y, in.px, in.py);
            in.pointerMoved = true;
            break;
        case SDL_MOUSEWHEEL:
            if (e.wheel.y > 0)
                in.rollback = true;
            if (e.wheel.y < 0)
                in.rollforward = true;
            in.scroll -= e.wheel.y * 40.0f;
            break;
        case SDL_FINGERDOWN: {
            float x, y, w, h;
            gfx::screenRect(x, y, w, h);
            in.px = (e.tfinger.x * gfx::outW() - x) * VW / w;
            in.py = (e.tfinger.y * gfx::outH() - y) * VH / h;
            in.pointerPressed = true;
            g_fingerDown = true;
            break;
        }
        case SDL_FINGERUP: {
            float x, y, w, h;
            gfx::screenRect(x, y, w, h);
            in.px = (e.tfinger.x * gfx::outW() - x) * VW / w;
            in.py = (e.tfinger.y * gfx::outH() - y) * VH / h;
            in.pointerReleased = true;
            g_fingerDown = false;
            break;
        }
        case SDL_FINGERMOTION: {
            float x, y, w, h;
            gfx::screenRect(x, y, w, h);
            in.px = (e.tfinger.x * gfx::outW() - x) * VW / w;
            in.py = (e.tfinger.y * gfx::outH() - y) * VH / h;
            in.scroll -= e.tfinger.dy * VH;
            break;
        }
        }
    }
    in.pointerDown = g_mouseDown || g_fingerDown;
    if (!in.pointerMoved && !in.pointerPressed && !in.pointerReleased && !in.pointerDown) {
        // keep last pointer position for hover checks
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        toVirtual((float)mx, (float)my, in.px, in.py);
#ifdef __SWITCH__
        in.px = in.py = -1;
#endif
    }
    const Uint8 *ks = SDL_GetKeyboardState(nullptr);
    auto btn = [&](int b) { return g_joy && SDL_JoystickGetButton(g_joy, b); };
    int ax = g_joy ? SDL_JoystickGetAxis(g_joy, 0) : 0, ay = g_joy ? SDL_JoystickGetAxis(g_joy, 1) : 0;
    double now = nowSeconds();
    in.up = rUp.fire(btn(PAD_DUP) || ay < -20000 || ks[SDL_SCANCODE_UP], now);
    in.down = rDown.fire(btn(PAD_DDOWN) || ay > 20000 || ks[SDL_SCANCODE_DOWN], now);
    in.left = rLeft.fire(btn(PAD_DLEFT) || ax < -20000 || ks[SDL_SCANCODE_LEFT], now);
    in.right = rRight.fire(btn(PAD_DRIGHT) || ax > 20000 || ks[SDL_SCANCODE_RIGHT], now);
    in.skipHeld = btn(PAD_ZR) || ks[SDL_SCANCODE_LCTRL] || ks[SDL_SCANCODE_RCTRL];
    testHooks(true);
    if (g_joy) {
        int ry = SDL_JoystickGetAxis(g_joy, 3);
        if (ry > 8000 || ry < -8000)
            in.scroll += ry / 32767.0f * 12.0f;
    }
#ifdef __SWITCH__
    static double lastModeCheck = 0;
    if (now - lastModeCheck > 1.0) {
        lastModeCheck = now;
        bool hh = platform::isHandheld();
        int w = hh ? 1280 : 1920, h = hh ? 720 : 1080;
        if (gfx::outW() != w) {
            SDL_SetWindowSize(g_win, w, h);
            gfx::setOutputSize(w, h);
        }
    }
#endif
    return true;
}

// Test hooks (PC): KS_SHOTS="t1,t2,..." saves screenshots at those times;
// KS_KEYS="t:key,t:key" injects inputs (keys: A B X Y L R + U D Lf Rt ZR skip).
// Test settings from the environment, or from <user dir>/test.txt lines "KS_SHOTS=..." (for the console/emulators).
static const char *testSetting(const char *name) {
    if (const char *e = getenv(name))
        return e;
    static std::vector<std::pair<std::string, std::string>> fileVals;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        std::vector<uint8_t> data;
        if (readFile(userPath("test.txt"), data)) {
            std::string all(data.begin(), data.end());
            size_t p = 0;
            while (p < all.size()) {
                size_t nl = all.find('\n', p);
                if (nl == std::string::npos)
                    nl = all.size();
                std::string line = all.substr(p, nl - p);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                size_t eq = line.find('=');
                if (eq != std::string::npos)
                    fileVals.emplace_back(line.substr(0, eq), line.substr(eq + 1));
                p = nl + 1;
            }
        }
    }
    for (auto &kv : fileVals)
        if (kv.first == name)
            return kv.second.c_str();
    return nullptr;
}

static void testHooks(bool keysPhase) {
    static double start = nowSeconds();
    double t = nowSeconds() - start;
    static std::vector<double> shots;
    static std::vector<std::pair<double, std::string>> keys;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        if (const char *s = testSetting("KS_SHOTS")) {
            std::string str = s;
            size_t p = 0;
            while (p < str.size()) {
                size_t c = str.find(',', p);
                if (c == std::string::npos)
                    c = str.size();
                shots.push_back(atof(str.substr(p, c - p).c_str()));
                p = c + 1;
            }
        }
        if (const char *s = testSetting("KS_KEYS")) {
            std::string str = s;
            size_t p = 0;
            while (p < str.size()) {
                size_t c = str.find(',', p);
                if (c == std::string::npos)
                    c = str.size();
                std::string item = str.substr(p, c - p);
                size_t colon = item.find(':');
                if (colon != std::string::npos)
                    keys.emplace_back(atof(item.substr(0, colon).c_str()), item.substr(colon + 1));
                p = c + 1;
            }
        }
    }
    for (auto it = keys.begin(); keysPhase && it != keys.end();) {
        if (t >= it->first) {
            Input &in = g_input;
            const std::string &k = it->second;
            if (k == "A")
                in.accept = true;
            else if (k == "B")
                in.cancel = in.menu = true;
            else if (k == "X")
                in.hide = true;
            else if (k == "Y")
                in.history = true;
            else if (k == "L")
                in.rollback = true;
            else if (k == "R")
                in.skipToggle = true;
            else if (k == "+")
                in.menu = true;
            else if (k == "U")
                in.up = true;
            else if (k == "D")
                in.down = true;
            else if (k == "Lf")
                in.left = true;
            else if (k == "Rt")
                in.right = true;
            else if (k == "S")
                G.configV.inst()->attrs["skipping"] = Value::str("slow");
            it = keys.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = shots.begin(); !keysPhase && it != shots.end();) {
        if (t >= *it) {
            int w = gfx::outW(), h = gfx::outH();
            std::vector<uint8_t> px((size_t)w * h * 4);
            extern void readScreenPixels(int w, int h, uint8_t *out);
            readScreenPixels(w, h, px.data());
            SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ABGR8888);
            for (int y = 0; y < h; y++)
                memcpy((uint8_t *)s->pixels + y * s->pitch, &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
            char name[64];
            snprintf(name, sizeof name, "shot_%05.1f.png", *it);
            IMG_SavePNG(s, userPath(name).c_str());
            SDL_FreeSurface(s);
            logf("screenshot %s", name);
            it = shots.erase(it);
        } else {
            ++it;
        }
    }
}

void platformSwap() {
    testHooks(false);
    SDL_GL_SwapWindow(g_win);
}

static int gameThread(void *) {
#ifdef __SWITCH__
    bool handheld = platform::isHandheld();
    g_win = SDL_CreateWindow("Katawa Shoujo", 0, 0, handheld ? 1280 : 1920, handheld ? 720 : 1080, SDL_WINDOW_OPENGL);
#else
    g_win = SDL_CreateWindow("Katawa Shoujo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
                             SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
#endif
    if (!g_win) {
        logf("SDL_CreateWindow: %s", SDL_GetError());
        return 1;
    }
    SDL_GLContext ctx = SDL_GL_CreateContext(g_win);
    if (!ctx) {
        logf("SDL_GL_CreateContext: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(getenv("KS_NOVSYNC") ? 0 : 1);
    int dw, dh;
    SDL_GL_GetDrawableSize(g_win, &dw, &dh);
    gfx::setOutputSize(dw, dh);
    gfx::init();
    IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    if (!texture::init() || !text::init()) {
        logf("asset init failed");
        return 1;
    }
    audio::init();
    if (SDL_NumJoysticks() > 0)
        g_joy = SDL_JoystickOpen(0);
    if (!G.init()) {
        logf("game init failed");
        return 1;
    }
    G.run();
    G.savePersistent();
    audio::shutdown();
    texture::shutdown();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(g_win);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
#ifndef __SWITCH__
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    platform::init();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) != 0) {
        logf("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_Thread *t = SDL_CreateThreadWithStackSize(gameThread, "game", 64 * 1024 * 1024, nullptr);
    int status = 0;
    SDL_WaitThread(t, &status);
    SDL_Quit();
    platform::shutdown();
    return status;
}
