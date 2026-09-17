#pragma once
#include "common.h"
#include <string>

struct TexImage {
    std::string file; // game-relative path ("bgs/school_gate.jpg")
    int w = 0, h = 0; // image size
    unsigned tex = 0;
    size_t bytes = 0;
    uint64_t lastUse = 0;
    int state = 0; // 0 unloaded, 1 queued, 2 ready, -1 failed
    bool known = false;
};

namespace texture {
bool init();
void shutdown();
void beginFrame(); // uploads decoded images, evicts old ones
uint64_t frameNo();
TexImage *get(const std::string &file); // never null
bool exists(const std::string &file);   // known in the data files
bool ensure(TexImage *img);             // synchronous load
void preload(const std::string &file);  // background decode
bool isLoaded(const std::string &file);
size_t pendingCount();
} // namespace texture
