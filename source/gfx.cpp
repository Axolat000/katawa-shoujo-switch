#include "gfx.h"
#include <GLES2/gl2.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

Mat matMul(const Mat &m, const Mat &n) {
    Mat r;
    r.a = m.a * n.a + m.c * n.b;
    r.b = m.b * n.a + m.d * n.b;
    r.c = m.a * n.c + m.c * n.d;
    r.d = m.b * n.c + m.d * n.d;
    r.tx = m.a * n.tx + m.c * n.ty + m.tx;
    r.ty = m.b * n.tx + m.d * n.ty + m.ty;
    return r;
}
Mat matTranslate(float x, float y) {
    Mat m;
    m.tx = x;
    m.ty = y;
    return m;
}
Mat matScale(float sx, float sy) {
    Mat m;
    m.a = sx;
    m.d = sy;
    return m;
}
Mat matLinear(float xdx, float xdy, float ydx, float ydy) {
    Mat m;
    m.a = xdx;
    m.c = xdy;
    m.b = ydx;
    m.d = ydy;
    return m;
}
Mat matRect(float x, float y, float w, float h) {
    Mat m;
    m.a = w;
    m.d = h;
    m.tx = x;
    m.ty = y;
    return m;
}
void matApply(const Mat &m, float x, float y, float &ox, float &oy) {
    ox = m.a * x + m.c * y + m.tx;
    oy = m.b * x + m.d * y + m.ty;
}
bool matAxisAligned(const Mat &m) { return std::fabs(m.b) < 1e-6f && std::fabs(m.c) < 1e-6f; }

CMat::CMat() {
    memset(m, 0, sizeof m);
    m[0] = m[6] = m[12] = m[18] = 1;
}
CMat CMat::fromList(const double *v, int n) {
    CMat c;
    if (n < 20)
        return c;
    for (int i = 0; i < 20; i++)
        c.m[i] = (float)v[i];
    c.identity = false;
    return c;
}
CMat CMat::operator*(const CMat &o) const {
    if (identity)
        return o;
    if (o.identity)
        return *this;
    // 5x5 multiply with implicit last row (0 0 0 0 1)
    CMat r;
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 5; col++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += m[row * 5 + k] * o.m[k * 5 + col];
            if (col == 4)
                s += m[row * 5 + 4];
            r.m[row * 5 + col] = s;
        }
    }
    r.identity = false;
    return r;
}
CMat CMat::alpha(float a) {
    CMat c;
    c.m[18] = a;
    c.identity = a == 1.0f;
    return c;
}

namespace gfx {

static int g_outW = 1280, g_outH = 720;
static float g_vw = VW, g_vh = VH;
static int g_pixW = 1280, g_pixH = 720; // current target pixel size
static int g_vpX = 0, g_vpY = 0;
static bool g_screen = true;
static GLuint g_vbo = 0;

struct Prog {
    GLuint id = 0;
    GLint m1, m2, proj, uv, alpha, cm, co, usecm, color, tex0, tex1, tex2, t, offset, mult;
};
static Prog g_tex, g_solid, g_blend, g_image;
static GLuint g_cur = 0;

static const char *VS = R"(
attribute vec2 a_pos;
uniform vec4 u_m1;
uniform vec2 u_m2;
uniform vec2 u_proj;
uniform vec4 u_uv;
varying vec2 v_uv;
void main() {
    vec2 p = vec2(u_m1.x * a_pos.x + u_m1.z * a_pos.y + u_m2.x, u_m1.y * a_pos.x + u_m1.w * a_pos.y + u_m2.y);
    gl_Position = vec4(p.x * u_proj.x - 1.0, 1.0 - p.y * u_proj.y, 0.0, 1.0);
    v_uv = u_uv.xy + a_pos * u_uv.zw;
}
)";

static const char *FS_TEX = R"(
precision mediump float;
uniform sampler2D u_tex0;
uniform float u_alpha;
uniform mat4 u_cm;
uniform vec4 u_co;
uniform float u_usecm;
varying vec2 v_uv;
void main() {
    vec4 c = texture2D(u_tex0, v_uv);
    if (u_usecm > 0.5) c = clamp(u_cm * c + u_co, 0.0, 1.0);
    c.a *= u_alpha;
    gl_FragColor = c;
}
)";

static const char *FS_SOLID = R"(
precision mediump float;
uniform vec4 u_color;
void main() { gl_FragColor = u_color; }
)";

static const char *FS_BLEND = R"(
precision mediump float;
uniform sampler2D u_tex0;
uniform sampler2D u_tex1;
uniform float u_alpha;
uniform float u_t;
varying vec2 v_uv;
void main() {
    vec4 c = mix(texture2D(u_tex0, v_uv), texture2D(u_tex1, v_uv), u_t);
    c.a *= u_alpha;
    gl_FragColor = c;
}
)";

static const char *FS_IMAGE = R"(
precision highp float;
uniform sampler2D u_tex0;
uniform sampler2D u_tex1;
uniform sampler2D u_tex2;
uniform float u_alpha;
uniform float u_offset;
uniform float u_mult;
varying vec2 v_uv;
void main() {
    float a = clamp((texture2D(u_tex2, v_uv).a + u_offset) * u_mult, 0.0, 1.0);
    vec4 c = mix(texture2D(u_tex0, v_uv), texture2D(u_tex1, v_uv), a);
    c.a *= u_alpha;
    gl_FragColor = c;
}
)";

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        logf("shader error: %s", log);
    }
    return s;
}

static Prog link(const char *fs) {
    Prog p;
    p.id = glCreateProgram();
    glAttachShader(p.id, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(p.id, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p.id, 0, "a_pos");
    glLinkProgram(p.id);
    GLint ok;
    glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p.id, sizeof log, nullptr, log);
        logf("link error: %s", log);
    }
    auto U = [&](const char *n) { return glGetUniformLocation(p.id, n); };
    p.m1 = U("u_m1");
    p.m2 = U("u_m2");
    p.proj = U("u_proj");
    p.uv = U("u_uv");
    p.alpha = U("u_alpha");
    p.cm = U("u_cm");
    p.co = U("u_co");
    p.usecm = U("u_usecm");
    p.color = U("u_color");
    p.tex0 = U("u_tex0");
    p.tex1 = U("u_tex1");
    p.tex2 = U("u_tex2");
    p.t = U("u_t");
    p.offset = U("u_offset");
    p.mult = U("u_mult");
    glUseProgram(p.id);
    if (p.tex0 >= 0)
        glUniform1i(p.tex0, 0);
    if (p.tex1 >= 0)
        glUniform1i(p.tex1, 1);
    if (p.tex2 >= 0)
        glUniform1i(p.tex2, 2);
    return p;
}

bool init() {
    g_tex = link(FS_TEX);
    g_solid = link(FS_SOLID);
    g_blend = link(FS_BLEND);
    g_image = link(FS_IMAGE);
    static const GLfloat quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    logf("GL: %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    return true;
}

void setOutputSize(int w, int h) {
    g_outW = w;
    g_outH = h;
}
int outW() { return g_outW; }
int outH() { return g_outH; }

void screenRect(float &x, float &y, float &w, float &h) {
    float s = std::min(g_outW / VW, g_outH / VH);
    w = std::floor(VW * s);
    h = std::floor(VH * s);
    x = std::floor((g_outW - w) / 2);
    y = std::floor((g_outH - h) / 2);
}

float pixelScale() {
    float x, y, w, h;
    screenRect(x, y, w, h);
    return w / VW;
}

// ---- clipping stack (in pixels of the current target, GL bottom-left origin)
struct Clip {
    int x0, y0, x1, y1;
};
static std::vector<Clip> g_clips;

static void applyClip() {
    if (g_clips.empty()) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    const Clip &c = g_clips.back();
    glEnable(GL_SCISSOR_TEST);
    glScissor(c.x0, c.y0, std::max(0, c.x1 - c.x0), std::max(0, c.y1 - c.y0));
}

void pushClip(float x0, float y0, float x1, float y1) {
    float sx = g_pixW / g_vw, sy = g_pixH / g_vh;
    Clip c;
    c.x0 = g_vpX + (int)std::floor(x0 * sx);
    c.x1 = g_vpX + (int)std::ceil(x1 * sx);
    // virtual y grows downwards; GL scissor origin is bottom-left
    int top = (int)std::floor(y0 * sy), bottom = (int)std::ceil(y1 * sy);
    if (g_screen) {
        c.y0 = g_vpY + (g_pixH - bottom);
        c.y1 = g_vpY + (g_pixH - top);
    } else {
        // targets are drawn with the same projection (content upside down in texture space)
        c.y0 = g_pixH - bottom;
        c.y1 = g_pixH - top;
    }
    if (!g_clips.empty()) {
        const Clip &p = g_clips.back();
        c.x0 = std::max(c.x0, p.x0);
        c.y0 = std::max(c.y0, p.y0);
        c.x1 = std::min(c.x1, p.x1);
        c.y1 = std::min(c.y1, p.y1);
    }
    g_clips.push_back(c);
    applyClip();
}

void popClip() {
    if (!g_clips.empty())
        g_clips.pop_back();
    applyClip();
}

void bindScreen() {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    float x, y, w, h;
    screenRect(x, y, w, h);
    g_vpX = (int)x;
    g_vpY = g_outH - (int)(y + h);
    g_pixW = (int)w;
    g_pixH = (int)h;
    glViewport(g_vpX, g_vpY, g_pixW, g_pixH);
    g_vw = VW;
    g_vh = VH;
    g_screen = true;
    g_clips.clear();
    applyClip();
}

void bindTarget(RenderTarget *rt, float vw, float vh) {
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    g_vpX = g_vpY = 0;
    g_pixW = rt->w;
    g_pixH = rt->h;
    glViewport(0, 0, rt->w, rt->h);
    g_vw = vw;
    g_vh = vh;
    g_screen = false;
    g_clips.clear();
    applyClip();
}

void clear(float r, float g, float b, float a) {
    bool sc = glIsEnabled(GL_SCISSOR_TEST);
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
    if (sc)
        glEnable(GL_SCISSOR_TEST);
}

static std::multimap<uint64_t, RenderTarget *> g_pool;

RenderTarget *acquireTarget(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    uint64_t key = ((uint64_t)w << 32) | (uint32_t)h;
    auto it = g_pool.find(key);
    if (it != g_pool.end()) {
        RenderTarget *rt = it->second;
        g_pool.erase(it);
        return rt;
    }
    RenderTarget *rt = new RenderTarget();
    rt->w = w;
    rt->h = h;
    glGenTextures(1, &rt->tex);
    glBindTexture(GL_TEXTURE_2D, rt->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &rt->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        logf("framebuffer incomplete %dx%d", w, h);
    return rt;
}

void releaseTarget(RenderTarget *rt) {
    if (!rt)
        return;
    if (g_pool.size() > 24) {
        glDeleteFramebuffers(1, &rt->fbo);
        glDeleteTextures(1, &rt->tex);
        delete rt;
        return;
    }
    g_pool.emplace(((uint64_t)rt->w << 32) | (uint32_t)rt->h, rt);
}

static void use(const Prog &p, const Mat &m, float u0, float v0, float u1, float v1) {
    if (g_cur != p.id) {
        glUseProgram(p.id);
        g_cur = p.id;
    }
    glUniform2f(p.proj, 2.0f / g_vw, 2.0f / g_vh);
    glUniform4f(p.m1, m.a, m.b, m.c, m.d);
    glUniform2f(p.m2, m.tx, m.ty);
    glUniform4f(p.uv, u0, v0, u1 - u0, v1 - v0);
}

void drawTexture(unsigned tex, const Mat &m, float u0, float v0, float u1, float v1, float alpha, const CMat *cm) {
    if (alpha <= 0.003f || !tex)
        return;
    use(g_tex, m, u0, v0, u1, v1);
    glUniform1f(g_tex.alpha, alpha);
    if (cm && !cm->identity) {
        // GLSL mat4 is column-major; rows of the color matrix become columns.
        GLfloat mm[16];
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++)
                mm[col * 4 + row] = cm->m[row * 5 + col];
        glUniformMatrix4fv(g_tex.cm, 1, GL_FALSE, mm);
        glUniform4f(g_tex.co, cm->m[4], cm->m[9], cm->m[14], cm->m[19]);
        glUniform1f(g_tex.usecm, 1.0f);
    } else {
        glUniform1f(g_tex.usecm, 0.0f);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void drawSolid(const Mat &m, float r, float g, float b, float a) {
    if (a <= 0.003f)
        return;
    use(g_solid, m, 0, 0, 1, 1);
    glUniform4f(g_solid.color, r, g, b, a);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void drawBlend(unsigned texA, unsigned texB, const Mat &m, float alpha, float t) {
    use(g_blend, m, 0, 1, 1, 0);
    glUniform1f(g_blend.alpha, alpha);
    glUniform1f(g_blend.t, std::max(0.0f, std::min(1.0f, t)));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texB);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void drawImageBlend(unsigned texA, unsigned texB, unsigned texCtrl, const Mat &m, float alpha, float t, int ramp) {
    use(g_image, m, 0, 1, 1, 0);
    if (ramp < 1)
        ramp = 1;
    float start = -1.0f, end = ramp / 256.0f;
    glUniform1f(g_image.alpha, alpha);
    glUniform1f(g_image.offset, start + (end - start) * t);
    glUniform1f(g_image.mult, 256.0f / ramp);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, texCtrl);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, texB);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texA);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

unsigned uploadTexture(const uint8_t *px, int w, int h, bool rgb, bool mipmap) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    GLenum fmt = rgb ? GL_RGB : GL_RGBA;
    glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mipmap) {
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    } else {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }
    return t;
}

void deleteTexture(unsigned tex) {
    GLuint t = tex;
    glDeleteTextures(1, &t);
}

} // namespace gfx

void readScreenPixels(int w, int h, uint8_t *out) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, out);
}

void updateTextureRGBA(unsigned tex, const uint8_t *px, int w, int h) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
}

namespace gfx {

bool readPixels(RenderTarget *rt, std::vector<uint8_t> &rgba) {
    glBindFramebuffer(GL_FRAMEBUFFER, rt->fbo);
    std::vector<uint8_t> px((size_t)rt->w * rt->h * 4);
    glReadPixels(0, 0, rt->w, rt->h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    rgba.resize(px.size());
    // content is drawn with y down in NDC, so the top of the image is the last GL row
    for (int y = 0; y < rt->h; y++)
        memcpy(&rgba[(size_t)y * rt->w * 4], &px[(size_t)(rt->h - 1 - y) * rt->w * 4], (size_t)rt->w * 4);
    return true;
}

} // namespace gfx
