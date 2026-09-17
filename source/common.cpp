#include "common.h"
#include <SDL.h>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

Color hexColor(const char *s) {
    Color c;
    if (*s == '#')
        s++;
    size_t n = strlen(s);
    auto hx = [](char ch) -> int {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        ch |= 0x20;
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        return 0;
    };
    if (n == 3 || n == 4) {
        c.r = hx(s[0]) * 17 / 255.0f;
        c.g = hx(s[1]) * 17 / 255.0f;
        c.b = hx(s[2]) * 17 / 255.0f;
        c.a = n == 4 ? hx(s[3]) * 17 / 255.0f : 1.0f;
    } else if (n == 6 || n == 8) {
        c.r = (hx(s[0]) * 16 + hx(s[1])) / 255.0f;
        c.g = (hx(s[2]) * 16 + hx(s[3])) / 255.0f;
        c.b = (hx(s[4]) * 16 + hx(s[5])) / 255.0f;
        c.a = n == 8 ? (hx(s[6]) * 16 + hx(s[7])) / 255.0f : 1.0f;
    }
    return c;
}

static FILE *g_log = nullptr;
static SDL_mutex *g_logMutex = nullptr;

void logf(const char *fmt, ...) {
    if (!g_logMutex)
        g_logMutex = SDL_CreateMutex();
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SDL_LockMutex(g_logMutex);
    fprintf(stderr, "%s\n", buf);
    if (!g_log) {
        makeDirs(userPath(""));
        g_log = fopen(userPath("log.txt").c_str(), "w");
    }
    if (g_log) {
        fprintf(g_log, "%s\n", buf);
        fflush(g_log);
    }
    SDL_UnlockMutex(g_logMutex);
}

double nowSeconds() {
    static Uint64 base = SDL_GetPerformanceCounter();
    return (double)(SDL_GetPerformanceCounter() - base) / (double)SDL_GetPerformanceFrequency();
}

std::string romfsPath(const std::string &rel) {
#ifdef __SWITCH__
    return "romfs:/" + rel;
#else
    static std::string base;
    if (base.empty()) {
        const char *env = getenv("KS_ROMFS");
        if (env)
            base = std::string(env) + "/";
        else if (fileExists("romfs/data/game.bin"))
            base = "romfs/";
        else
            base = "../romfs/";
    }
    return base + rel;
#endif
}

std::string userPath(const std::string &rel) {
#ifdef __SWITCH__
    return "sdmc:/switch/KatawaShoujo/" + rel;
#else
    return "userdata/" + rel;
#endif
}

bool readFile(const std::string &path, std::vector<uint8_t> &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(sz > 0 ? (size_t)sz : 0);
    size_t rd = sz > 0 ? fread(out.data(), 1, (size_t)sz, f) : 0;
    fclose(f);
    return rd == out.size();
}

bool writeFile(const std::string &path, const std::vector<uint8_t> &data) {
    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f)
        return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp.c_str());
        return false;
    }
    remove(path.c_str());
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool fileExists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

void removeFile(const std::string &path) { remove(path.c_str()); }

void makeDirs(const std::string &path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if ((path[i] == '/' || i + 1 == path.size()) && cur.size() > 1 && cur.back() != ':' &&
            !(cur.size() >= 2 && cur[cur.size() - 2] == ':')) {
#ifdef _WIN32
            _mkdir(cur.c_str());
#else
            mkdir(cur.c_str(), 0777);
#endif
        }
    }
}

uint32_t utf8Decode(const std::string &s, size_t &i) {
    unsigned char c = s[i];
    uint32_t cp;
    int extra;
    if (c < 0x80) {
        cp = c;
        extra = 0;
    } else if ((c & 0xe0) == 0xc0) {
        cp = c & 0x1f;
        extra = 1;
    } else if ((c & 0xf0) == 0xe0) {
        cp = c & 0x0f;
        extra = 2;
    } else if ((c & 0xf8) == 0xf0) {
        cp = c & 0x07;
        extra = 3;
    } else {
        i++;
        return 0xfffd;
    }
    i++;
    for (int k = 0; k < extra && i < s.size(); k++, i++)
        cp = (cp << 6) | ((unsigned char)s[i] & 0x3f);
    return cp;
}

std::string utf8Encode(uint32_t cp) {
    std::string r;
    if (cp < 0x80)
        r += (char)cp;
    else if (cp < 0x800) {
        r += (char)(0xc0 | (cp >> 6));
        r += (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
        r += (char)(0xe0 | (cp >> 12));
        r += (char)(0x80 | ((cp >> 6) & 0x3f));
        r += (char)(0x80 | (cp & 0x3f));
    } else {
        r += (char)(0xf0 | (cp >> 18));
        r += (char)(0x80 | ((cp >> 12) & 0x3f));
        r += (char)(0x80 | ((cp >> 6) & 0x3f));
        r += (char)(0x80 | (cp & 0x3f));
    }
    return r;
}

size_t utf8Count(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xc0) != 0x80)
            n++;
    return n;
}

bool isCJK(uint32_t cp) {
    return (cp >= 0x2e80 && cp <= 0x9fff) || (cp >= 0xac00 && cp <= 0xd7af) || (cp >= 0xf900 && cp <= 0xfaff) ||
           (cp >= 0xff00 && cp <= 0xffef) || (cp >= 0x3000 && cp <= 0x303f);
}

std::string formatTime(int64_t t, const char *fmt) {
    time_t tt = (time_t)t;
    struct tm *lt = localtime(&tt);
    char buf[128];
    if (!lt || !strftime(buf, sizeof buf, fmt, lt))
        return "";
    return buf;
}
