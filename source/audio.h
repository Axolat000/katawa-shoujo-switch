#pragma once
#include "common.h"
#include <functional>
#include <string>
#include <vector>

namespace audio {
bool init();
void shutdown();
void registerChannel(const std::string &name, const std::string &mixer, bool defaultLoop, bool tight);
bool hasChannel(const std::string &name);
// loop: -1 channel default, 0 no, 1 yes
void play(const std::string &channel, const std::vector<std::string> &files, double fadein, double fadeout, int loop,
          bool ifChanged);
inline void play(const std::string &channel, const std::string &file, double fadein, double fadeout, int loop,
                 bool ifChanged = false) {
    play(channel, std::vector<std::string>{file}, fadein, fadeout, loop, ifChanged);
}
void queue(const std::string &channel, const std::vector<std::string> &files, int loop, bool clearQueue);
void stop(const std::string &channel, double fadeout);
void stopAll();
void setChannelVolume(const std::string &channel, double volume, double delay);
double channelVolume(const std::string &channel);
void setMixerVolume(const std::string &mixer, double volume); // 0..1 (already shaped)
std::string playing(const std::string &channel);
std::vector<std::string> lastQueued(const std::string &channel); // looping files (for saves)
void setForceStop(const std::string &channel, bool stop);
void pauseAll(bool paused);
// External PCM source for the movie channel (stereo 48k int16). Returns frames written.
void setMovieSource(std::function<int(int16_t *, int)> src);
std::vector<std::string> channelNames();
} // namespace audio
