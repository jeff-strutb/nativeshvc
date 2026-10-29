/*
 * present_check.c: the presentation (ppu.h ppu_present), the frame drawn
 * at an output's resolution with the Mode 7 plane anti-aliased by each
 * output pixel's footprint on it (rr_mode7.h), checked on a cartridge of a
 * few lines of C that shows Mode 7 scenes (rotation and scale, the three
 * wrap modes, flips, a matrix changed each line by HDMA, EXTBG, sprites,
 * colour maths, windows, brightness, mosaic, pseudo-hires, direct colour,
 * a larger screen, one off its centre, a large one, a minified plane),
 * one of tiles,
 * and a flat plane. For each scene's frame,
 * at one to one, scales up, down, of another shape and a large window:
 *
 *   ref      against the definition written again here, independently
 *            of ppu.c and rr_mode7.h (scenes whose composition is BG1
 *            alone): the footprint, the texels' edges covered where it is
 *            within a texel, 4, 8 or 16 samples beyond
 *   quality  against the plane's ground truth (16 x 16 point samples an
 *            output pixel): the mean error at most a quarter of one point
 *            sample's (no anti-aliasing), and where the plane is minified
 *            (supersampled) at most half
 *   tiles    a frame without Mode 7: rr_ref_image's scaling of the picture
 *   flat     a Mode 7 plane of one colour under sprites, windows and
 *            colour maths: rr_ref_image's scaling of the picture, byte for
 *            byte (anti-aliasing changes the plane alone)
 *
 * Exit 0 when every check holds. With arguments, the scenes run through
 * the front end instead (nshvc_main): with NSHVC_RRCHECK=1, the renderer's
 * Mode 7 presentation held to the reference's.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "ppu.h"
#include "rr_size.h"

#define NSCENES 14
static int fails;
static long compared;

/* ---- the cartridge ---- */
static unsigned rng = 12345;
static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}
static void w(unsigned reg, unsigned v) { wr8(0x2100 + reg, (uint8_t)v); }
static void w2(unsigned reg, int v)
{
    w(reg, (unsigned)v & 0xFF);
    w(reg, ((unsigned)v >> 8) & 0xFF);
}

/* each line's M7A and M7D (a floor in perspective), by HDMA */
static void hdma_perspective(int on)
{
    int ch;
    wr8(0x420C, 0);
    if (!on)
        return;
    for (ch = 0; ch < 2; ch++) {
        uint32_t t = 0x7E3000u + (uint32_t)ch * 0x200u;
        int line, k = 0;
        for (line = 0; line < 224; line++) {
            int v = (int)(0x100 * 48.0 / (line + 24.0)) + (ch ? 7 : 0);
            if (line % 112 == 0)
                wr8(t + (uint32_t)k++, 0x80 | 112);
            wr8(t + (uint32_t)k++, (uint8_t)v);
            wr8(t + (uint32_t)k++, (uint8_t)(v >> 8));
        }
        wr8(t + (uint32_t)k, 0);
        wr8(0x4300u + (uint32_t)ch * 16, 0x02);
        wr8(0x4301u + (uint32_t)ch * 16, ch ? 0x1E : 0x1B);
        wr8(0x4302u + (uint32_t)ch * 16, (uint8_t)t);
        wr8(0x4303u + (uint32_t)ch * 16, (uint8_t)(t >> 8));
        wr8(0x4304u + (uint32_t)ch * 16, 0x7E);
    }
    wr8(0x420C, 0x03);
}

static void setup(int k)
{
    int i;
    /* VRAM: the map's tile numbers in the low bytes, the 256 tiles'
       pixels in the high bytes (a quarter of them 0, clear) */
    w(0x15, 0x80);
    w(0x16, 0);
    w(0x17, 0);
    for (i = 0; i < 0x8000; i++) {
        unsigned p = rnd() & 0xFF;
        w(0x18, rnd() & 0xFF);
        w(0x19, (p & 3) == 0 ? 0 : p);
    }
    w(0x21, 0);
    for (i = 0; i < 256; i++)
        w2(0x22, (int)(rnd() & 0x7FFF));
    w(0x02, 0);
    w(0x03, 0);
    for (i = 0; i < 544; i++)
        w(0x04, i < 512 && (i & 3) == 1 ? rnd() % 224 : rnd() & 0xFF);
    w(0x01, 0x02);
    w(0x05, k == 5 ? 0x01 : 0x07);
    w(0x06, 0x00);
    w(0x07, 0x00);
    w(0x08, 0x10);
    w(0x0B, 0x00);
    w(0x1A, 0x00);
    w2(0x1B, 0x0180);
    w2(0x1C, 0x0060);
    w2(0x1D, -0x0060);
    w2(0x1E, 0x0180);
    w2(0x1F, 0x0200);
    w2(0x20, 0x0180);
    w2(0x0D, 0x0123);
    w2(0x0E, 0x0067);
    w(0x23, 0);
    w(0x24, 0);
    w(0x25, 0);
    w(0x2A, 0);
    w(0x2B, 0);
    w(0x2C, 0x01);
    w(0x2D, 0x00);
    w(0x2E, 0);
    w(0x2F, 0);
    w(0x30, 0x00);
    w(0x31, 0x00);
    w(0x32, 0xE0 | 3);
    w(0x33, 0x00);
    hdma_perspective(0);
    switch (k) {
    case 1:   /* magnified, the outside clear, both flips */
        w2(0x1B, 0x0050);
        w2(0x1C, -0x0030);
        w2(0x1D, 0x0030);
        w2(0x1E, 0x0050);
        w2(0x0D, 0x1E00);
        w(0x1A, 0x83);
        break;
    case 2:   /* the outside tile 0, the matrix by line */
        w2(0x1C, 0x0010);
        w2(0x1D, -0x0010);
        w2(0x0E, 0x1F80);
        w(0x1A, 0xC0);
        hdma_perspective(1);
        break;
    case 3:   /* EXTBG, sprites, colour maths, windows, brightness */
    case 4:   /* and mosaic on BG1, pseudo-hires */
    case 9:   /* the same, Mode 7 constant along a line (M7A = M7C = 0) */
        w(0x33, k == 4 ? 0x48 : 0x40);
        w(0x2C, 0x13);
        w(0x2D, 0x12);
        w(0x30, 0x02);
        w(0x31, 0x53);
        w(0x23, 0x02);
        w(0x26, 40);
        w(0x27, 200);
        w(0x2E, 0x01);
        w(0x25, 0x02);
        w(0x2F, 0x10);
        if (k != 3)
            w(0x06, 0x31);
        if (k == 9) {
            w(0x33, 0x48);
            w2(0x1B, 0);
            w2(0x1D, 0);
        } else {
            hdma_perspective(1);
        }
        break;
    case 5:   /* tiles (mode 1) and sprites */
        w(0x2C, 0x17);
        break;
    case 6:   /* a larger screen */
        nshvc_ext_screen(320, 240);
        break;
    case 7:   /* direct colour, the larger screen */
        w(0x30, 0x01);
        break;
    case 10:  /* a Mode 7 plane of one colour under sprites, colour maths
                 and windows: anti-aliasing must change nothing */
        w(0x15, 0x80);
        w(0x16, 0);
        w(0x17, 0);
        for (i = 0; i < 0x8000; i++) {
            w(0x18, 0x01);
            w(0x19, 0x05);
        }
        w(0x2C, 0x11);
        w(0x2D, 0x10);
        w(0x30, 0x02);
        w(0x31, 0x51);
        w(0x23, 0x02);
        w(0x25, 0x02);
        w(0x26, 40);
        w(0x27, 200);
        w(0x2E, 0x11);
        w(0x2F, 0x10);
        hdma_perspective(1);
        break;
    case 11:  /* minified: the plane at a distance, 5 texels a pixel, turned */
        w2(0x1B, 0x0500);
        w2(0x1C, 0x0100);
        w2(0x1D, -0x0100);
        w2(0x1E, 0x0500);
        break;
    case 12:  /* a larger screen with the console's picture off its centre */
        nshvc_ext_screen(320, 272);
        nshvc_ext_screen_origin(8, 40);
        break;
    case 13:  /* a large screen, the console's picture near its top right
                 (at one to one and scaled down) */
        nshvc_ext_screen(2048, 1536);
        nshvc_ext_screen_origin(1700, 40);
        break;
    case 8:   /* the console's screen again, wrap, magnified with a turn */
        nshvc_ext_screen(256, 224);
        w2(0x1B, 0x0040);
        w2(0x1C, 0x0025);
        w2(0x1D, -0x0025);
        w2(0x1E, 0x0040);
        break;
    }
}

static void game(void)
{
    int k;
    wr8(0x004200, 0x00);
    for (k = 0; k < NSCENES; k++) {
        w(0x00, 0x80);
        setup(k);
        wr8(0x7E0100, 0);
        w(0x00, k == 3 || k == 4 || k == 9 ? 0x0B : 0x0F);
        nshvc_wait_vblank();           /* a frame the display came on in */
        wr8(0x7E0100, (uint8_t)(k + 1));
        nshvc_wait_vblank();           /* the scene's whole frame */
        wr8(0x7E0100, 0);
    }
    wr8(0x7E0100, 0xFF);
    for (;;)
        nshvc_wait_vblank();
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, game, "reset", 0 } };

/* ---- the reference ---- */
/* The adaptive anti-aliasing (rr_mode7.h), written again here from its
   definition, for scenes of BG1 alone on the main screen, brightness 15,
   no maths: each picture pixel under an output pixel on a Mode 7 line the
   plane filtered by the pixel's footprint, then sharp bilinear across the
   picture's pixels. Positions in doubles to 1/1024 of a console pixel. */
static const int8_t std4[4][2] = { { -2, -6 }, { 6, -2 }, { -6, 2 }, { 2, 6 } };
static const int8_t std8[8][2] = { { 1, -3 }, { -1, 3 }, { 5, 1 }, { -3, -5 }, { -5, 5 }, { -7, -1 }, { 3, 7 }, { 7, -7 } };
static const int8_t std16[16][2] = { { 1, 1 }, { -1, -3 }, { -3, 2 }, { 4, -1 }, { -5, -2 }, { 2, 5 }, { 5, 3 }, { 3, -5 },
                                     { -2, 6 }, { 0, -7 }, { -4, -6 }, { -6, 4 }, { -8, 0 }, { 7, -4 }, { 6, 7 }, { -7, -8 } };

static int s13(int v) { return (v & 0x1000) ? v | ~0x1FFF : v & 0x1FFF; }
static int c10(int v) { return (v & 0x2000) ? v | ~0x3FF : v & 0x3FF; }
static long fmod_l(long v, long m) { return ((v % m) + m) % m; }
static long floor_l(long long v, long long m) { return (long)(v >= 0 ? v / m : -((-v + m - 1) / m)); }

static uint32_t rgb(unsigned c)
{
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* a picture row's Mode 7 line: its state, and the matrix's origin */
typedef struct {
    const PpuScreen *st;
    const uint16_t *pal, *vr;
    long ox, oy;
    int a, b, c, d, sel, m7;
} RefLine;

static RefLine ref_line(const PpuFrame *f, int r)
{
    int top = f->top, line = r - top + 1, L, y, x0, y0, cx, cy;
    RefLine l;
    L = line < 1 ? 1 : line > f->height ? f->height : line;
    l.st = &f->line[L];
    l.m7 = (l.st->bgmode & 7) == 7 && !(l.st->inidisp & 0x80);
    l.pal = f->palette[L];
    l.vr = f->vram[f->version[L]];
    l.a = l.st->m7a, l.b = l.st->m7b, l.c = l.st->m7c, l.d = l.st->m7d, l.sel = l.st->m7sel;
    cx = s13(l.st->m7x), cy = s13(l.st->m7y);
    y = line;
    if (l.sel & 2)
        y = 255 - y;
    x0 = c10(s13(l.st->m7hofs) - cx);
    y0 = c10(s13(l.st->m7vofs) - cy);
    l.ox = ((l.a * x0) & ~63) + ((l.b * y) & ~63) + ((l.b * y0) & ~63) + (cx << 8);
    l.oy = ((l.c * x0) & ~63) + ((l.d * y) & ~63) + ((l.d * y0) & ~63) + (cy << 8);
    return l;
}

/* the colour of texel (px, py) of the line */
static uint32_t ref_texel(const RefLine *l, long px, long py)
{
    long tile, pix;
    if (px < 0 || px > 1023 || py < 0 || py > 1023) {
        if ((l->sel & 0xC0) == 0x80)
            return rgb(l->pal[0]);
        tile = (l->sel & 0xC0) == 0xC0 ? 0 : l->vr[fmod_l(py, 1024) / 8 * 128 + fmod_l(px, 1024) / 8] & 0xFF;
    } else {
        tile = l->vr[py / 8 * 128 + px / 8] & 0xFF;
    }
    pix = l->vr[tile * 64 + fmod_l(py, 8) * 8 + fmod_l(px, 8)] >> 8;
    if (!pix)
        return rgb(l->pal[0]);
    if (l->st->cgwsel & 1)   /* direct colour: BGR 233, palette 0 */
        return rgb((unsigned)((pix & 7) << 2 | ((pix >> 3) & 7) << 7 | ((pix >> 6) & 3) << 13));
    return rgb(l->pal[pix]);
}

/* the plane's position (1/256 texel) at screen column u and rows e below
   the line's own sample, both in 1/1024ths */
static void ref_pos(const RefLine *l, long long u, long long e, long *px, long *py)
{
    if (l->sel & 1)
        u = 255 * 1024 - u;
    if (l->sel & 2)
        e = -e;
    *px = floor_l(l->ox * 1024LL + l->a * u + l->b * e, 1024);
    *py = floor_l(l->oy * 1024LL + l->c * u + l->d * e, 1024);
}

static long nearest(double v) { return (long)floor(v + 0.5); }

/* the next texel's part (1/256ths) of a footprint of spans sa, sb along an
   axis, f past the texel's centre: the sum of two uniform spans beyond the
   edge, in doubled units to keep it whole */
static long ref_edge(long f, long sa, long sb)
{
    long A = sa > sb ? sa : sb, B = sa > sb ? sb : sa, T = 2 * (128 - f), cov;
    if (A == 0)
        return T < 0 ? 256 : T > 0 ? 0 : 128;
    if (T <= -(A + B))
        cov = 0;
    else if (B > 0 && T <= -(A - B))
        cov = floor_l(256LL * (T + A + B) * (T + A + B), 8LL * A * B);
    else if (T < A - B || B == 0)
        cov = 128 + floor_l(128LL * T, A);
    else if (T < A + B)
        cov = 256 - floor_l(256LL * (A + B - T) * (A + B - T), 8LL * A * B);
    else
        cov = 256;
    cov = cov < 0 ? 0 : cov > 256 ? 256 : cov;
    return 256 - cov;
}

/* picture pixel (i, r) over output pixel (X, Y) of an ow x oh presentation */
static uint32_t ref_q(const PpuFrame *f, const uint32_t *pic, int ow, int oh, int i, int r, int X, int Y)
{
    int pw = ppu_frame_width(f), ph = f->screen_height, sl = f->left;
    long long u, e;
    long su, sv, ax, bx, ay, by, wx, wy, w, px, py;
    RefLine l;
    i = i < 0 ? 0 : i >= pw ? pw - 1 : i;
    r = r < 0 ? 0 : r >= ph ? ph - 1 : r;
    l = ref_line(f, r);
    if (!l.m7)
        return pic[(size_t)r * (size_t)pw + (size_t)i];
    u = nearest((((double)X + 0.5) * pw / ow - 0.5 - sl) * 1024);
    e = nearest((((double)Y + 0.5) * ph / oh - 0.5) * 1024) - 1024LL * r;
    su = nearest(1024.0 * pw / ow);
    sv = nearest(1024.0 * ph / oh);
    ax = floor_l((long long)labs(l.a) * su, 1024);
    bx = floor_l((long long)labs(l.b) * sv, 1024);
    ay = floor_l((long long)labs(l.c) * su, 1024);
    by = floor_l((long long)labs(l.d) * sv, 1024);
    wx = ax + bx;
    wy = ay + by;
    w = wx > wy ? wx : wy;
    ref_pos(&l, u, e, &px, &py);
    if (w <= 256) {
        long qx = px - 128, qy = py - 128, ix = floor_l(qx, 256), iy = floor_l(qy, 256);
        long ex = ref_edge(qx - 256 * ix, ax, bx), ey = ref_edge(qy - 256 * iy, ay, by);
        uint32_t c00, c01, c10, c11;
        if (ex == 256)
            ix++, ex = 0;
        if (ey == 256)
            iy++, ey = 0;
        c00 = ref_texel(&l, ix, iy);
        c01 = ex ? ref_texel(&l, ix + 1, iy) : c00;
        c10 = ey ? ref_texel(&l, ix, iy + 1) : c00;
        c11 = ey ? (ex ? ref_texel(&l, ix + 1, iy + 1) : c10) : c01;
        if (!ey)
            return rr_mix(c00, c01, (float)ex / 256.0f);
        return rr_mix(rr_mix(c00, c01, (float)ex / 256.0f), rr_mix(c10, c11, (float)ex / 256.0f), (float)ey / 256.0f);
    }
    {
        int n = w <= 512 ? 4 : w <= 1024 ? 8 : 16, k;
        const int8_t (*p)[2] = n == 4 ? std4 : n == 8 ? std8 : std16;
        unsigned sum[3] = { 0, 0, 0 };
        for (k = 0; k < n; k++) {
            long du = nearest(p[k][0] * 64.0 * pw / ow), dv = nearest(p[k][1] * 64.0 * ph / oh);
            uint32_t c;
            ref_pos(&l, u + du, e + dv, &px, &py);
            c = ref_texel(&l, floor_l(px, 256), floor_l(py, 256));
            sum[0] += c & 0xFF;
            sum[1] += (c >> 8) & 0xFF;
            sum[2] += (c >> 16) & 0xFF;
        }
        return 0xFF000000u | ((sum[2] + n / 2) / n) << 16 | ((sum[1] + n / 2) / n) << 8 | ((sum[0] + n / 2) / n);
    }
}

static void ref_present(const PpuFrame *f, const uint32_t *pic, uint32_t *out, int ow, int oh)
{
    int pw = ppu_frame_width(f), ph = f->screen_height, X, Y;
    float dx = (float)pw / (float)ow, dy = (float)ph / (float)oh;
    for (Y = 0; Y < oh; Y++) {
        int ty;
        float fy = rr_sharp(((float)Y + 0.5f) * dy, dy, &ty);
        for (X = 0; X < ow; X++) {
            int tx;
            float fx = rr_sharp(((float)X + 0.5f) * dx, dx, &tx);
            uint32_t q00 = ref_q(f, pic, ow, oh, tx, ty, X, Y), q01 = fx > 0 ? ref_q(f, pic, ow, oh, tx + 1, ty, X, Y) : q00;
            uint32_t q10, q11;
            if (!(fy > 0)) {
                out[(size_t)Y * (size_t)ow + (size_t)X] = rr_mix(q00, q01, fx);
                continue;
            }
            q10 = ref_q(f, pic, ow, oh, tx, ty + 1, X, Y);
            q11 = fx > 0 ? ref_q(f, pic, ow, oh, tx + 1, ty + 1, X, Y) : q10;
            out[(size_t)Y * (size_t)ow + (size_t)X] = rr_mix(rr_mix(q00, q01, fx), rr_mix(q10, q11, fx), fy);
        }
    }
}

/* Against the plane's ground truth: at the output pixels inside one
   picture pixel on a Mode 7 line, the mean of 16 x 16 point samples of
   the plane over the pixel; the mean error (a channel, 0-255) of the
   presentation and of one point sample at the centre (no anti-aliasing) */
static void quality(const PpuFrame *f, const uint32_t *got, int ow, int oh, double *e_aa, double *e_point, double *m_aa,
                    double *m_point, long *minified)
{
    int pw = ppu_frame_width(f), ph = f->screen_height, sl = f->left, X, Y, sx, sy, ch;
    float dx = (float)pw / (float)ow, dy = (float)ph / (float)oh;
    double ea = 0, ep = 0, ma = 0, mp = 0;
    long n = 0, nm = 0;
    long su = nearest(1024.0 * pw / ow), sv = nearest(1024.0 * ph / oh);
    for (Y = 0; Y < oh; Y++) {
        int ty;
        float fy = rr_sharp(((float)Y + 0.5f) * dy, dy, &ty);
        RefLine l = ref_line(f, ty < 0 ? 0 : ty >= ph ? ph - 1 : ty);
        if (fy > 0 || !l.m7 || ty < 0 || ty >= ph)
            continue;
        for (X = 0; X < ow; X++) {
            int tx;
            float fx = rr_sharp(((float)X + 0.5f) * dx, dx, &tx);
            double sum[3] = { 0, 0, 0 };
            uint32_t pt, g = got[(size_t)Y * (size_t)ow + (size_t)X];
            if (fx > 0)
                continue;
            for (sy = 0; sy < 16; sy++)
                for (sx = 0; sx < 16; sx++) {
                    double u = (X + (sx + 0.5) / 16) * pw / ow - 0.5 - sl, e = (Y + (sy + 0.5) / 16) * ph / oh - 0.5 - ty;
                    long px, py;
                    uint32_t c;
                    ref_pos(&l, (long long)floor(u * 1024), (long long)floor(e * 1024), &px, &py);
                    c = ref_texel(&l, floor_l(px, 256), floor_l(py, 256));
                    for (ch = 0; ch < 3; ch++)
                        sum[ch] += (c >> (8 * ch)) & 0xFF;
                }
            {
                long px, py;
                ref_pos(&l, nearest((((double)X + 0.5) * pw / ow - 0.5 - sl) * 1024),
                        nearest((((double)Y + 0.5) * ph / oh - 0.5) * 1024) - 1024LL * ty, &px, &py);
                pt = ref_texel(&l, floor_l(px, 256), floor_l(py, 256));
            }
            {
                /* minified here: supersampled */
                long wx = floor_l((long long)labs(l.a) * su, 1024) + floor_l((long long)labs(l.b) * sv, 1024);
                long wy = floor_l((long long)labs(l.c) * su, 1024) + floor_l((long long)labs(l.d) * sv, 1024);
                int mini = (wx > wy ? wx : wy) > 256;
                for (ch = 0; ch < 3; ch++) {
                    double gt = sum[ch] / 256, a1 = fabs((double)((g >> (8 * ch)) & 0xFF) - gt);
                    double p1 = fabs((double)((pt >> (8 * ch)) & 0xFF) - gt);
                    ea += a1;
                    ep += p1;
                    if (mini) {
                        ma += a1;
                        mp += p1;
                    }
                }
                n += 3;
                nm += mini ? 3 : 0;
            }
        }
    }
    *e_aa = n ? ea / n : 0;
    *e_point = n ? ep / n : 0;
    *m_aa = nm ? ma / nm : 0;
    *m_point = nm ? mp / nm : 0;
    *minified = nm / 3;
}

static void same(const char *what, int scene, const uint32_t *got, const uint32_t *want, int w, int h)
{
    long bad = 0, i, n = (long)w * h;
    for (i = 0; i < n; i++)
        if (got[i] != want[i]) {
            if (!bad)
                printf("  first at (%ld, %ld): got %08x want %08x\n", i % w, i / w, got[i], want[i]);
            bad++;
        }
    compared += n;
    printf("%s scene %d %-36s %dx%d: %ld differ\n", bad ? "FAIL" : "ok  ", scene, what, w, h, bad);
    if (bad)
        fails++;
}

static void check_scene(int k)
{
    const PpuFrame *f = ppu_frame();
    int pw, ph, si, j;
    const uint32_t *pic = ppu_picture(&pw, &ph);
    /* output sizes: one to one, up, of another shape, down, and a large window */
    const double sc[5][2] = { { 1, 1 }, { 2.5, 2.5 }, { 3.37, 2.91 }, { 0.6, 0.77 }, { 6, 6 } };
    int simple = k == 0 || k == 1 || k == 2 || k == 6 || k == 7 || k == 8 || k == 11 || k == 12 || k == 13;
    uint32_t *a, *b;
    char what[80];
    a = malloc((size_t)pw * 6 * (size_t)ph * 6 * 4);
    b = malloc((size_t)pw * 6 * (size_t)ph * 6 * 4);
    printf("scene %d: %dx%d picture, Mode 7 %s\n", k, pw, ph, ppu_frame_mode7(f) ? "shown" : "not shown");
    if (ppu_frame_mode7(f) != (k != 5)) {
        printf("FAIL scene %d: Mode 7 expected %s\n", k, k != 5 ? "shown" : "not shown");
        fails++;
    }
    if (k == 12 && (f->left != 8 || f->top != 40 || pw != 320 || ph != 272)) {
        printf("FAIL scene 12: the picture at (%d, %d) of %dx%d, not (8, 40) of 320x272\n", f->left, f->top, pw, ph);
        fails++;
    }
    for (si = 0; si < 5; si++) {
        int ow = (int)(pw * sc[si][0] + 0.5), oh = (int)(ph * sc[si][1] + 0.5), d[4] = { 0, 0, ow, oh };
        if (k == 13 && si != 0 && si != 3)
            continue;   /* the large screen: one to one and down */
        ppu_present(f, pic, a, ow, oh);
        if (k == 5 || k == 10) {
            /* no Mode 7, or a plane of one colour: the picture scaled, the
               sprites' edges, windows and colour maths as the pixel art's */
            for (j = 0; j < ow * oh; j++)
                b[j] = 0xFF000000u;
            rr_ref_image(b, ow, oh, pic, pw, ph, d);
            snprintf(what, sizeof what, "%s: rr_ref_image x%.2fx%.2f", k == 5 ? "tiles" : "flat plane", sc[si][0], sc[si][1]);
            same(what, k, a, b, ow, oh);
        }
        if (simple) {
            ref_present(f, pic, b, ow, oh);
            snprintf(what, sizeof what, "ref x%.2fx%.2f", sc[si][0], sc[si][1]);
            same(what, k, a, b, ow, oh);
        }
        if (simple && (k == 0 || k == 2 || k == 8 || k == 11 || k == 12) && (si >= 1 || k == 11) && si != 3) {
            double ea, ep, ma, mp;
            long nm;
            int ok;
            quality(f, a, ow, oh, &ea, &ep, &ma, &mp, &nm);
            ok = ea <= 0.25 * ep && (!nm || ma <= 0.5 * mp);
            printf("%s scene %d quality x%.2fx%.2f: mean error %.2f, point sampled %.2f; minified (%ld): %.2f, %.2f\n",
                   ok ? "ok  " : "FAIL", k, sc[si][0], sc[si][1], ea, ep, nm, ma, mp);
            if (!ok)
                fails++;
        }
    }
    if (k == 10) {
        /* (the scene must have sprites, windows and maths: colours other
           than the plane's) */
        long other = 0;
        for (j = 0; j < pw * ph; j++)
            other += pic[j] != pic[0];
        printf("%s scene 10 pixels not the plane's colour: %ld\n", other > pw * ph / 20 ? "ok  " : "FAIL", other);
        if (other <= pw * ph / 20)
            fails++;
    }
    if (k == 3) {
        /* the time it takes (not judged): a 4:3 window 800 pixels high */
        uint32_t *o = malloc((size_t)1067 * 800 * 4);
        clock_t t0 = clock();
        for (j = 0; j < 5; j++)
            ppu_present(f, pic, o, 1067, 800);
        printf("time: scene 3 at 1067x800: %.1f ms a frame (the CPU's reference)\n",
               (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 5);
        free(o);
    }
    free(a);
    free(b);
}

int main(int argc, char **argv)
{
    nshvc_cart cart;
    int seen = 0;
    memset(&cart, 0, sizeof cart);
    cart.title = "present_check";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    if (argc > 1)
        /* the scenes through the front end (--headless --frames N, with
           NSHVC_RRCHECK=1: each renderer held to the reference) */
        return nshvc_main(&cart, argc, argv);
    nshvc_power(&cart);
    while (seen < NSCENES) {
        uint8_t id;
        nshvc_run_frame();
        id = nshvc_wram()[0x100];
        if (id >= 1 && id <= NSCENES && id == seen + 1) {
            check_scene(id - 1);
            seen++;
        }
        if (nshvc_frames() > 200) {
            printf("FAIL: the scenes did not all show (%d)\n", seen);
            return 1;
        }
    }
    printf("present_check: %d scenes, %ld pixels compared, %d check(s) failed\n", seen, compared, fails);
    return fails ? 1 : 0;
}
