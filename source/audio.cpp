#include "audio.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <memory>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_HEADER_ONLY
#include "ext/stb_vorbis.c"

namespace audio {

static const int RATE = 48000;

struct Track {
    stb_vorbis *vorbis = nullptr;
    std::string file;
    int channels = 2;
    double step = 1.0; // source samples per output sample
    double pos = 0;    // fractional read position within buf
    std::vector<float> buf; // interleaved decoded frames
    size_t bufFrames = 0;
    bool eof = false;
    bool loop = false;
    // fades (in output frames)
    long fadeInTotal = 0, fadeInDone = 0;
    long fadeOutTotal = 0, fadeOutDone = 0;
    bool fadingOut = false;
    ~Track() {
        if (vorbis)
            stb_vorbis_close(vorbis);
    }
    bool open(const std::string &f) {
        file = f;
        int err = 0;
        vorbis = stb_vorbis_open_filename(romfsPath("game/" + f).c_str(), &err, nullptr);
        if (!vorbis) {
            logf("audio: cannot open %s (%d)", f.c_str(), err);
            return false;
        }
        stb_vorbis_info info = stb_vorbis_get_info(vorbis);
        channels = info.channels;
        step = (double)info.sample_rate / RATE;
        return true;
    }
    bool refill() {
        float **out = nullptr;
        int n = stb_vorbis_get_frame_float(vorbis, nullptr, &out);
        if (n <= 0)
            return false;
        buf.resize((size_t)n * 2);
        for (int i = 0; i < n; i++) {
            float l = out[0][i];
            float r = channels > 1 ? out[1][i] : l;
            buf[i * 2] = l;
            buf[i * 2 + 1] = r;
        }
        bufFrames = (size_t)n;
        return true;
    }
    // mixes up to n frames into dst (adds); returns frames produced
    int mix(float *dst, int n, float gain) {
        int produced = 0;
        while (produced < n) {
            if (pos >= bufFrames) {
                pos -= bufFrames;
                bufFrames = 0;
                if (!refill()) {
                    eof = true;
                    break;
                }
                continue;
            }
            size_t i0 = (size_t)pos;
            double frac = pos - i0;
            float l, r;
            if (i0 + 1 < bufFrames) {
                l = (float)(buf[i0 * 2] * (1 - frac) + buf[(i0 + 1) * 2] * frac);
                r = (float)(buf[i0 * 2 + 1] * (1 - frac) + buf[(i0 + 1) * 2 + 1] * frac);
            } else {
                l = buf[i0 * 2];
                r = buf[i0 * 2 + 1];
            }
            float g = gain;
            if (fadeInTotal > 0 && fadeInDone < fadeInTotal) {
                g *= (float)fadeInDone / fadeInTotal;
                fadeInDone++;
            }
            if (fadingOut) {
                if (fadeOutDone >= fadeOutTotal) {
                    eof = true;
                    break;
                }
                g *= 1.0f - (float)fadeOutDone / fadeOutTotal;
                fadeOutDone++;
            }
            dst[produced * 2] += l * g;
            dst[produced * 2 + 1] += r * g;
            produced++;
            pos += step;
        }
        return produced;
    }
};

struct QueueItem {
    std::string file;
    bool loop;
    double fadein;
};

struct Channel {
    std::string name, mixer;
    bool defaultLoop = false, tight = false;
    std::unique_ptr<Track> current;
    std::vector<std::unique_ptr<Track>> fading;
    std::deque<QueueItem> queue;
    std::vector<std::string> lastFiles;
    bool lastLoop = false;
    double volume = 1.0, volTarget = 1.0, volStart = 1.0;
    long volRampTotal = 0, volRampDone = 0;
    bool forceStop = false;
};

static SDL_AudioDeviceID g_dev = 0;
static std::map<std::string, Channel> g_channels;
static std::map<std::string, double> g_mixers = {{"music", 1.0}, {"sfx", 1.0}, {"voice", 1.0}};
static std::function<int(int16_t *, int)> g_movie;
static std::vector<float> g_mixbuf;
static bool g_paused = false;

static void lock() {
    if (g_dev)
        SDL_LockAudioDevice(g_dev);
}
static void unlock() {
    if (g_dev)
        SDL_UnlockAudioDevice(g_dev);
}

static void startNext(Channel &c) {
    while (!c.queue.empty()) {
        QueueItem q = c.queue.front();
        c.queue.pop_front();
        auto t = std::make_unique<Track>();
        if (!t->open(q.file))
            continue;
        t->loop = q.loop;
        if (q.fadein > 0)
            t->fadeInTotal = (long)(q.fadein * RATE);
        if (q.loop && c.queue.empty()) {
            // loop: re-queue the last files when they finish
            for (auto &f : c.lastFiles)
                c.queue.push_back({f, true, 0});
        }
        c.current = std::move(t);
        return;
    }
}

static void callback(void *, Uint8 *stream, int len) {
    int frames = len / 4;
    if ((int)g_mixbuf.size() < frames * 2)
        g_mixbuf.resize(frames * 2);
    int16_t *out = (int16_t *)stream;
    std::fill(g_mixbuf.begin(), g_mixbuf.begin() + frames * 2, 0.0f);
    if (!g_paused) {
        std::vector<float> tmp;
        for (auto &kv : g_channels) {
            Channel &c = kv.second;
            if (c.forceStop)
                continue;
            float mixerVol = (float)g_mixers[c.mixer];
            tmp.assign(frames * 2, 0.0f);
            int done = 0;
            // fading-out tracks (tight channels)
            for (auto &f : c.fading)
                f->mix(tmp.data(), frames, 1.0f);
            c.fading.erase(std::remove_if(c.fading.begin(), c.fading.end(),
                                          [](const std::unique_ptr<Track> &t) { return t->eof; }),
                           c.fading.end());
            int guard = 0;
            while (done < frames && guard++ < 8) {
                if (!c.current) {
                    startNext(c);
                    if (!c.current)
                        break;
                }
                int n = c.current->mix(tmp.data() + done * 2, frames - done, 1.0f);
                done += n;
                if (c.current->eof) {
                    bool wasFade = c.current->fadingOut;
                    c.current.reset();
                    if (wasFade && c.queue.empty())
                        break;
                }
            }
            // channel volume ramp + mixer volume
            for (int i = 0; i < frames; i++) {
                if (c.volRampDone < c.volRampTotal) {
                    c.volRampDone++;
                    c.volume = c.volStart + (c.volTarget - c.volStart) * ((double)c.volRampDone / c.volRampTotal);
                }
                float g = (float)c.volume * mixerVol;
                g_mixbuf[i * 2] += tmp[i * 2] * g;
                g_mixbuf[i * 2 + 1] += tmp[i * 2 + 1] * g;
            }
        }
        if (g_movie) {
            std::vector<int16_t> mv(frames * 2, 0);
            int got = g_movie(mv.data(), frames);
            float g = (float)g_mixers["music"];
            for (int i = 0; i < got * 2; i++)
                g_mixbuf[i] += mv[i] / 32768.0f * g;
        }
    }
    for (int i = 0; i < frames * 2; i++) {
        float s = std::max(-1.0f, std::min(1.0f, g_mixbuf[i]));
        out[i] = (int16_t)(s * 32767.0f);
    }
}

bool init() {
    SDL_AudioSpec want{}, have{};
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 2048;
    want.callback = callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        logf("SDL_OpenAudioDevice: %s", SDL_GetError());
        return false;
    }
    registerChannel("music", "music", true, false);
    registerChannel("sound", "sfx", false, false);
    registerChannel("voice", "voice", false, false);
    registerChannel("movie", "music", false, false);
    // registered by the game's init code (ui_code.rpy)
    registerChannel("ambient", "sfx", true, true);
    registerChannel("ambient2", "sfx", true, true);
    SDL_PauseAudioDevice(g_dev, 0);
    return true;
}

void shutdown() {
    if (g_dev) {
        SDL_CloseAudioDevice(g_dev);
        g_dev = 0;
    }
    g_channels.clear();
}

void registerChannel(const std::string &name, const std::string &mixer, bool defaultLoop, bool tight) {
    lock();
    Channel &c = g_channels[name];
    c.name = name;
    c.mixer = mixer;
    c.defaultLoop = defaultLoop;
    c.tight = tight;
    unlock();
}

bool hasChannel(const std::string &name) { return g_channels.count(name) != 0; }

static void fadeoutLocked(Channel &c, double fadeout) {
    if (!c.current)
        return;
    if (fadeout <= 0) {
        c.current.reset();
        return;
    }
    if (!c.current->fadingOut) {
        c.current->fadingOut = true;
        c.current->fadeOutTotal = std::max(1L, (long)(fadeout * RATE));
        c.current->fadeOutDone = 0;
    }
    if (c.tight)
        c.fading.push_back(std::move(c.current));
}

void play(const std::string &channel, const std::vector<std::string> &files, double fadein, double fadeout, int loop,
          bool ifChanged) {
    if (files.empty())
        return;
    lock();
    auto it = g_channels.find(channel);
    if (it == g_channels.end()) {
        unlock();
        logf("audio: unknown channel %s", channel.c_str());
        return;
    }
    Channel &c = it->second;
    bool lp = loop < 0 ? c.defaultLoop : loop != 0;
    c.queue.clear();
    bool keep = ifChanged && c.current && !c.current->fadingOut &&
                std::find(files.begin(), files.end(), c.current->file) != files.end();
    if (keep) {
        fadein = 0;
        c.current->loop = lp;
    } else {
        fadeoutLocked(c, fadeout);
    }
    c.lastFiles = files;
    c.lastLoop = lp;
    if (keep) {
        if (lp)
            for (auto &f : files)
                c.queue.push_back({f, true, 0});
    } else {
        for (size_t i = 0; i < files.size(); i++)
            c.queue.push_back({files[i], lp, i == 0 ? fadein : 0});
    }
    if (!lp) {
        c.lastFiles.clear();
    }
    unlock();
}

void queue(const std::string &channel, const std::vector<std::string> &files, int loop, bool clearQueue) {
    lock();
    auto it = g_channels.find(channel);
    if (it != g_channels.end()) {
        Channel &c = it->second;
        bool lp = loop < 0 ? c.defaultLoop : loop != 0;
        if (clearQueue)
            c.queue.clear();
        for (auto &f : files)
            c.queue.push_back({f, lp, 0});
        c.lastFiles = lp ? files : std::vector<std::string>();
        c.lastLoop = lp;
    }
    unlock();
}

void stop(const std::string &channel, double fadeout) {
    lock();
    auto it = g_channels.find(channel);
    if (it != g_channels.end()) {
        Channel &c = it->second;
        c.queue.clear();
        fadeoutLocked(c, fadeout);
        c.lastFiles.clear();
        c.lastLoop = false;
    }
    unlock();
}

void stopAll() {
    lock();
    for (auto &kv : g_channels) {
        kv.second.queue.clear();
        kv.second.current.reset();
        kv.second.fading.clear();
        kv.second.lastFiles.clear();
    }
    unlock();
}

void setChannelVolume(const std::string &channel, double volume, double delay) {
    lock();
    auto it = g_channels.find(channel);
    if (it != g_channels.end()) {
        Channel &c = it->second;
        c.volStart = c.volume;
        c.volTarget = volume;
        c.volRampTotal = std::max(1L, (long)(delay * RATE));
        c.volRampDone = 0;
        if (delay <= 0) {
            c.volume = volume;
            c.volRampDone = c.volRampTotal;
        }
    }
    unlock();
}

double channelVolume(const std::string &channel) {
    auto it = g_channels.find(channel);
    return it == g_channels.end() ? 1.0 : it->second.volTarget;
}

void setMixerVolume(const std::string &mixer, double volume) {
    lock();
    g_mixers[mixer] = volume;
    unlock();
}

std::string playing(const std::string &channel) {
    lock();
    std::string r;
    auto it = g_channels.find(channel);
    if (it != g_channels.end()) {
        Channel &c = it->second;
        if (c.current && !c.current->fadingOut)
            r = c.current->file;
        else if (!c.queue.empty())
            r = c.queue.front().file;
    }
    unlock();
    return r;
}

std::vector<std::string> lastQueued(const std::string &channel) {
    lock();
    std::vector<std::string> r;
    auto it = g_channels.find(channel);
    if (it != g_channels.end())
        r = it->second.lastFiles;
    unlock();
    return r;
}

void setForceStop(const std::string &channel, bool stop) {
    lock();
    auto it = g_channels.find(channel);
    if (it != g_channels.end())
        it->second.forceStop = stop;
    unlock();
}

void pauseAll(bool paused) {
    lock();
    g_paused = paused;
    unlock();
}

void setMovieSource(std::function<int(int16_t *, int)> src) {
    lock();
    g_movie = std::move(src);
    unlock();
}

std::vector<std::string> channelNames() {
    std::vector<std::string> r;
    for (auto &kv : g_channels)
        r.push_back(kv.first);
    return r;
}

} // namespace audio
