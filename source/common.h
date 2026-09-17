#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Katawa Shoujo virtual screen
constexpr float VW = 800.0f;
constexpr float VH = 600.0f;

struct Color {
    float r = 1, g = 1, b = 1, a = 1;
};
Color hexColor(const char *s);

void logf(const char *fmt, ...);
double nowSeconds();

std::string romfsPath(const std::string &rel);
std::string userPath(const std::string &rel);
bool readFile(const std::string &path, std::vector<uint8_t> &out);
bool writeFile(const std::string &path, const std::vector<uint8_t> &data);
bool fileExists(const std::string &path);
void removeFile(const std::string &path);
void makeDirs(const std::string &path);

uint32_t utf8Decode(const std::string &s, size_t &i); // advances i
std::string utf8Encode(uint32_t cp);
size_t utf8Count(const std::string &s);
bool isCJK(uint32_t cp);
std::string formatTime(int64_t t, const char *fmt);

struct ByteWriter {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void uvar(uint64_t v) {
        while (true) {
            uint8_t x = v & 0x7f;
            v >>= 7;
            if (v) {
                b.push_back(x | 0x80);
            } else {
                b.push_back(x);
                break;
            }
        }
    }
    void svar(int64_t v) { uvar(v >= 0 ? (uint64_t)v << 1 : (((uint64_t)(-(v + 1))) << 1) | 1); }
    void f64(double v) {
        uint64_t x;
        memcpy(&x, &v, 8);
        for (int i = 0; i < 8; i++)
            b.push_back((x >> (8 * i)) & 0xff);
    }
    void str(const std::string &s) {
        uvar(s.size());
        b.insert(b.end(), s.begin(), s.end());
    }
};

struct ByteReader {
    const uint8_t *p;
    size_t n, pos = 0;
    bool ok = true;
    ByteReader(const uint8_t *p_, size_t n_) : p(p_), n(n_) {}
    uint8_t u8() {
        if (pos >= n) {
            ok = false;
            return 0;
        }
        return p[pos++];
    }
    uint64_t uvar() {
        uint64_t v = 0;
        int sh = 0;
        while (pos < n) {
            uint8_t x = p[pos++];
            v |= (uint64_t)(x & 0x7f) << sh;
            if (!(x & 0x80))
                return v;
            sh += 7;
        }
        ok = false;
        return v;
    }
    int64_t svar() {
        uint64_t u = uvar();
        return (u & 1) ? -(int64_t)(u >> 1) - 1 : (int64_t)(u >> 1);
    }
    double f64() {
        if (pos + 8 > n) {
            ok = false;
            return 0;
        }
        uint64_t x = 0;
        for (int i = 0; i < 8; i++)
            x |= (uint64_t)p[pos + i] << (8 * i);
        pos += 8;
        double v;
        memcpy(&v, &x, 8);
        return v;
    }
    std::string str() {
        uint64_t len = uvar();
        if (pos + len > n) {
            ok = false;
            return std::string();
        }
        std::string s((const char *)p + pos, (size_t)len);
        pos += (size_t)len;
        return s;
    }
};
