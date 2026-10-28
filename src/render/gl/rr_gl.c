/*
 * rr_gl.c: the renderer backend on OpenGL 3.3 core (Windows, on the host's
 * window; macOS, offscreen only, for checking it there: the Mac's windows
 * draw with Metal): the frame is drawn into a
 * framebuffer object the size of the window, then copied onto the window's
 * back buffer and swapped. Pictures go on with the sharp bilinear shader
 * (rr_size.h's weight; fwidth gives the texels to one target pixel); a
 * frame with Mode 7 with rr_mode7.h's arithmetic in a fragment shader.
 *
 * GL keeps rows bottom up: the frame is drawn with its top at NDC +1, so the
 * framebuffer object's last row is the picture's first, and a read back
 * turns the rows over.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl_api.h"
#include "rr.h"
#include "rr_size.h"
#include "rr_mode7.h"
#include "host.h"

/* the host's window for GL (the Windows host's HWND), its size in pixels;
   without one, offscreen */
void *host_window_handle(void);
void host_window_pixels(int *w, int *h);
#if !defined(_WIN32)          /* (the Windows hosts, windowed and null, give both) */
__attribute__((weak)) void *host_window_handle(void) { return NULL; }
__attribute__((weak)) void host_window_pixels(int *w, int *h) { *w = *h = 0; }
#endif

static const char *k_vs =
    "#version 330 core\n"
    "layout(location = 0) in vec4 a;\n"
    "out vec2 uv;\n"
    "void main() { gl_Position = vec4(a.xy, 0.0, 1.0); uv = a.zw; }\n";
/* one to one */
static const char *k_copy =
    "#version 330 core\n"
    "uniform sampler2D t;\n"
    "in vec2 uv;\n"
    "out vec4 o;\n"
    "void main() { o = vec4(texelFetch(t, ivec2(uv * vec2(textureSize(t, 0))), 0).rgb, 1.0); }\n";
/* sharp bilinear: whole texels at any scale, blended only across the one
   target pixel a texel edge falls in */
static const char *k_sharp =
    "#version 330 core\n"
    "uniform sampler2D t;\n"
    "in vec2 uv;\n"
    "out vec4 o;\n"
    "void main() {\n"
    "  vec2 ts = vec2(textureSize(t, 0)), px = uv * ts - 0.5;\n"
    "  vec2 i = floor(px), f = px - i, d = max(fwidth(px), vec2(1e-5));\n"
    "  f = clamp((f - 0.5) / d + 0.5, 0.0, 1.0);\n"
    "  o = vec4(texture(t, (i + 0.5 + f) / ts).rgb, 1.0);\n"
    "}\n";

/* an overlay: bilinear, its alpha kept for the blend */
static const char *k_over =
    "#version 330 core\n"
    "uniform sampler2D t;\n"
    "in vec2 uv;\n"
    "out vec4 o;\n"
    "void main() { o = texture(t, uv); }\n";

/* a frame with Mode 7 presented (rr_mode7.h, the same arithmetic): the
   shader the Vulkan backend's is made from (vulkan/shaders/m7_body.glsl) */
#include "rr_m7_glsl.h"

static GLuint s_copy, s_sharp, s_over, s_m7, s_otex, s_vao, s_vbo, s_fbo, s_col, s_pic;
static GLuint s_m7buf[7], s_m7tex[7], s_plane;
static uint32_t s_plane_gen;
static int s_planes, s_plane_layers;
static int s_pw, s_ph;
static int s_rw, s_rh, s_window;

static GLuint shader(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    GLint ok = 0;
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "rr: gl shader: %s\n", log);
        return 0;
    }
    return s;
}

static GLuint program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram(), v = shader(GL_VERTEX_SHADER, vs), f = shader(GL_FRAGMENT_SHADER, fs);
    GLint ok = 0;
    if (!v || !f)
        return 0;
    glAttachShader(p, v);
    glAttachShader(p, f);
    glBindAttribLocation(p, 0, "a");
    glBindFragDataLocation(p, 0, "o");
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "rr: gl program: %s\n", log);
        return 0;
    }
    glUseProgram(p);
    glUniform1i(glGetUniformLocation(p, "t"), 0);
    return p;
}

static void targets(int w, int h)
{
    if (!s_fbo)
        glGenFramebuffers(1, &s_fbo);
    if (!s_col)
        glGenTextures(1, &s_col);
    glBindTexture(GL_TEXTURE_2D, s_col);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_col, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        fprintf(stderr, "rr: gl: the target is not complete\n");
    s_rw = w;
    s_rh = h;
}

int rr_open(void)
{
    void *win = host_window_handle();
    int w = 0, h = 0;
    if (!gl_ctx_open(win) || !gl_api_load())
        return 0;
    s_window = win != NULL;
    s_copy = program(k_vs, k_copy);
    s_sharp = program(k_vs, k_sharp);
    s_over = program(k_vs, k_over);
    s_m7 = program(k_vs, k_m7);
    if (!s_copy || !s_sharp || !s_over || !s_m7)
        return 0;
    {
        static const char *names[8] = { "Pb", "Sb", "palb", "vramb", "picb", "colsb", "rowsb", "planet" };
        int i;
        glUseProgram(s_m7);
        glGenBuffers(7, s_m7buf);
        glGenTextures(7, s_m7tex);
        glGenTextures(1, &s_plane);
        for (i = 0; i < 8; i++)
            glUniform1i(glGetUniformLocation(s_m7, names[i]), i + 1);
    }
    glGenTextures(1, &s_otex);
    glBindTexture(GL_TEXTURE_2D, s_otex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glGenBuffers(1, &s_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, 6 * 16, NULL, GL_STREAM_DRAW);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, (const void *)0);
    glEnableVertexAttribArray(0);
    glGenTextures(1, &s_pic);
    glBindTexture(GL_TEXTURE_2D, s_pic);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (s_window)
        host_window_pixels(&w, &h);
    if (w < 1 || h < 1)
        rr_headless_size(&w, &h);
    targets(w, h);
    return 1;
}

void rr_close(void) {}

const char *rr_name(void) { return "gl"; }

void rr_target(int *w, int *h)
{
    if (s_window) {
        int fw, fh;
        host_window_pixels(&fw, &fh);
        if (fw > 0 && fh > 0 && (fw != s_rw || fh != s_rh))
            targets(fw, fh);
    }
    *w = s_rw;
    *h = s_rh;
}

void rr_begin(uint32_t argb)
{
    int w, h;
    rr_target(&w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_rw, s_rh);
    glClearColor(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* texture tex (w x h, on unit 0) drawn over target rectangle d: one to one
   when d is w x h, else sharp bilinear */
static void draw(GLuint tex, int w, int h, const int d[4])
{
    static const int ix[6] = { 0, 1, 2, 2, 1, 3 };
    float c[4][4] = { { (float)d[0], (float)d[1], 0, 0 }, { (float)d[2], (float)d[1], 1, 0 },
                      { (float)d[0], (float)d[3], 0, 1 }, { (float)d[2], (float)d[3], 1, 1 } };
    float q[6][4];
    int i;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    for (i = 0; i < 6; i++) {
        q[i][0] = c[ix[i]][0] / s_rw * 2 - 1;
        q[i][1] = 1 - c[ix[i]][1] / s_rh * 2;
        q[i][2] = c[ix[i]][2];
        q[i][3] = c[ix[i]][3];
    }
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glUseProgram(d[2] - d[0] == w && d[3] - d[1] == h ? s_copy : s_sharp);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void rr_image(const uint32_t *argb, int w, int h, const int d[4])
{
    if (w <= 0 || h <= 0 || d[2] <= d[0] || d[3] <= d[1])
        return;
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_pic);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != s_pw || h != s_ph) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, argb);
        s_pw = w;
        s_ph = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    }
    draw(s_pic, w, h, d);
}

void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s)
{
    static const GLenum fmt[7] = { GL_RGBA32UI, GL_R32I, GL_R16UI, GL_R16UI, GL_R32UI, GL_R32I, GL_R32I };
    static const int ix[6] = { 0, 1, 2, 2, 1, 3 };
    int ow = d[2] - d[0], oh = d[3] - d[1], i, du[28], dv[28];
    size_t np = (size_t)m->pw * (size_t)m->ph;
    const void *src[7];
    size_t len[7];
    float c[4][4] = { { (float)d[0], (float)d[1], 0, 0 }, { (float)d[2], (float)d[1], 1, 0 },
                      { (float)d[0], (float)d[3], 0, 1 }, { (float)d[2], (float)d[3], 1, 1 } };
    float q[6][4];
    if (ow <= 0 || oh <= 0 || np == 0)
        return;
    src[0] = m->px;
    len[0] = np * sizeof *m->px;
    src[1] = m->seg;
    len[1] = (size_t)(m->segs ? m->segs : 1) * sizeof *m->seg;
    src[2] = m->pal;
    len[2] = (size_t)(m->pals ? m->pals : 1) * 512;
    src[3] = m->vram;
    len[3] = (size_t)(m->vrams ? m->vrams : 1) * 0x10000;
    src[4] = m->pic;
    len[4] = np * 4;
    src[5] = s->cols;
    len[5] = (size_t)ow * 3 * 4;
    src[6] = s->rows;
    len[6] = (size_t)oh * 4 * 4;
    glUseProgram(s_m7);
    for (i = 0; i < 7; i++) {
        glBindBuffer(GL_TEXTURE_BUFFER, s_m7buf[i]);
        glBufferData(GL_TEXTURE_BUFFER, (GLsizeiptr)len[i], src[i], GL_STREAM_DRAW);
        glActiveTexture(GL_TEXTURE0 + 1 + (GLenum)i);
        glBindTexture(GL_TEXTURE_BUFFER, s_m7tex[i]);
        glTexBuffer(GL_TEXTURE_BUFFER, fmt[i], s_m7buf[i]);
    }
    glActiveTexture(GL_TEXTURE0 + 8);
    glBindTexture(GL_TEXTURE_2D_ARRAY, s_plane);
    if (s_planes < m->vrams || s_plane_gen != m->plane_gen) {
        /* the planes changed: uploaded again */
        int layers = m->vrams ? m->vrams : 1;
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (s_plane_layers < layers) {
            glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_R8UI, 1024, 1024, layers, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            s_plane_layers = layers;
        }
        if (m->vrams)
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, 0, 1024, 1024, m->vrams, GL_RED_INTEGER, GL_UNSIGNED_BYTE, m->plane);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        s_plane_gen = m->plane_gen;
        s_planes = m->vrams;
    }
    /* the supersamples' offsets: 4 from 0, 8 from 4, 16 from 12 */
    memcpy(du, s->du4, sizeof s->du4);
    memcpy(du + 4, s->du8, sizeof s->du8);
    memcpy(du + 12, s->du16, sizeof s->du16);
    memcpy(dv, s->dv4, sizeof s->dv4);
    memcpy(dv + 4, s->dv8, sizeof s->dv8);
    memcpy(dv + 12, s->dv16, sizeof s->dv16);
    glUniform1i(glGetUniformLocation(s_m7, "U_pw"), m->pw);
    glUniform1i(glGetUniformLocation(s_m7, "U_ph"), m->ph);
    glUniform1i(glGetUniformLocation(s_m7, "U_d0"), d[0]);
    glUniform1i(glGetUniformLocation(s_m7, "U_d1"), d[1]);
    glUniform1i(glGetUniformLocation(s_m7, "U_step_u"), s->step_u);
    glUniform1i(glGetUniformLocation(s_m7, "U_step_v"), s->step_v);
    glUniform1i(glGetUniformLocation(s_m7, "U_rh"), s_rh);
    glUniform1iv(glGetUniformLocation(s_m7, "U_du_"), 28, du);
    glUniform1iv(glGetUniformLocation(s_m7, "U_dv_"), 28, dv);
    for (i = 0; i < 6; i++) {
        q[i][0] = c[ix[i]][0] / s_rw * 2 - 1;
        q[i][1] = 1 - c[ix[i]][1] / s_rh * 2;
        q[i][2] = c[ix[i]][2];
        q[i][3] = c[ix[i]][3];
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_rw, s_rh);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glActiveTexture(GL_TEXTURE0);
}

void rr_overlay(const uint32_t *argb, int w, int h, const float q[6])
{
    static const int ix[6] = { 0, 1, 2, 2, 1, 3 };
    float c[4][4] = { { q[0], q[1], 0, 0 }, { q[2], q[3], 1, 0 }, { q[4], q[5], 0, 1 },
                      { q[2] + q[4] - q[0], q[3] + q[5] - q[1], 1, 1 } };
    float v[6 * 4];
    int i;
    if (w <= 0 || h <= 0)
        return;
    for (i = 0; i < 6; i++) {
        v[4 * i + 0] = c[ix[i]][0] / s_rw * 2 - 1;
        v[4 * i + 1] = 1 - c[ix[i]][1] / s_rh * 2;
        v[4 * i + 2] = c[ix[i]][2];
        v[4 * i + 3] = c[ix[i]][3];
    }
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0, 0, s_rw, s_rh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_otex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STREAM_DRAW);
    glUseProgram(s_over);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glDisable(GL_BLEND);
}

void rr_present(void)
{
    if (s_window) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glBlitFramebuffer(0, 0, s_rw, s_rh, 0, 0, s_rw, s_rh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        gl_ctx_swap();
    }
}

int rr_read(uint32_t *argb)
{
    int y;
    uint32_t *row = (uint32_t *)malloc((size_t)s_rw * 4);
    if (!row)
        return 0;
    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, s_rw, s_rh, GL_BGRA, GL_UNSIGNED_BYTE, argb);
    for (y = 0; y < s_rh / 2; y++) {           /* rows top first */
        uint32_t *a = argb + (size_t)y * (size_t)s_rw, *b = argb + (size_t)(s_rh - 1 - y) * (size_t)s_rw;
        memcpy(row, a, (size_t)s_rw * 4);
        memcpy(a, b, (size_t)s_rw * 4);
        memcpy(b, row, (size_t)s_rw * 4);
    }
    free(row);
    return 1;
}
