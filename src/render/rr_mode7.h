/*
 * rr_mode7.h: a frame with Mode 7 presented at the target's resolution
 * (rr.h rr_mode7), the one definition: the GPU backends' shaders compute it,
 * and the checks' reference on the CPU (tests/ref/rr_ref_mode7.h rr_ref_mode7,
 * ppu_ref.h ppu_present) is it drawn into a buffer.
 *
 * The PPU gives each picture pixel on a Mode 7 line everything that does
 * not depend on which of Mode 7's texels shows there (RrM7Px): the main
 * and sub screens' colour without Mode 7 (the sprites' pixel over the
 * backdrop), whether each Mode 7 layer may show on each screen there (its
 * enable bits and windows), the colour window's regions, and its line's or
 * segment's state (RrM7Seg: the matrix's origin for the line, the matrix,
 * the wrap mode and flips, colour maths, the fixed colour, brightness, its
 * palette and VRAM). A texel's colour there is then the console's rules
 * (tests/ref/ppu_ref.c draw_line, finish) for that one pixel with that texel (the
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

/* A frame for rr_mode7: its picture (w x h, the compositor's: pw x ph), a record
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
    int cv_fx, cv_fy;         /* a screen canvas over the whole screen (k 0): cv_w, cv_h (else 0) */
    int cv_rgb;               /* its texels 32 bits each (nshvc_ext_canvas_screen32; cv_len bytes) */
} RrMode7;

/* A full-colour canvas's colour (nshvc_ext_canvas_screen32) where a colour
   is otherwise 15-bit BGR: RR_RGB | 0xRRGGBB. A texel of such a canvas at
   least RR_RGB is that colour; below it, its low byte an index. */
#define RR_RGB 0x01000000u

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

/* rr_m7_tables's rows' flags alone, for another frame of the same size: all
   else in the tables a function of the sizes and m's cpp and left */
static inline void rr_m7_tables_flags(const RrMode7 *m, int oh, int32_t *rows)
{
    int Y, ph = m->ph;
    for (Y = 0; Y < oh; Y++) {
        int32_t *r = rows + 4 * (size_t)Y;
        int ty = r[0], a = ty < 0 ? 0 : ty >= ph ? ph - 1 : ty, b = ty + 1 < 0 ? 0 : ty + 1 >= ph ? ph - 1 : ty + 1;
        float fy;
        memcpy(&fy, &r[1], 4);
        r[2] = m->row_m7[a] || (fy > 0 && m->row_m7[b]);
    }
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

/* A screen canvas over the whole screen (k 0), fx texels across n pixels
   of it (or down): the texel under the point f / 1024 into pixel c (f
   0-1023; a pixel's centre 512), floor((1024 c + f) fx / 1024 n), in 32
   bits for c < n <= 16384 and fx < 65536 (c fx = q n + e). */
static inline uint32_t rr_cv_fit(uint32_t c, uint32_t f, uint32_t fx, uint32_t n)
{
    uint32_t a = c * fx, q = a / n, e = a - q * n;
    return q + (1024u * e + f * fx) / (1024u * n);
}

#endif
