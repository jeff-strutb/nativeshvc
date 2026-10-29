/*
 * rr_mode7.h: a frame with Mode 7 presented at the target's resolution
 * (rr.h rr_mode7), the one definition: the software backend draws it with
 * rr_ref_mode7, the GPU backends' shaders compute the same, and the PPU's
 * own presentation (ppu.h ppu_present) is it drawn into a buffer.
 *
 * The PPU gives each picture pixel on a Mode 7 line everything that does
 * not depend on which of Mode 7's texels shows there (RrM7Px): the main
 * and sub screens' colour without Mode 7 (the sprites' pixel over the
 * backdrop), whether each Mode 7 layer may show on each screen there (its
 * enable bits and windows), the colour window's regions, and its line's or
 * segment's state (RrM7Seg: the matrix's origin for the line, the matrix,
 * the wrap mode and flips, colour maths, the fixed colour, brightness, its
 * palette and VRAM). A texel's colour there is then the console's rules
 * (ppu.c draw_line, finish) for that one pixel with that texel (the
 * layers' priorities against the sprites, colour maths, brightness).
 * Sprites, the other backgrounds, the windows and which layer shows are
 * the console pixel's whatever the texel: anti-aliasing only ever changes
 * the Mode 7 plane.
 *
 * Anti-aliasing is adaptive, by the output pixel's footprint on the plane
 * (the texels it spans across and down, from the line's matrix):
 *
 *   at most a texel    (the plane magnified, or one to one) the texel's
 *                      edges covered exactly: the texel under the pixel's
 *                      centre, blended with the next one only across the
 *                      one output pixel a texel edge falls in, by the part
 *                      of the pixel's footprint past the edge, along each
 *                      axis (sharp bilinear on the plane: 1 to 4 texels)
 *   more               (the plane minified, toward the horizon)
 *                      supersampled: 4 samples up to 2 texels, 8 up to 4,
 *                      16 beyond, at the standard positions (rr_samples),
 *                      each the texel under it, the colours averaged
 *
 * then each picture pixel's colour so filtered blended with sharp
 * bilinear across the picture's pixels, as the pixel art is.
 *
 * A layer's canvas (nshvc_ext_canvas_chars, an extension) is presented the
 * same way: the PPU gives each picture pixel whose tile the canvas covers
 * the screens without that layer (the other layers', the sprites' and the
 * backdrop's pixel, with their depths), whether the layer may show on each
 * screen, the colour window's regions, and the pixel's place in the canvas
 * (its character, its pixel in it, the flips, palette and the layer's
 * depth there: all the console pixel's, as a background's scroll is whole
 * pixels). At an output pixel the canvas's texel under its centre (k
 * texels a console pixel along each axis, one sample: the canvas is the
 * window's resolution) is composed by the console's rules for that pixel
 * (rr_cv_q).
 *
 * All of it in 32-bit integers (but the blends, rr_size.h rr_mix): a
 * position is a pixel centre's from tables the CPU computes exactly plus a
 * sample's own offset (rr_m7_tables), the footprint and the edges'
 * weights integers, so every backend looks up the same texels.
 */
#ifndef PORT_RR_MODE7_H
#define PORT_RR_MODE7_H

#include <stdint.h>
#include <string.h>

#include "rr_size.h"

/* RrM7Px.w2's low 16 bits */
#define RR_M7_NOT      0x0001u  /* not on a Mode 7 line: the picture's pixel */
#define RR_M7_BLACK    0x0002u  /* a hires line's first half pixel */
#define RR_M7_SWAP     0x0004u  /* a hires line's even half pixel: the sub screen's colour */
#define RR_M7_BG1_MAIN 0x0008u  /* BG1 may show on the main screen here */
#define RR_M7_BG1_SUB  0x0010u
#define RR_M7_BG2_MAIN 0x0020u  /* BG2 (EXTBG) */
#define RR_M7_BG2_SUB  0x0040u
#define RR_M7_CLIP     0x0080u  /* inside the colour window's clip region */
#define RR_M7_PREVENT  0x0100u  /* inside its prevent region */
#define RR_M7_BG1_FIX  0x0200u  /* BG1 under mosaic: its block's texel, fix 0 */
#define RR_M7_BG2_FIX  0x0400u
#define RR_CV          0x0800u  /* a canvas pixel (BG1_MAIN, _SUB: its layer may show; w3 its place) */

/* A picture pixel, four words (a GPU's uint4):
     w0  main colour | sub colour << 16 (15-bit BGR, without Mode 7)
     w1  main depth | sub depth << 8 | main layer << 16 | sub layer << 24
         (ppu.c Pixel: 4 or 6 sprites, 5 the backdrop)
     w2  flags (RR_M7_*) | segment << 16
     w3  BG1's fixed texel | BG2's << 8 (the pixel byte, RR_M7_*_FIX); a
         canvas pixel (RR_CV): its character in the canvas (14 bits) | the
         pixel in it, x << 14, y << 17 | the flips, H << 20, V << 21 | its
         palette << 22 | the layer's depth << 25 (4 bits) | the layer << 29 */
typedef struct {
    uint32_t w0, w1, w2, w3;
} RrM7Px;

/* A line's or segment's state, eight words:
     s0, s1  the matrix's origin for the line, ox and oy (ppu.c m7_origin,
             1/256 texel)
     s2, s3  M7A | M7B << 16, M7C | M7D << 16 (16-bit, signed)
     s4      M7SEL | direct colour << 8 | CGADSUB << 16 | CGWSEL << 24
     s5      the fixed colour | brightness << 16 | VRAM (its index) << 24
     s6      the palette's index
     s7      0; a canvas line's: the layer's bits a pixel | the mode << 8 */
typedef struct {
    int32_t s[8];
} RrM7Seg;

/* A frame for rr_mode7: its picture (ppu_draw_frame's, pw x ph), a record
   for each picture pixel, the states, the palettes (256 colours each) and
   the VRAM (0x8000 words each) they name, and each VRAM's Mode 7 plane
   (1024 x 1024 pixel bytes, row by row: the map's tiles laid out, so a
   texel is one read), plane_gen counting the times a plane has changed
   (a GPU backend uploads them again when it does). */
typedef struct RrMode7 {
    const uint32_t *pic;
    int pw, ph;
    int cpp;                  /* picture pixels a console pixel: 1, or 2 when hires */
    int left;                 /* the hardware's column 0 in the screen (ppu.c sl) */
    const RrM7Px *px;
    const uint8_t *row_m7;    /* each picture row: a pixel of it is on a Mode 7 line */
    const RrM7Seg *seg;
    int segs;
    const uint16_t *pal;
    int pals;
    const uint16_t *vram;
    int vrams;
    const uint8_t *plane;
    uint32_t plane_gen;
    const uint8_t *cv;        /* the canvas's texels (PpuFrame cv), cv_len of them, cv_k a console pixel */
    size_t cv_len;
    int cv_k;
    int cv_w, cv_h;           /* a screen canvas's texels across and down (0: a canvas of characters) */
} RrMode7;

static inline int64_t rr_m7_floordiv(int64_t a, int64_t b)
{
    int64_t q = a / b;
    return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}

/* Where a presentation at ow x oh samples, for rr_mode7: for each output
   column X, cols[3 X ..]: the picture column tx, the weight fx of tx + 1
   (rr_sharp's, the float's bits), and the pixel centre's screen column u
   in 1/1024 of a console pixel to the nearest (the hardware's column 0 at
   0, a column's centre at its integer); for each output row Y,
   rows[4 Y ..]: the picture row ty, fy's bits, whether row ty (or ty + 1,
   fy > 0) is on a Mode 7 line, and the centre's row v in 1/1024 of a
   picture row (row r's centre at 1024 r). step_u, step_v: one output
   pixel in those units, to the nearest (the footprint's scale). A
   supersample's offsets from the centre: du4/dv4, du8/dv8, du16/dv16 for
   4, 8 and 16 samples (rr_samples, 1/16 of an output pixel), in those
   units to the nearest, the same for every pixel. */
typedef struct RrM7Samples {
    int32_t step_u, step_v;
    int32_t du4[4], dv4[4], du8[8], dv8[8], du16[16], dv16[16];
    const int32_t *cols, *rows;
} RrM7Samples;

static inline int32_t rr_m7_round(int64_t num, int64_t den) { return (int32_t)rr_m7_floordiv(2 * num + den, 2 * den); }

static inline void rr_m7_tables(const RrMode7 *m, int ow, int oh, RrM7Samples *s, int32_t *cols, int32_t *rows)
{
    int X, Y, k, pw = m->pw, ph = m->ph;
    float dx = (float)pw / (float)ow, dy = (float)ph / (float)oh;
    int64_t dcol = (int64_t)16 * ow * m->cpp, drow = (int64_t)16 * oh;
    const int8_t (*p4)[2] = rr_samples(4), (*p8)[2] = rr_samples(8), (*p16)[2] = rr_samples(16);
    s->cols = cols;
    s->rows = rows;
    s->step_u = rr_m7_round((int64_t)16 * pw * 1024, dcol);
    s->step_v = rr_m7_round((int64_t)16 * ph * 1024, drow);
    /* p/16 of an output pixel: p pw / (16 ow cpp) console pixels */
    for (k = 0; k < 16; k++) {
        if (k < 4) {
            s->du4[k] = rr_m7_round((int64_t)p4[k][0] * pw * 1024, dcol);
            s->dv4[k] = rr_m7_round((int64_t)p4[k][1] * ph * 1024, drow);
        }
        if (k < 8) {
            s->du8[k] = rr_m7_round((int64_t)p8[k][0] * pw * 1024, dcol);
            s->dv8[k] = rr_m7_round((int64_t)p8[k][1] * ph * 1024, drow);
        }
        s->du16[k] = rr_m7_round((int64_t)p16[k][0] * pw * 1024, dcol);
        s->dv16[k] = rr_m7_round((int64_t)p16[k][1] * ph * 1024, drow);
    }
    for (X = 0; X < ow; X++) {
        int32_t *c = cols + 3 * (size_t)X;
        int tx;
        float fx = rr_sharp(((float)X + 0.5f) * dx, dx, &tx);
        c[0] = tx;
        memcpy(&c[1], &fx, 4);
        /* u = (X + 1/2) pw / ow / cpp - 1/2 - left */
        c[2] = rr_m7_round((int64_t)(16 * X + 8) * pw * 1024 - (int64_t)(512 + 1024 * m->left) * dcol, dcol);
    }
    for (Y = 0; Y < oh; Y++) {
        int32_t *r = rows + 4 * (size_t)Y;
        int ty, a, b;
        float fy = rr_sharp(((float)Y + 0.5f) * dy, dy, &ty);
        a = ty < 0 ? 0 : ty >= ph ? ph - 1 : ty;
        b = ty + 1 < 0 ? 0 : ty + 1 >= ph ? ph - 1 : ty + 1;
        r[0] = ty;
        memcpy(&r[1], &fy, 4);
        r[2] = m->row_m7[a] || (fy > 0 && m->row_m7[b]);
        /* v = (Y + 1/2) ph / oh - 1/2 */
        r[3] = rr_m7_round((int64_t)(16 * Y + 8) * ph * 1024 - (int64_t)512 * drow, drow);
    }
}

/* Mode 7's pixel byte at texel (px, py) of VRAM v (ppu.c m7_lookup): 0
   where the wrap mode leaves it clear */
static inline unsigned rr_m7_texel(const uint16_t *v, unsigned sel, int px, int py)
{
    unsigned tile;
    if ((px | py) & ~0x3FF) {
        if ((sel & 0xC0) == 0x80)
            return 0;
        if ((sel & 0xC0) == 0xC0)
            tile = 0;
        else
            tile = v[((py >> 3 & 127) << 7 | (px >> 3 & 127)) & 0x7FFF] & 0xFF;
    } else {
        tile = v[((py >> 3) << 7 | (px >> 3)) & 0x7FFF] & 0xFF;
    }
    return v[(tile * 64 + (unsigned)(py & 7) * 8 + (unsigned)(px & 7)) & 0x7FFF] >> 8;
}

/* VRAM v's plane (RrMode7.plane): every texel inside the map */
static inline void rr_m7_plane(uint8_t *plane, const uint16_t *v)
{
    int tx, ty, y, x;
    for (ty = 0; ty < 128; ty++)
        for (tx = 0; tx < 128; tx++) {
            const uint16_t *tile = v + (size_t)(v[ty << 7 | tx] & 0xFF) * 64;
            for (y = 0; y < 8; y++) {
                uint8_t *o = plane + (size_t)(ty * 8 + y) * 1024 + (size_t)tx * 8;
                for (x = 0; x < 8; x++)
                    o[x] = (uint8_t)(tile[y * 8 + x] >> 8);
            }
        }
}

/* the same texel from the plane: one read inside the map or where the map
   repeats; tile 0 from VRAM where the wrap mode fills with it */
static inline unsigned rr_m7_plane_texel(const uint8_t *plane, const uint16_t *v, unsigned sel, int px, int py)
{
    if ((px | py) & ~0x3FF) {
        if ((sel & 0xC0) == 0x80)
            return 0;
        if ((sel & 0xC0) == 0xC0)
            return v[(unsigned)(py & 7) * 8 + (unsigned)(px & 7)] >> 8;
    }
    return plane[(size_t)(py & 1023) * 1024 + (size_t)(px & 1023)];
}

static inline uint16_t rr_m7_direct(unsigned c)
{
    return (uint16_t)((c & 7u) << 2 | ((c & 0x38u) >> 1) << 5 | ((c & 0xC0u) >> 3) << 10);
}

/* one screen's colour out of it and the other (ppu.c finish), 0xFFRRGGBB */
static inline uint32_t rr_m7_finish(unsigned mc, unsigned ml, unsigned sc, unsigned sl, unsigned flags, unsigned adsub,
                                    unsigned wsel, unsigned fixed, unsigned bright)
{
    int clip = (flags & RR_M7_CLIP) != 0, prevent = (flags & RR_M7_PREVENT) != 0;
    unsigned bit = ml == 5 ? 5 : ml == 6 ? 8 : ml;
    unsigned c = clip ? 0 : mc, r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    if (bit < 8 && (adsub >> bit & 1) && !prevent) {
        unsigned o;
        int halve = (adsub & 0x40) != 0 && !clip;
        if (wsel & 2) {
            if (sl == 5) {
                o = fixed;
                halve = 0;
            } else {
                o = sc;
            }
        } else {
            o = fixed;
        }
        if (adsub & 0x80) {
            int rr = (int)r - (int)(o & 31), gg = (int)g - (int)((o >> 5) & 31), bb = (int)b - (int)((o >> 10) & 31);
            r = rr < 0 ? 0 : (unsigned)rr;
            g = gg < 0 ? 0 : (unsigned)gg;
            b = bb < 0 ? 0 : (unsigned)bb;
            if (halve) {
                r >>= 1;
                g >>= 1;
                b >>= 1;
            }
        } else {
            r += o & 31;
            g += (o >> 5) & 31;
            b += (o >> 10) & 31;
            if (halve) {
                r >>= 1;
                g >>= 1;
                b >>= 1;
            }
            if (r > 31) r = 31;
            if (g > 31) g = 31;
            if (b > 31) b = 31;
        }
    }
    if (bright != 15) {
        /* (x + 7) / 15 for x up to 472, as (x + 7) 4370 >> 16 (exact there:
           no division, for the shaders) */
        r = ((r * bright + 7u) * 4370u) >> 16;
        g = ((g * bright + 7u) * 4370u) >> 16;
        b = ((b * bright + 7u) * 4370u) >> 16;
    }
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* A picture pixel's colour with Mode 7's pixel byte pix (both layers
   read it, but a layer under mosaic its block's, fix): the console's rules
   for that pixel */
static inline uint32_t rr_m7_compose(const RrMode7 *m, const RrM7Px *p, const RrM7Seg *g, unsigned pix)
{
    unsigned flags = p->w2 & 0xFFFFu, pix1 = pix, pix2 = pix;
    unsigned mc = p->w0 & 0xFFFF, sc = p->w0 >> 16, mz = p->w1 & 0xFF, sz = (p->w1 >> 8) & 0xFF;
    unsigned ml = (p->w1 >> 16) & 0xFF, sl = p->w1 >> 24;
    unsigned adsub = ((unsigned)g->s[4] >> 16) & 0xFF, wsel = (unsigned)g->s[4] >> 24;
    unsigned fixed = (unsigned)g->s[5] & 0xFFFF, bright = ((unsigned)g->s[5] >> 16) & 0xFF;
    const uint16_t *pal = m->pal + (size_t)g->s[6] * 256u;
    if (flags & RR_M7_BG1_FIX)
        pix1 = p->w3 & 0xFF;
    if (flags & RR_M7_BG2_FIX)
        pix2 = (p->w3 >> 8) & 0xFF;
    /* BG1 at depth 3, then BG2 at 1 or 5 by its bit 7 (ppu.c z7) */
    if (pix1) {
        unsigned col = (g->s[4] >> 8 & 1) ? rr_m7_direct(pix1) : pal[pix1];
        if ((flags & RR_M7_BG1_MAIN) && 3 > mz)
            mc = col, mz = 3, ml = 0;
        if ((flags & RR_M7_BG1_SUB) && 3 > sz)
            sc = col, sz = 3, sl = 0;
    }
    if (pix2 & 0x7F) {
        unsigned col = pal[pix2 & 0x7F], z = (pix2 & 0x80) ? 5 : 1;
        if ((flags & RR_M7_BG2_MAIN) && z > mz)
            mc = col, mz = z, ml = 1;
        if ((flags & RR_M7_BG2_SUB) && z > sz)
            sc = col, sz = z, sl = 1;
    }
    if (flags & RR_M7_SWAP) {
        /* a hires line's even half pixel: the sub screen's colour, over the
           main screen's */
        unsigned t = mc;
        mc = sl == 5 ? pal[0] : sc;
        sc = t;
        t = ml;
        ml = sl;
        sl = t;
    }
    return rr_m7_finish(mc, ml, sc, sl, flags, adsub, wsel, fixed, bright);
}

/* direct colour: a pixel byte and its tile's palette bits (ppu.c direct_color) */
static inline uint16_t rr_cv_direct(unsigned c, unsigned pal)
{
    unsigned r = (c & 7u) << 2 | (pal & 1u) << 1, g = (c & 0x38u) >> 1 | (pal & 2u), b = (c & 0xC0u) >> 3 | (pal & 4u);
    return (uint16_t)(r | g << 5 | b << 10);
}

/* A canvas pixel's colour with the canvas's texel pix: its layer's colour
   by its colour depth (ppu.c draw_bg), at the layer's depth on each screen
   it may show on, then the console's rules (finish) */
static inline uint32_t rr_cv_compose(const RrMode7 *m, const RrM7Px *p, const RrM7Seg *g, unsigned pix)
{
    unsigned flags = p->w2 & 0xFFFFu, w3 = p->w3;
    unsigned mc = p->w0 & 0xFFFF, sc = p->w0 >> 16, mz = p->w1 & 0xFF, sz = (p->w1 >> 8) & 0xFF;
    unsigned ml = (p->w1 >> 16) & 0xFF, sl = p->w1 >> 24;
    unsigned adsub = ((unsigned)g->s[4] >> 16) & 0xFF, wsel = (unsigned)g->s[4] >> 24;
    unsigned fixed = (unsigned)g->s[5] & 0xFFFF, bright = ((unsigned)g->s[5] >> 16) & 0xFF;
    unsigned bpp = (unsigned)g->s[7] & 0xFF, mode = ((unsigned)g->s[7] >> 8) & 7;
    unsigned pal = (w3 >> 22) & 7, z = (w3 >> 25) & 15, n = (w3 >> 29) & 3;
    const uint16_t *cg = m->pal + (size_t)g->s[6] * 256u;
    pix &= (1u << bpp) - 1u;
    if (pix) {
        unsigned col;
        if (bpp == 8)
            col = (g->s[4] >> 8 & 1) ? rr_cv_direct(pix, pal) : cg[pix];
        else if (bpp == 4)
            col = cg[pal * 16 + pix];
        else
            col = cg[(mode == 0 ? n * 32 : 0) + pal * 4 + pix];
        if ((flags & RR_M7_BG1_MAIN) && z > mz)
            mc = col, mz = z, ml = n;
        if ((flags & RR_M7_BG1_SUB) && z > sz)
            sc = col, sz = z, sl = n;
    }
    return rr_m7_finish(mc, ml, sc, sl, flags, adsub, wsel, fixed, bright);
}

/* Canvas pixel (i, r) over the output pixel whose centre is at screen
   column u and picture row v (1/1024ths, rr_m7_tables): the canvas's texel
   under the centre. The centre's place in the console pixel, 0 to 1023
   across and down, picks one of the pixel's k x k texels (mirrored where
   the tile is flipped); the console pixel's own centre the texel k / 2
   across and down (ppu.c cv_centre). */
static inline uint32_t rr_cv_q(const RrMode7 *m, const RrM7Px *p, const RrM7Seg *g, int i, int r, int32_t u, int32_t v)
{
    unsigned w3 = p->w3, k = (unsigned)m->cv_k, sx, sy;
    int32_t h = i / m->cpp - m->left, fu = u - 1024 * h + 512, fv = v - 1024 * r + 512;
    size_t at;
    fu = fu < 0 ? 0 : fu > 1023 ? 1023 : fu;
    fv = fv < 0 ? 0 : fv > 1023 ? 1023 : fv;
    sx = ((unsigned)fu * k) >> 10;
    sy = ((unsigned)fv * k) >> 10;
    if (w3 >> 20 & 1)
        sx = k - 1 - sx;
    if (w3 >> 21 & 1)
        sy = k - 1 - sy;
    if (w3 >> 31) {
        /* a screen canvas: by the screen's pixel */
        size_t col = (size_t)(i / m->cpp), ex = col * k + sx, ey = (size_t)r * k + sy;
        return rr_cv_compose(m, p, g, ex < (size_t)m->cv_w && ey < (size_t)m->cv_h ? m->cv[ey * (size_t)m->cv_w + ex] : 0u);
    }
    at = (size_t)(w3 & 0x3FFF) * 64u * k * k + ((size_t)(w3 >> 17 & 7) * k + sy) * 8u * k + (size_t)(w3 >> 14 & 7) * k + sx;
    return rr_cv_compose(m, p, g, at < m->cv_len ? m->cv[at] : 0u);
}

/* The weight (1/256ths) of the next texel along an axis: f the position
   past the texel's centre (1/256 texel, 0-255), the output pixel's
   footprint along the axis the parallelogram of spans sa and sb (1/256
   texel: the matrix's two terms, sa + sb at most a texel). The part of the
   footprint past the texel's edge: the footprint's extent along the axis
   is the sum of two uniform spans, so the part is piecewise quadratic in
   the edge's distance t = 128 - f from the centre, exactly (sharp bilinear
   on the plane, exact for a turned plane as for a square one). In
   doubled units T = 2 t, with A >= B the spans:
     T <= -(A + B)       256
     T <= -(A - B)       256 - 256 (T + A + B)^2 / 8 A B
     T <   A - B         128 - 128 T / A
     T <   A + B         256 (A + B - T)^2 / 8 A B
     else                0
   (B = 0: the linear part alone; A = 0: a step, half at the edge). */
static inline int32_t rr_m7_edge(int32_t f, int32_t sa, int32_t sb)
{
    int32_t A = sa > sb ? sa : sb, B = sa > sb ? sb : sa, T = 2 * (128 - f), cov;
    if (A == 0)
        return T < 0 ? 256 : T > 0 ? 0 : 128;
    if (T <= -(A + B))
        cov = 0;
    else if (B > 0 && T <= -(A - B))
        cov = (int32_t)rr_m7_floordiv((int64_t)256 * (T + A + B) * (T + A + B), (int64_t)8 * A * B);
    else if (T < A - B || B == 0)
        cov = 128 + (int32_t)rr_m7_floordiv((int64_t)128 * T, A);
    else if (T < A + B)
        cov = 256 - (int32_t)rr_m7_floordiv((int64_t)256 * (A + B - T) * (A + B - T), (int64_t)8 * A * B);
    else
        cov = 256;
    cov = cov < 0 ? 0 : cov > 256 ? 256 : cov;
    return 256 - cov;   /* cov: the part on this texel's side */
}

/* Picture pixel (i, r)'s colour over the output pixel whose centre is at
   screen column u and picture row v (1/1024ths, rr_m7_tables): its Mode 7
   plane filtered by the pixel's footprint */
static inline uint32_t rr_m7_q(const RrMode7 *m, const RrM7Samples *s, int i, int r, int32_t u, int32_t v)
{
    const RrM7Px *p;
    const RrM7Seg *g;
    const uint16_t *vr;
    const uint8_t *pl;
    unsigned flags, sel;
    int32_t a, b, c, d, uu, ee, sx = 1, sy = 1, hx, lx, hy, ly, ax, bx, ay, by, wx, wy, w;
    i = i < 0 ? 0 : i >= m->pw ? m->pw - 1 : i;
    r = r < 0 ? 0 : r >= m->ph ? m->ph - 1 : r;
    p = &m->px[(size_t)r * (size_t)m->pw + (size_t)i];
    flags = p->w2 & 0xFFFFu;
    if (flags & RR_M7_NOT)
        return m->pic[(size_t)r * (size_t)m->pw + (size_t)i];
    if (flags & RR_M7_BLACK)
        return 0xFF000000u;
    g = &m->seg[p->w2 >> 16];
    if (flags & RR_CV)
        return rr_cv_q(m, p, g, i, r, u, v);
    if (!(flags & (RR_M7_BG1_MAIN | RR_M7_BG1_SUB | RR_M7_BG2_MAIN | RR_M7_BG2_SUB)) ||
        (flags & (RR_M7_BG1_FIX | RR_M7_BG2_FIX)) == (RR_M7_BG1_FIX | RR_M7_BG2_FIX))
        return rr_m7_compose(m, p, g, 0);    /* no texel of the plane shows here */
    sel = (unsigned)g->s[4] & 0xFF;
    vr = m->vram + (size_t)((unsigned)g->s[5] >> 24) * 0x8000u;
    pl = m->plane + (size_t)((unsigned)g->s[5] >> 24) * 0x100000u;
    a = (int16_t)(g->s[2] & 0xFFFF);
    b = (int16_t)(g->s[2] >> 16);
    c = (int16_t)(g->s[3] & 0xFFFF);
    d = (int16_t)(g->s[3] >> 16);
    /* the matrix at the centre: (ox << 10) + a u + b e, e the rows below
       the line's own sample, held as 1024 hx + lx (in 32 bits), the plane's
       position in 1/256 texel hx + (lx >> 10); an offset du, dv from the
       centre adds a du + b dv to lx */
    uu = u;
    ee = v - 1024 * r;
    if (sel & 1) {
        uu = 255 * 1024 - uu;
        sx = -1;
    }
    if (sel & 2) {
        ee = -ee;
        sy = -1;
    }
    hx = g->s[0] + a * (uu >> 10) + b * (ee >> 10);
    lx = a * (uu & 1023) + b * (ee & 1023);
    hy = g->s[1] + c * (uu >> 10) + d * (ee >> 10);
    ly = c * (uu & 1023) + d * (ee & 1023);
    /* the footprint: an output pixel's spans along each axis of the plane,
       its two terms (1/256 texel) */
    ax = (int32_t)(((int64_t)(a < 0 ? -a : a) * s->step_u) >> 10);
    bx = (int32_t)(((int64_t)(b < 0 ? -b : b) * s->step_v) >> 10);
    ay = (int32_t)(((int64_t)(c < 0 ? -c : c) * s->step_u) >> 10);
    by = (int32_t)(((int64_t)(d < 0 ? -d : d) * s->step_v) >> 10);
    wx = ax + bx;
    wy = ay + by;
    w = wx > wy ? wx : wy;
    if (w <= 256) {
        /* the texel's edges covered: the texel under the centre and, where
           an edge is within half an output pixel, the next */
        int32_t qx = hx + (lx >> 10) - 128, qy = hy + (ly >> 10) - 128;
        int32_t ix = qx >> 8, iy = qy >> 8, ex = rr_m7_edge(qx & 255, ax, bx), ey = rr_m7_edge(qy & 255, ay, by);
        uint32_t c00, c01, c10, c11;
        float fx = (float)ex / 256.0f, fy = (float)ey / 256.0f;
        if (ex == 256) ix++, ex = 0, fx = 0;
        if (ey == 256) iy++, ey = 0, fy = 0;
        c00 = rr_m7_compose(m, p, g, rr_m7_plane_texel(pl, vr, sel, ix, iy));
        c01 = ex ? rr_m7_compose(m, p, g, rr_m7_plane_texel(pl, vr, sel, ix + 1, iy)) : c00;
        if (!ey)
            return rr_mix(c00, c01, fx);
        c10 = rr_m7_compose(m, p, g, rr_m7_plane_texel(pl, vr, sel, ix, iy + 1));
        c11 = ex ? rr_m7_compose(m, p, g, rr_m7_plane_texel(pl, vr, sel, ix + 1, iy + 1)) : c10;
        return rr_mix(rr_mix(c00, c01, fx), rr_mix(c10, c11, fx), fy);
    }
    {
        /* supersampled: each sample the texel under it */
        int n = w <= 512 ? 4 : w <= 1024 ? 8 : 16, k;
        const int32_t *du = n == 4 ? s->du4 : n == 8 ? s->du8 : s->du16, *dv = n == 4 ? s->dv4 : n == 8 ? s->dv8 : s->dv16;
        unsigned sum[3] = { 0, 0, 0 }, last = 0x100;
        uint32_t col = 0;
        for (k = 0; k < n; k++) {
            int32_t eu = sx * du[k], ev = sy * dv[k];
            int32_t px = (hx + ((lx + a * eu + b * ev) >> 10)) >> 8, py = (hy + ((ly + c * eu + d * ev) >> 10)) >> 8;
            unsigned pix = rr_m7_plane_texel(pl, vr, sel, px, py);
            if (pix != last) {
                col = rr_m7_compose(m, p, g, pix);
                last = pix;
            }
            sum[0] += col & 0xFF;
            sum[1] += (col >> 8) & 0xFF;
            sum[2] += (col >> 16) & 0xFF;
        }
        return 0xFF000000u | ((sum[2] + (unsigned)n / 2) / (unsigned)n) << 16 | ((sum[1] + (unsigned)n / 2) / (unsigned)n) << 8 |
               ((sum[0] + (unsigned)n / 2) / (unsigned)n);
    }
}

/* Output pixel (X, Y) of the presentation: the picture scaled with sharp
   bilinear where no Mode 7 line is under it (rr_ref_image), else the
   sharp bilinear blend of the picture pixels under it, each its colour
   filtered over the output pixel (rr_m7_q) */
static inline uint32_t rr_m7_pixel(const RrMode7 *m, const RrM7Samples *s, int X, int Y)
{
    const int32_t *col = s->cols + 3 * (size_t)X, *row = s->rows + 4 * (size_t)Y;
    int tx = col[0], ty = row[0];
    int32_t u = col[2], v = row[3];
    float fx, fy;
    uint32_t q00, q01, q10, q11;
    memcpy(&fx, &col[1], 4);
    memcpy(&fy, &row[1], 4);
    if (!row[2]) {
        uint32_t top = rr_mix(rr_texel(m->pic, m->pw, m->ph, tx, ty), rr_texel(m->pic, m->pw, m->ph, tx + 1, ty), fx);
        uint32_t bot = rr_mix(rr_texel(m->pic, m->pw, m->ph, tx, ty + 1), rr_texel(m->pic, m->pw, m->ph, tx + 1, ty + 1), fx);
        return rr_mix(top, bot, fy);
    }
    q00 = rr_m7_q(m, s, tx, ty, u, v);
    q01 = fx > 0 ? rr_m7_q(m, s, tx + 1, ty, u, v) : q00;
    if (!(fy > 0))
        return rr_mix(q00, q01, fx);
    q10 = rr_m7_q(m, s, tx, ty + 1, u, v);
    q11 = fx > 0 ? rr_m7_q(m, s, tx + 1, ty + 1, u, v) : q10;
    return rr_mix(rr_mix(q00, q01, fx), rr_mix(q10, q11, fx), fy);
}

/* The reference: m presented over rectangle d of the tw x th target, s
   where it samples (rr_m7_tables, at d's size) */
static inline void rr_ref_mode7(uint32_t *dst, int tw, int th, const RrMode7 *m, const int d[4], const RrM7Samples *s)
{
    int x, y;
    if (d[2] <= d[0] || d[3] <= d[1])
        return;
    for (y = d[1] < 0 ? 0 : d[1]; y < d[3] && y < th; y++)
        for (x = d[0] < 0 ? 0 : d[0]; x < d[2] && x < tw; x++)
            dst[(size_t)y * (size_t)tw + (size_t)x] = rr_m7_pixel(m, s, x - d[0], y - d[1]);
}

#endif
