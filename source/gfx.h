#pragma once
#include "common.h"

// 2D affine matrix: x' = a*x + c*y + tx ; y' = b*x + d*y + ty
struct Mat {
    float a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};
Mat matMul(const Mat &m, const Mat &n); // apply n first, then m
Mat matTranslate(float x, float y);
Mat matScale(float sx, float sy);
Mat matLinear(float xdx, float xdy, float ydx, float ydy); // x' = xdx*x + xdy*y
Mat matRect(float x, float y, float w, float h);           // unit quad -> rect
void matApply(const Mat &m, float x, float y, float &ox, float &oy);
bool matAxisAligned(const Mat &m);

// 4x5 color matrix applied to straight RGBA in [0,1] (Ren'Py im.matrix, offset column normalized)
struct CMat {
    float m[20];
    bool identity = true;
    CMat();
    static CMat fromList(const double *v, int n); // 20 or 25 values
    CMat operator*(const CMat &o) const;          // this applied after o
    static CMat alpha(float a);
};

struct RenderTarget {
    unsigned fbo = 0, tex = 0;
    int w = 0, h = 0; // pixels
};

namespace gfx {
bool init();
void setOutputSize(int w, int h); // window pixels
int outW();
int outH();

// Rectangle of the window used by the 800x600 game screen (letterboxed).
void screenRect(float &x, float &y, float &w, float &h);
float pixelScale(); // physical pixels per virtual pixel

// Begins drawing into the window's game area in virtual coordinates.
void bindScreen();
// Begins drawing into a target covering a virtual area of vw x vh.
void bindTarget(RenderTarget *rt, float vw, float vh);
void clear(float r, float g, float b, float a);

RenderTarget *acquireTarget(int w, int h); // pooled
void releaseTarget(RenderTarget *rt);

void drawTexture(unsigned tex, const Mat &m, float u0, float v0, float u1, float v1, float alpha, const CMat *cm = nullptr);
void drawSolid(const Mat &m, float r, float g, float b, float a);
// Target textures are stored upside down: pass v0=1, v1=0.
void drawBlend(unsigned texA, unsigned texB, const Mat &m, float alpha, float t); // mix(A, B, t)
// ImageDissolve: control texture (red channel), ramp in 256 steps.
void drawImageBlend(unsigned texA, unsigned texB, unsigned texCtrl, const Mat &m, float alpha, float t, int ramp);

// Clip in virtual coordinates of the current target (axis aligned); nested via push/pop.
void pushClip(float x0, float y0, float x1, float y1);
void popClip();

unsigned uploadTexture(const uint8_t *px, int w, int h, bool rgb, bool mipmap);
void deleteTexture(unsigned tex);
bool readPixels(RenderTarget *rt, std::vector<uint8_t> &rgba); // rows top to bottom
} // namespace gfx
