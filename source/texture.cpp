#include "texture.h"
#include "gfx.h"
#include <SDL.h>
#include <SDL_image.h>
#include <algorithm>
#include <deque>
#include <unordered_map>

namespace texture {

static std::unordered_map<std::string, TexImage *> g_images;
static uint64_t g_frame = 1;
static size_t g_totalBytes = 0;
#ifdef __SWITCH__
static const size_t BUDGET = 700u * 1024 * 1024;
#else
static const size_t BUDGET = 1200u * 1024 * 1024;
#endif

static SDL_Thread *g_thread = nullptr;
static SDL_mutex *g_mutex = nullptr;
static SDL_cond *g_cond = nullptr;
static std::deque<TexImage *> g_queue;
static std::vector<std::pair<TexImage *, SDL_Surface *>> g_done;
static TexImage *g_working = nullptr;
static bool g_quit = false;

static SDL_Surface *decode(const std::string &file) {
    SDL_Surface *s = IMG_Load(romfsPath("game/" + file).c_str());
    if (!s) {
        logf("IMG_Load failed %s: %s", file.c_str(), IMG_GetError());
        return nullptr;
    }
    SDL_Surface *c;
    if (s->format->Amask || s->format->palette)
        c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ABGR8888, 0);
    else
        c = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB24, 0);
    SDL_FreeSurface(s);
    return c;
}

static int worker(void *) {
    SDL_LockMutex(g_mutex);
    while (!g_quit) {
        if (g_queue.empty()) {
            SDL_CondWait(g_cond, g_mutex);
            continue;
        }
        TexImage *img = g_queue.front();
        g_queue.pop_front();
        g_working = img;
        std::string file = img->file;
        SDL_UnlockMutex(g_mutex);
        SDL_Surface *s = decode(file);
        SDL_LockMutex(g_mutex);
        g_working = nullptr;
        g_done.push_back({img, s});
        SDL_CondBroadcast(g_cond);
    }
    SDL_UnlockMutex(g_mutex);
    return 0;
}

static void upload(TexImage *img, SDL_Surface *s) {
    if (!s) {
        img->state = -1;
        return;
    }
    bool rgb = s->format->BytesPerPixel == 3;
    int bpp = rgb ? 3 : 4;
    SDL_LockSurface(s);
    const uint8_t *px = (const uint8_t *)s->pixels;
    std::vector<uint8_t> tight;
    if (s->pitch != s->w * bpp) {
        tight.resize((size_t)s->w * s->h * bpp);
        for (int y = 0; y < s->h; y++)
            memcpy(&tight[(size_t)y * s->w * bpp], px + y * s->pitch, (size_t)s->w * bpp);
        px = tight.data();
    }
    img->tex = gfx::uploadTexture(px, s->w, s->h, rgb, true);
    SDL_UnlockSurface(s);
    img->w = s->w;
    img->h = s->h;
    img->bytes = (size_t)s->w * s->h * bpp * 4 / 3;
    g_totalBytes += img->bytes;
    img->state = 2;
    SDL_FreeSurface(s);
}

static void drainDone() {
    std::vector<std::pair<TexImage *, SDL_Surface *>> done;
    SDL_LockMutex(g_mutex);
    done.swap(g_done);
    SDL_UnlockMutex(g_mutex);
    for (auto &d : done) {
        if (d.first->state == 1)
            upload(d.first, d.second);
        else if (d.second)
            SDL_FreeSurface(d.second);
    }
}

bool init() {
    std::vector<uint8_t> data;
    if (!readFile(romfsPath("data/images.tsv"), data)) {
        logf("missing data/images.tsv");
        return false;
    }
    std::string all(data.begin(), data.end());
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        if (nl == std::string::npos)
            nl = all.size();
        std::string line = all.substr(pos, nl - pos);
        pos = nl + 1;
        size_t t1 = line.find('\t');
        size_t t2 = t1 == std::string::npos ? std::string::npos : line.find('\t', t1 + 1);
        if (t2 == std::string::npos)
            continue;
        TexImage *img = new TexImage();
        img->file = line.substr(0, t1);
        img->w = atoi(line.c_str() + t1 + 1);
        img->h = atoi(line.c_str() + t2 + 1);
        img->known = true;
        g_images[img->file] = img;
    }
    g_mutex = SDL_CreateMutex();
    g_cond = SDL_CreateCond();
    g_thread = SDL_CreateThread(worker, "decoder", nullptr);
    logf("texture: %d images", (int)g_images.size());
    return true;
}

void shutdown() {
    SDL_LockMutex(g_mutex);
    g_quit = true;
    SDL_CondBroadcast(g_cond);
    SDL_UnlockMutex(g_mutex);
    SDL_WaitThread(g_thread, nullptr);
}

uint64_t frameNo() { return g_frame; }

static void evict() {
    if (g_totalBytes <= BUDGET)
        return;
    std::vector<TexImage *> cands;
    for (auto &kv : g_images)
        if (kv.second->state == 2 && kv.second->lastUse + 5 < g_frame)
            cands.push_back(kv.second);
    std::sort(cands.begin(), cands.end(), [](TexImage *a, TexImage *b) { return a->lastUse < b->lastUse; });
    for (TexImage *img : cands) {
        if (g_totalBytes <= BUDGET * 3 / 4)
            break;
        gfx::deleteTexture(img->tex);
        img->tex = 0;
        g_totalBytes -= img->bytes;
        img->bytes = 0;
        img->state = 0;
    }
}

void beginFrame() {
    g_frame++;
    drainDone();
    evict();
}

TexImage *get(const std::string &file) {
    auto it = g_images.find(file);
    if (it != g_images.end())
        return it->second;
    TexImage *img = new TexImage();
    img->file = file;
    g_images[file] = img;
    return img;
}

bool exists(const std::string &file) {
    auto it = g_images.find(file);
    return it != g_images.end() && it->second->known;
}

bool isLoaded(const std::string &file) {
    auto it = g_images.find(file);
    return it != g_images.end() && it->second->state == 2;
}

bool ensure(TexImage *img) {
    img->lastUse = g_frame;
    if (img->state == 2)
        return true;
    if (img->state == -1)
        return false;
    if (img->state == 1) {
        SDL_LockMutex(g_mutex);
        auto q = std::find(g_queue.begin(), g_queue.end(), img);
        if (q != g_queue.end()) {
            g_queue.erase(q);
            SDL_UnlockMutex(g_mutex);
            img->state = 0;
        } else {
            while (g_working == img)
                SDL_CondWait(g_cond, g_mutex);
            SDL_UnlockMutex(g_mutex);
            drainDone();
            if (img->state == 2)
                return true;
            img->state = 0;
        }
    }
    upload(img, decode(img->file));
    return img->state == 2;
}

void preload(const std::string &file) {
    TexImage *img = get(file);
    img->lastUse = g_frame;
    if (img->state != 0)
        return;
    img->state = 1;
    SDL_LockMutex(g_mutex);
    g_queue.push_back(img);
    SDL_CondSignal(g_cond);
    SDL_UnlockMutex(g_mutex);
}

size_t pendingCount() {
    SDL_LockMutex(g_mutex);
    size_t n = g_queue.size() + (g_working ? 1 : 0);
    SDL_UnlockMutex(g_mutex);
    return n;
}

} // namespace texture
