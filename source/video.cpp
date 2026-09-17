#include "audio.h"
#include "game.h"
#include "input.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <mutex>

#define PL_MPEG_IMPLEMENTATION
#include "ext/pl_mpeg.h"

void updateTextureRGBA(unsigned tex, const uint8_t *px, int w, int h);

namespace {

struct Movie {
    plm_t *plm = nullptr;
    unsigned tex = 0;
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool frameReady = false;
    bool ended = false;
    double duration = 0;
    // audio ring buffer (stereo float at the movie rate)
    std::mutex mtx;
    std::vector<float> ring;
    size_t rHead = 0, rCount = 0;
    int rate = 44100;
    double resamplePos = 0;

    bool open(const std::string &file) {
        plm = plm_create_with_filename(romfsPath("game/" + file).c_str());
        if (!plm) {
            logf("movie: cannot open %s", file.c_str());
            return false;
        }
        if (!plm_probe(plm, 5000 * 1024))
            logf("movie: probe failed %s", file.c_str());
        w = plm_get_width(plm);
        h = plm_get_height(plm);
        rate = plm_get_samplerate(plm) > 0 ? plm_get_samplerate(plm) : 44100;
        duration = plm_get_duration(plm);
        plm_set_loop(plm, 0);
        plm_set_audio_enabled(plm, plm_get_num_audio_streams(plm) > 0);
        plm_set_audio_lead_time(plm, 0.15);
        plm_set_video_decode_callback(plm, onVideo, this);
        plm_set_audio_decode_callback(plm, onAudio, this);
        // plm_frame_to_rgba only writes R, G and B: keep the alpha bytes opaque
        rgba.assign((size_t)w * h * 4, 255);
        ring.assign((size_t)rate * 2 * 2, 0.0f); // 2 seconds
        return w > 0 && h > 0;
    }
    ~Movie() {
        if (plm)
            plm_destroy(plm);
        if (tex)
            gfx::deleteTexture(tex);
    }
    static void onVideo(plm_t *, plm_frame_t *frame, void *user) {
        Movie *m = (Movie *)user;
        plm_frame_to_rgba(frame, m->rgba.data(), m->w * 4);
        m->frameReady = true;
    }
    static void onAudio(plm_t *, plm_samples_t *samples, void *user) {
        Movie *m = (Movie *)user;
        std::lock_guard<std::mutex> lock(m->mtx);
        size_t cap = m->ring.size() / 2;
        for (unsigned i = 0; i < samples->count; i++) {
            if (m->rCount >= cap)
                break;
            size_t idx = (m->rHead + m->rCount) % cap;
            m->ring[idx * 2] = samples->interleaved[i * 2];
            m->ring[idx * 2 + 1] = samples->interleaved[i * 2 + 1];
            m->rCount++;
        }
    }
    // SDL audio thread
    int pull(int16_t *out, int frames) {
        std::lock_guard<std::mutex> lock(mtx);
        size_t cap = ring.size() / 2;
        double step = (double)rate / 48000.0;
        int produced = 0;
        while (produced < frames && rCount > 1) {
            float l = ring[rHead * 2], r = ring[rHead * 2 + 1];
            out[produced * 2] = (int16_t)(std::max(-1.0f, std::min(1.0f, l)) * 32767);
            out[produced * 2 + 1] = (int16_t)(std::max(-1.0f, std::min(1.0f, r)) * 32767);
            produced++;
            resamplePos += step;
            while (resamplePos >= 1.0 && rCount > 0) {
                resamplePos -= 1.0;
                rHead = (rHead + 1) % cap;
                rCount--;
            }
        }
        return produced;
    }
    void decode(double dt) {
        if (!plm || ended)
            return;
        plm_decode(plm, std::min(dt, 0.1));
        if (plm_has_ended(plm))
            ended = true;
        if (frameReady) {
            frameReady = false;
            if (!tex) {
                tex = gfx::uploadTexture(rgba.data(), w, h, false, false);
            } else {
                updateTextureRGBA(tex, rgba.data(), w, h);
            }
        }
    }
};

std::shared_ptr<Movie> g_bgMovie; // "play movie" channel

std::string movieFile(std::string f) {
    if (f.compare(0, 6, "video/") != 0)
        f = "video/" + f;
    if (f.size() > 4 && f.substr(f.size() - 4) == ".mkv")
        f = f.substr(0, f.size() - 4) + ".mpg";
    return f;
}

} // namespace

void updateTextureRGBA(unsigned tex, const uint8_t *px, int w, int h);

bool playMovie(const std::string &fileIn, bool skippable) {
    Game &g = G;
    std::string key = fileIn.compare(0, 6, "video/") == 0 ? fileIn : "video/" + fileIn;
    Value seen = g.persistentGet("seen_videos");
    bool seenBefore = false;
    if (ListObj *l = seen.list())
        for (auto &x : l->v)
            if (x.isStr() && x.s() == key)
                seenBefore = true;
    if (!seenBefore)
        g.configV.inst()->attrs["skipping"] = Value();
    auto movie = std::make_shared<Movie>();
    if (!movie->open(movieFile(fileIn)))
        return false;
    audio::setForceStop("music", true);
    std::weak_ptr<Movie> weak = movie;
    audio::setMovieSource([weak](int16_t *out, int frames) {
        auto m = weak.lock();
        return m ? m->pull(out, frames) : 0;
    });
    double last = nowSeconds();
    bool aborted = false;
    Interaction in;
    in.type = IType::Movie;
    in.suppressWindow = true;
    in.update = [&]() {
        double t = nowSeconds();
        movie->decode(t - last);
        last = t;
        const Input &inp = g_input;
        bool abort = inp.cancel || inp.menu;
        bool dismiss = inp.accept || inp.pointerReleased;
        if (abort || (dismiss && seenBefore && skippable)) {
            aborted = true;
            return true;
        }
        return movie->ended;
    };
    in.draw = [&]() {
        gfx::drawSolid(matRect(0, 0, VW, VH), 0, 0, 0, 1);
        if (movie->tex) {
            float s = std::min(VW / movie->w, VH / movie->h);
            float dw = movie->w * s, dh = movie->h * s;
            gfx::drawTexture(movie->tex, matRect((VW - dw) / 2, (VH - dh) / 2, dw, dh), 0, 0, 1, 1, 1.0f);
        }
    };
    try {
        g.interact(in);
    } catch (...) {
        audio::setMovieSource(nullptr);
        audio::setForceStop("music", false);
        throw;
    }
    audio::setMovieSource(nullptr);
    audio::setForceStop("music", false);
    if (!seenBefore) {
        if (!seen.list()) {
            seen = mkList();
            g.persistentSet("seen_videos", seen);
        }
        seen.list()->v.push_back(Value::str(key));
        g.savePersistent();
    }
    return aborted;
}

// ------------------------------------------------------------------ background movie (credits)

void startBackgroundMovie(const std::string &file) {
    auto movie = std::make_shared<Movie>();
    if (!movie->open(movieFile(file)))
        return;
    g_bgMovie = movie;
    std::weak_ptr<Movie> weak = movie;
    audio::setMovieSource([weak](int16_t *out, int frames) {
        auto m = weak.lock();
        return m ? m->pull(out, frames) : 0;
    });
}

void stopBackgroundMovie() {
    audio::setMovieSource(nullptr);
    g_bgMovie.reset();
}

struct MovieDisp : Displayable {
    float w = 400, h = 300;
    double lastT = -1;
    RenderP render(float, float, double, double) override {
        auto rv = Render::make(w, h);
        auto m = g_bgMovie;
        if (!m)
            return rv;
        double t = nowSeconds();
        if (lastT >= 0)
            m->decode(t - lastT);
        lastT = t;
        rv->op = Render::CUSTOM;
        float ww = w, hh = h;
        rv->draw = [m, ww, hh](const Mat &mat, float alpha) {
            if (m->tex)
                gfx::drawTexture(m->tex, matMul(mat, matRect(0, 0, ww, hh)), 0, 0, 1, 1, alpha);
        };
        return rv;
    }
};

DispP makeMovieDisp(const CallArgs &a) {
    auto d = std::make_shared<MovieDisp>();
    d->kind = "Movie";
    for (auto &k : a.kw) {
        if (k.first == "size") {
            ListObj *l = k.second.list();
            if (l && l->v.size() == 2) {
                d->w = (float)l->v[0].num();
                d->h = (float)l->v[1].num();
            }
        } else {
            d->style.apply(k.first, k.second);
        }
    }
    return d;
}
