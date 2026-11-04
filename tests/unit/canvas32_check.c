/*
 * canvas32_check.c: a full-colour screen canvas (nshvc_ext_canvas_screen32,
 * contract 2.13), on a cartridge of a few lines of C that shows scenes of
 * it on a 320 x 240 screen with the console's picture off its centre, BG1
 * the canvas (4 bpp, palette 5) over or under BG2's tiles:
 *
 *   plain      BG1 at priority 0 under BG2's priority-1 tiles, no maths
 *   sub        colour maths subtracting the fixed colour from BG1, the
 *              fixed colour written each line by HDMA ($2132), and the
 *              canvas's CGRAM entry 81 written each line by HDMA too
 *   addhalf    BG1 on the main screen adding BG2 on the sub screen, halved,
 *              the backdrop too, at brightness 9
 *   onsub      the canvas on the sub screen: BG2 and the backdrop adding it
 *   windows    window 1 at screen columns (nshvc_ext_window_columns)
 *              masking BG1; the colour window (window 2, open at the
 *              picture's right edge: nshvc_ext_window_edges) clipping to
 *              black inside, the fixed colour added
 *   wmap       window 1's registers mapped across the screen
 *              (nshvc_ext_window_map) masking BG1; subtracting the fixed
 *              colour, halved
 *
 * Each scene is shown twice, the same frame: once with the canvas's texels
 * as indices (nshvc_ext_canvas_screen) and once as 32-bit texels
 * (nshvc_ext_canvas_screen32): 0 where the index is 0, the index itself
 * for some (and always for entry 81, which HDMA changes each line), and
 * for the rest 0xFF000000 | the CGRAM colour the index names, its 5-bit
 * channels widened to 8 (c << 3 | c >> 2). The 32-bit canvas is copied
 * from a buffer the cartridge scribbles over at once (the frame shows the
 * canvas as it stood). Every picture of the second must be the first's,
 * pixel for pixel:
 *
 *   picture    the console's picture (nshvc_picture)
 *   compose    the C build of the compositor (compose_body.h) from the
 *              frame's description, against the picture
 *   present    the presentation at the window's resolution (rr_ref_mode7)
 *              at one to one, 3x, 2.5x, 3.37 x 2.91 and 0.77 x 0.6
 *   renderer   this build's renderer (soft, Metal, OpenGL or Vulkan): the
 *              presentation from the compositor's records
 *              (rr_compose_mode7) and from the PPU's (rr_mode7) at 3x, and
 *              the compositor's picture (rr_compose) one to one; each also
 *              within 1 a channel of the CPU's
 *
 * Then three scenes of colours no CGRAM entry holds (random 0xFFrrggbb),
 * BG1 alone, each pixel against the colour the contract gives, worked out
 * here: alone, its 8 bits as they are; subtracting the fixed colour f
 * (HDMA's, each line), each channel v as z = max(0, v - v / 32 - 8 f),
 * shown z + z / 32; at brightness 9, z = v - v / 32 to the console's 5
 * bits (z / 8) then the brightness's (q * 9 + 7) / 15, widened. In the
 * picture (each pixel's centre texel), the presentation at 3x (each output
 * pixel one texel) and the renderer's.
 *
 * Each scene also holds what it checks (direct texels, indices and
 * transparent ones shown, the maths and windows changing pixels, HDMA's
 * colours changing by line). Exit 0 when every check holds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "ppu.h"
#include "rr.h"
#include "rr_size.h"
#include "rr_mode7.h"
#include "rr_compose.h"

#define SC_W 320
#define SC_H 240
#define SC_LEFT 20
#define SC_TOP 6
#define K 3
#define PAL 5
#define HDMA_ENTRY 81                  /* PAL * 16 + 1: written each line by HDMA */
#define TW (SC_W * K)                  /* the renderer's target: the 3x presentation */
#define TH (SC_H * K)
#define TEX_W (SC_W * K)
#define TEX_H (SC_H * K)

enum { S_PLAIN, S_SUB, S_ADDHALF, S_ONSUB, S_WINDOWS, S_WMAP, S_EXACT, S_EXACTSUB, S_EXACTBRIGHT, NSCENES };
static const char *const scene_name[NSCENES] = { "plain", "sub", "addhalf", "onsub", "windows", "wmap",
                                                 "exact", "exact sub", "exact bright" };
#define EQUAL_SCENES 6                 /* 0-5 shown twice, indices then 32-bit */

static int fails;
static long compared;

/* ---- the cartridge ---- */
static unsigned rng = 9173;
static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}
static void w(unsigned reg, unsigned v) { wr8(0x2100 + reg, (uint8_t)v); }

static uint16_t cg[256];               /* CGRAM as the cartridge wrote it */
static uint8_t *ix;                    /* the scene's texels as indices */
static uint32_t *tx, *shared;          /* as 32 bits; the buffer handed over */

static uint32_t widen(unsigned c)
{
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* HDMA tables in work RAM: channel 0 COLDATA's red and green (mode 2),
   channel 2 its blue (mode 0), channel 1 CGRAM entry 81 (mode 3 to $2121:
   the address twice, the colour's two bytes) */
static void hdma_tables(void)
{
    uint32_t a = 0x7E2000, b = 0x7E2400, c = 0x7E2800;
    int half, i;
    for (half = 0; half < 2; half++) {
        wr8(a++, 0xF0);
        wr8(b++, 0xF0);
        wr8(c++, 0xF0);
        for (i = 0; i < 112; i++) {
            unsigned line = (unsigned)(half * 112 + i);
            uint16_t col = (uint16_t)((line * 1237u + 77u) & 0x7FFF);
            wr8(a++, (uint8_t)(0x20 | ((line * 5u + 3u) & 31)));
            wr8(a++, (uint8_t)(0x40 | ((line * 11u + 7u) & 31)));
            wr8(c++, (uint8_t)(0x80 | ((line * 3u + 20u) & 31)));
            wr8(b++, HDMA_ENTRY);
            wr8(b++, HDMA_ENTRY);
            wr8(b++, (uint8_t)(col & 0xFF));
            wr8(b++, (uint8_t)(col >> 8));
        }
    }
    wr8(a, 0);
    wr8(b, 0);
    wr8(c, 0);
}

static void hdma_on(int coldata, int cgram)
{
    static const uint8_t ch[3][5] = { { 0x02, 0x32, 0x00, 0x20, 0x7E }, { 0x03, 0x21, 0x00, 0x24, 0x7E },
                                      { 0x00, 0x32, 0x00, 0x28, 0x7E } };
    int c, i;
    for (c = 0; c < 3; c++)
        for (i = 0; i < 5; i++)
            wr8(0x004300 + (uint32_t)c * 16 + (uint32_t)i, ch[c][i]);
    wr8(0x00420C, (uint8_t)((coldata ? 0x05 : 0) | (cgram ? 0x02 : 0)));
}

static void setup(int sc)
{
    int i;
    /* the last scene's HDMA off first: its CGRAM channel writes CGADD each
       line */
    hdma_on(0, 0);
    nshvc_ext_screen(SC_W, SC_H);
    nshvc_ext_screen_origin(SC_LEFT, SC_TOP);
    /* VRAM: random words, BG2's map at $0800 random characters, flips,
       palettes and priorities; BG2's characters at $4000 */
    w(0x15, 0x80);
    w(0x16, 0);
    w(0x17, 0);
    for (i = 0; i < 0x8000; i++) {
        unsigned v = rnd() & 0xFFFF;
        if (i >= 0x0800 && i < 0x0C00)
            v = (rnd() % 256u) | (rnd() & 0xFC00u);
        w(0x18, v & 0xFF);
        w(0x19, v >> 8);
    }
    w(0x21, 0);
    for (i = 0; i < 256; i++) {
        cg[i] = (uint16_t)(rnd() & 0x7FFF);
        w(0x22, cg[i] & 0xFF);
        w(0x22, cg[i] >> 8);
    }
    w(0x05, 0x01);
    w(0x06, 0x00);
    w(0x07, 0x00);
    w(0x08, 0x08);
    w(0x0B, 0x40);
    for (i = 0; i < 4; i++) {
        w(0x0D + 2 * (unsigned)i, (unsigned)(i * 13));
        w(0x0D + 2 * (unsigned)i, 0);
        w(0x0E + 2 * (unsigned)i, (unsigned)(i * 29));
        w(0x0E + 2 * (unsigned)i, 0);
    }
    for (i = 0x23; i <= 0x31; i++)
        w((unsigned)i, 0);
    w(0x32, 0xE0);
    w(0x33, 0);
    nshvc_ext_window_edges(0);
    nshvc_ext_window_map(0, 65536, 0);
    nshvc_ext_window_columns(1, -1, 0);
    nshvc_ext_window_columns(2, -1, 0);
    hdma_on(0, 0);
    switch (sc) {
    case S_PLAIN:
        w(0x2C, 0x03);
        break;
    case S_SUB:
    case S_EXACTSUB:
        w(0x2C, sc == S_SUB ? 0x03 : 0x01);
        w(0x31, 0x81);
        hdma_on(1, sc == S_SUB);
        break;
    case S_ADDHALF:
        w(0x2C, 0x01);
        w(0x2D, 0x02);
        w(0x30, 0x02);
        w(0x31, 0x61);
        break;
    case S_ONSUB:
        w(0x2C, 0x02);
        w(0x2D, 0x01);
        w(0x30, 0x02);
        w(0x31, 0x22);
        w(0x32, 0x20 | 9);
        w(0x32, 0x40 | 3);
        w(0x32, 0x80 | 14);
        break;
    case S_WINDOWS:
        w(0x2C, 0x03);
        w(0x2E, 0x01);                 /* window masking BG1 on the main screen */
        w(0x23, 0x02);                 /* window 1 on BG1 */
        nshvc_ext_window_columns(1, 30, 120);
        w(0x28, 200);                  /* window 2: 200-255, open past the right edge */
        w(0x29, 255);
        nshvc_ext_window_edges(1);
        w(0x25, 0x80);                 /* window 2 the colour window */
        w(0x30, 0x80);                 /* clip to black inside it */
        w(0x31, 0x01);                 /* BG1 adds the fixed colour */
        w(0x32, 0x20 | 4);
        w(0x32, 0x40 | 2);
        w(0x32, 0x80 | 8);
        break;
    case S_WMAP:
        w(0x2C, 0x03);
        w(0x2E, 0x01);
        w(0x23, 0x02);
        w(0x26, 40);
        w(0x27, 150);
        nshvc_ext_window_map(1, 81920, -12);   /* 1.25 a console column */
        w(0x31, 0xC1);                 /* BG1 subtracts the fixed colour, halved */
        w(0x32, 0x20 | 6);
        w(0x32, 0x40 | 1);
        w(0x32, 0x80 | 17);
        break;
    default:
        w(0x2C, 0x01);
        break;
    }
}

/* the scene's texels: indices and their 32-bit forms, or (the exact
   scenes) random colours */
static void make_texels(int sc)
{
    size_t i, n = (size_t)TEX_W * TEX_H;
    for (i = 0; i < n; i++) {
        unsigned v = rnd() & 15;
        if ((rnd() & 3) == 0)
            v = 0;
        ix[i] = (uint8_t)v;
        if (sc >= EQUAL_SCENES)
            tx[i] = v ? 0xFF000000u | (rnd() & 0xFFFFFFu) : 0u;
        else if (!v)
            tx[i] = 0;
        else if (PAL * 16 + v == HDMA_ENTRY || (rnd() & 3) == 0)
            tx[i] = v;
        else
            tx[i] = 0xFF000000u | widen(cg[PAL * 16 + v]);
    }
}

static void show(uint8_t id)
{
    wr8(0x7E0100, 0);
    nshvc_wait_vblank();               /* a frame the canvas began with */
    wr8(0x7E0100, id);
    nshvc_wait_vblank();               /* the scene's whole frame */
    wr8(0x7E0100, 0);
}

static void game(void)
{
    int sc;
    wr8(0x004200, 0x00);
    hdma_tables();
    for (sc = 0; sc < NSCENES; sc++) {
        int prio = sc == S_PLAIN ? 0 : 1;
        w(0x00, 0x80);
        setup(sc);
        make_texels(sc);
        if (sc < EQUAL_SCENES) {
            nshvc_ext_canvas_screen(0, ix, TEX_W, TEX_H, K, prio, PAL);
            w(0x00, sc == S_ADDHALF ? 0x09 : 0x0F);
            show((uint8_t)(2 * sc + 1));
        }
        /* the 32-bit canvas from a buffer scribbled over once handed over */
        memcpy(shared, tx, (size_t)TEX_W * TEX_H * 4);
        nshvc_ext_canvas_screen32(0, shared, TEX_W, TEX_H, K, prio, PAL);
        memset(shared, 0xA5, (size_t)TEX_W * TEX_H * 4);
        w(0x00, sc == S_ADDHALF ? 0x09 : sc == S_EXACTBRIGHT ? 0x09 : 0x0F);
        show((uint8_t)(2 * sc + 2));
    }
    wr8(0x7E0100, 0xFF);
    for (;;)
        nshvc_wait_vblank();
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, game, "reset", 0 } };

/* ---- the checks ---- */
static void report(const char *what, int sc, long bad, long n)
{
    compared += n;
    printf("%s %-12s %-52s: %ld of %ld differ\n", bad ? "FAIL" : "ok  ", scene_name[sc], what, bad, n);
    if (bad)
        fails++;
}

static void holds(int sc, const char *what, int ok)
{
    printf("%s %-12s holds: %s\n", ok ? "ok  " : "FAIL", scene_name[sc], what);
    fails += !ok;
}

static int chan_diff(uint32_t a, uint32_t b)
{
    int sh, worst = 0;
    for (sh = 0; sh < 24; sh += 8) {
        int d = (int)((a >> sh) & 0xFF) - (int)((b >> sh) & 0xFF);
        if (d < 0)
            d = -d;
        if (d > worst)
            worst = d;
    }
    return worst;
}

/* n pixels of got against want: exactly (tol 0) or within tol a channel */
static long differ(const uint32_t *got, const uint32_t *want, size_t n, int tol)
{
    long bad = 0;
    size_t i;
    for (i = 0; i < n; i++)
        if (tol ? chan_diff(got[i], want[i]) > tol : got[i] != want[i]) {
            if (!bad)
                printf("  first at %zu: got %08x want %08x\n", i, got[i], want[i]);
            bad++;
        }
    return bad;
}

static const double sizes[5][2] = { { 1, 1 }, { 3, 3 }, { 2.5, 2.5 }, { 3.37, 2.91 }, { 0.77, 0.6 } };

/* everything a frame shows, kept from the indexed frame to hold the 32-bit
   one to it */
typedef struct {
    uint32_t *pic, *pres[5], *gpu[3];
    int have[3];
} Shot;
static Shot kept;
static int gpu_open;
static int32_t gcols[TW * 3], grows[TH * 4];

static int sz_w(int si) { return (int)(SC_W * sizes[si][0] + 0.5); }
static int sz_h(int si) { return (int)(SC_H * sizes[si][1] + 0.5); }

/* the renderer: kind 0 the presentation at 3x from the compositor's
   records, 1 from the PPU's, 2 the compositor's picture one to one; 0
   when the renderer has none (the soft one's compositor) */
static int gpu_draw(int kind, const PpuFrame *f, const uint32_t *pic, uint32_t *out)
{
    static uint32_t *buf;
    int tw, th, d[4] = { 0, 0, TW, TH }, ok = 1, y;
    RrMode7 m;
    RrM7Samples smp;
    size_t nw;
    const uint32_t *desc;
    rr_target(&tw, &th);
    if (tw < TW || th < TH) {
        printf("FAIL the renderer's target %dx%d, not at least %dx%d\n", tw, th, TW, TH);
        fails++;
        return 0;
    }
    if (!buf)
        buf = malloc((size_t)tw * (size_t)th * 4);
    rr_begin(0xFF000000u);
    if (kind == 0) {
        desc = ppu_compose_m7(f, &nw, &m);
        rr_m7_tables(&m, TW, TH, &smp, gcols, grows);
        ok = rr_compose_mode7(desc, nw, &m, d, &smp);
    } else if (kind == 1) {
        if (!ppu_mode7(f, pic, &m))
            return 0;
        rr_m7_tables(&m, TW, TH, &smp, gcols, grows);
        rr_mode7(&m, d, &smp);
    } else {
        d[2] = SC_W;
        d[3] = SC_H;
        desc = ppu_compose(f, &nw);
        ok = rr_compose(desc, nw, SC_W, SC_H, d);
    }
    if (!ok)
        return 0;
    rr_present();
    if (!rr_read(buf))
        return 0;
    for (y = 0; y < d[3]; y++)
        memcpy(out + (size_t)y * (size_t)d[2], buf + (size_t)y * (size_t)tw, (size_t)d[2] * 4);
    return 1;
}

static const char *const gpu_what[3] = { "renderer: compositor's records, 3x", "renderer: PPU's records, 3x",
                                         "renderer: compositor's picture" };

/* the frame's pictures into s, each also checked against the CPU's own */
static void shoot(int sc, Shot *s, const char *tag)
{
    const PpuFrame *f = ppu_frame();
    int pw, ph, si, kind;
    const uint32_t *pic = ppu_picture(&pw, &ph);
    char what[96];
    if (pw != SC_W || ph != SC_H) {
        printf("FAIL %s: a %dx%d picture\n", scene_name[sc], pw, ph);
        fails++;
        return;
    }
    memcpy(s->pic, pic, (size_t)pw * ph * 4);
    {
        /* the C compositor against the picture */
        size_t nw;
        uint32_t x, y;
        long bad = 0;
        rr_cp_fd = ppu_compose(f, &nw);
        for (y = 0; y < (uint32_t)ph; y++)
            for (x = 0; x < (uint32_t)pw; x++)
                if (rr_cp_pixel(x, y) != pic[y * (uint32_t)pw + x]) {
                    if (!bad)
                        printf("  first at (%u, %u): got %08x want %08x\n", x, y, rr_cp_pixel(x, y), pic[y * (uint32_t)pw + x]);
                    bad++;
                }
        snprintf(what, sizeof what, "%s: compose (C) against the picture", tag);
        report(what, sc, bad, (long)pw * ph);
    }
    for (si = 0; si < 5; si++)
        ppu_present(f, pic, s->pres[si], sz_w(si), sz_h(si));
    for (kind = 0; kind < 3; kind++) {
        s->have[kind] = gpu_open && gpu_draw(kind, f, pic, s->gpu[kind]);
        if (s->have[kind]) {
            /* within 1 a channel of the CPU's (the GPU's blends round) */
            const uint32_t *want = kind == 2 ? s->pic : s->pres[1];
            size_t n = kind == 2 ? (size_t)SC_W * SC_H : (size_t)TW * TH;
            snprintf(what, sizeof what, "%s: %s against the CPU's", tag, gpu_what[kind]);
            report(what, sc, differ(s->gpu[kind], want, n, 1), (long)n);
        }
    }
}

/* the 32-bit frame against the indexed one, pixel for pixel */
static void against_kept(int sc, const Shot *s)
{
    int si, kind;
    char what[96];
    report("picture: 32-bit against indices", sc, differ(s->pic, kept.pic, (size_t)SC_W * SC_H, 0), (long)SC_W * SC_H);
    for (si = 0; si < 5; si++) {
        size_t n = (size_t)sz_w(si) * (size_t)sz_h(si);
        snprintf(what, sizeof what, "present x%.2fx%.2f: 32-bit against indices", sizes[si][0], sizes[si][1]);
        report(what, sc, differ(s->pres[si], kept.pres[si], n, 0), (long)n);
    }
    for (kind = 0; kind < 3; kind++) {
        size_t n = kind == 2 ? (size_t)SC_W * SC_H : (size_t)TW * TH;
        if (s->have[kind] != kept.have[kind]) {
            printf("FAIL %s %s: drawn once of the two\n", scene_name[sc], gpu_what[kind]);
            fails++;
            continue;
        }
        if (!s->have[kind]) {
            printf("n/a  %-12s %s: none in %s\n", scene_name[sc], gpu_what[kind], rr_name());
            continue;
        }
        snprintf(what, sizeof what, "%s: 32-bit against indices", gpu_what[kind]);
        report(what, sc, differ(s->gpu[kind], kept.gpu[kind], n, 0), (long)n);
    }
}

/* the texel under console pixel (h, r)'s centre, and the line whose state
   a row takes */
static uint32_t centre(int h, int r) { return tx[(size_t)(r * K + K / 2) * TEX_W + (size_t)(h * K + K / 2)]; }
static int row_line(const PpuFrame *f, int r)
{
    int line = r - f->top + 1;
    return line < 1 ? 1 : line > f->height ? f->height : line;
}

/* the colour the contract gives a texel t (32 bits, not 0) of the exact
   scenes on row r, and a transparent one's (the backdrop, CGRAM 0) */
static uint32_t exact_want(int sc, const PpuFrame *f, int r, uint32_t t)
{
    const PpuScreen *st = &f->line[row_line(f, r)];
    unsigned fix[3] = { st->fixed_r, st->fixed_g, st->fixed_b }, k, out = 0xFF000000u;
    if (!t) {
        unsigned c = f->palette[row_line(f, r)][0];
        if (sc == S_EXACTBRIGHT) {
            unsigned ch[3] = { c & 31, (c >> 5) & 31, (c >> 10) & 31 };
            for (k = 0; k < 3; k++)
                ch[k] = (ch[k] * 9 + 7) / 15;
            c = ch[0] | ch[1] << 5 | ch[2] << 10;
        }
        return 0xFF000000u | widen(c);
    }
    if (sc == S_EXACT)
        return 0xFF000000u | (t & 0xFFFFFFu);
    for (k = 0; k < 3; k++) {
        unsigned v = (t >> (16 - 8 * k)) & 0xFF, z = v - v / 32, q;
        if (sc == S_EXACTSUB) {
            z = z > 8 * fix[k] ? z - 8 * fix[k] : 0;
        } else {
            q = (z / 8 * 9 + 7) / 15;
            z = 8 * q;
        }
        out |= (z + z / 32) << (16 - 8 * k);
    }
    return out;
}

static void check_exact(int sc, const Shot *s)
{
    const PpuFrame *f = ppu_frame();
    int h, r, X, Y, kind;
    long bad = 0, n = 0, kept8 = 0;
    for (r = 0; r < SC_H; r++)
        for (h = 0; h < SC_W; h++) {
            uint32_t want = exact_want(sc, f, r, centre(h, r));
            n++;
            if (s->pic[(size_t)r * SC_W + h] != want) {
                if (!bad)
                    printf("  first at (%d, %d): got %08x want %08x\n", h, r, s->pic[(size_t)r * SC_W + h], want);
                bad++;
            }
        }
    report("picture: the contract's colour of each centre texel", sc, bad, n);
    {
        /* the 3x presentation (and the renderer's): each output pixel a texel */
        uint32_t *want = malloc((size_t)TW * TH * 4);
        for (Y = 0; Y < TH; Y++)
            for (X = 0; X < TW; X++) {
                uint32_t t = tx[(size_t)Y * TEX_W + (size_t)X];
                want[(size_t)Y * TW + X] = exact_want(sc, f, Y / K, t);
                if (t && sc == S_EXACT && (t & 0x070707u) != 0)
                    kept8++;
            }
        report("present x3: the contract's colour of each texel", sc, differ(s->pres[1], want, (size_t)TW * TH, 0),
               (long)TW * TH);
        for (kind = 0; kind < 2; kind++)
            if (s->have[kind]) {
                char what[96];
                snprintf(what, sizeof what, "%s: the contract's colour", gpu_what[kind]);
                report(what, sc, differ(s->gpu[kind], want, (size_t)TW * TH, 0), (long)TW * TH);
            }
        if (s->have[2]) {
            for (r = 0; r < SC_H; r++)
                for (h = 0; h < SC_W; h++)
                    want[(size_t)r * SC_W + h] = exact_want(sc, f, r, centre(h, r));
            report("renderer: compositor's picture: the contract's colour", sc,
                   differ(s->gpu[2], want, (size_t)SC_W * SC_H, 0), (long)SC_W * SC_H);
        }
        free(want);
    }
    if (sc == S_EXACT) {
        char what[96];
        snprintf(what, sizeof what, "%ld texels of colours no CGRAM entry holds (low bits set)", kept8);
        holds(sc, what, kept8 > (long)TW * TH / 2);
    }
    if (sc == S_EXACTSUB) {
        long changes = 0;
        int l;
        for (l = 2; l <= f->height; l++)
            changes += f->line[l].fixed_r != f->line[l - 1].fixed_r || f->line[l].fixed_b != f->line[l - 1].fixed_b;
        holds(sc, "the fixed colour changing by line (HDMA)", changes > 150);
    }
}

/* what an indexed-and-32-bit scene holds: its frame's centre texels of
   each kind shown, and where the scene says, the maths, windows and HDMA */
static void check_holds(int sc, const Shot *s)
{
    const PpuFrame *f = ppu_frame();
    long direct = 0, index = 0, clear = 0, as_is = 0, changed = 0;
    int h, r;
    char what[160];
    {
        /* the frame's CGRAM the cartridge's copy (its 32-bit colours made
           from it), but HDMA's entry */
        long off = 0;
        int l, e;
        for (l = 1; l <= f->height; l++)
            for (e = 0; e < 256; e++)
                off += e != HDMA_ENTRY && f->palette[l][e] != cg[e];
        snprintf(what, sizeof what, "the frame's CGRAM the cartridge's (%ld entries of its lines not)", off);
        holds(sc, what, off == 0);
    }
    for (r = 0; r < SC_H; r++)
        for (h = 0; h < SC_W; h++) {
            uint32_t t = centre(h, r);
            if (!t) {
                clear++;
            } else if (t < RR_RGB) {
                index++;
            } else {
                direct++;
                if (s->pic[(size_t)r * SC_W + h] == t)
                    as_is++;
                else
                    changed++;
            }
        }
    snprintf(what, sizeof what, "centre texels: %ld 32-bit colours (%ld shown as they are, %ld changed), %ld indices, %ld clear",
             direct, as_is, changed, index, clear);
    /* plain: some under BG2's tiles, the rest as they are; else the maths
       (or the other screen) changes most */
    holds(sc, what, direct > 20000 && index > 10000 && clear > 10000 && (sc != S_PLAIN || as_is > 2000) &&
                    changed > (sc == S_PLAIN ? 2000 : 15000));
    if (sc == S_SUB) {
        long cgl = 0, fxl = 0;
        int l;
        for (l = 2; l <= f->height; l++) {
            cgl += f->palette[l][HDMA_ENTRY] != f->palette[l - 1][HDMA_ENTRY];
            fxl += f->line[l].fixed_r != f->line[l - 1].fixed_r;
        }
        snprintf(what, sizeof what, "HDMA: CGRAM entry %d changing on %ld lines, the fixed colour on %ld", HDMA_ENTRY, cgl, fxl);
        holds(sc, what, cgl > 200 && fxl > 150);
    }
}

int main(int argc, char **argv)
{
    nshvc_cart cart;
    int seen = 0, si, k;
    (void)argc;
    (void)argv;
    memset(&cart, 0, sizeof cart);
    cart.title = "canvas32_check";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    ix = malloc((size_t)TEX_W * TEX_H);
    tx = malloc((size_t)TEX_W * TEX_H * 4);
    shared = malloc((size_t)TEX_W * TEX_H * 4);
    {
        Shot *both[2] = { &kept, NULL };
        static Shot now;
        both[1] = &now;
        for (k = 0; k < 2; k++) {
            both[k]->pic = malloc((size_t)SC_W * SC_H * 4);
            for (si = 0; si < 5; si++)
                both[k]->pres[si] = malloc((size_t)sz_w(si) * (size_t)sz_h(si) * 4);
            for (si = 0; si < 3; si++)
                both[k]->gpu[si] = malloc((size_t)TW * TH * 4);
        }
#ifdef _WIN32
        _putenv_s("RR_SIZE", "960x720");
#else
        setenv("RR_SIZE", "960x720", 1);
#endif
        gpu_open = rr_open();
        printf("canvas32_check: renderer %s%s\n", rr_name(), gpu_open ? "" : " (cannot open: CPU checks only)");
        if (!gpu_open)
            fails++;
        nshvc_power(&cart);
        while (seen < NSCENES + EQUAL_SCENES) {
            uint8_t id;
            int sc;
            nshvc_run_frame();
            id = nshvc_wram()[0x100];
            if (id < 1 || id > 2 * NSCENES)
                continue;
            sc = (id - 1) / 2;
            if (id & 1) {
                if (sc < EQUAL_SCENES && seen == 2 * sc) {
                    shoot(sc, &kept, "indices");
                    seen++;
                }
            } else if (sc < EQUAL_SCENES ? seen == 2 * sc + 1 : seen == EQUAL_SCENES + sc) {
                shoot(sc, &now, "32-bit");
                if (sc < EQUAL_SCENES) {
                    against_kept(sc, &now);
                    check_holds(sc, &now);
                } else {
                    check_exact(sc, &now);
                }
                seen++;
            }
            if (nshvc_frames() > 400) {
                printf("FAIL: the scenes did not all show (%d)\n", seen);
                return 1;
            }
        }
    }
    if (gpu_open)
        rr_close();
    printf("canvas32_check (%s): %d frames, %ld pixels compared, %d check(s) failed\n", rr_name(), seen, compared, fails);
    return fails ? 1 : 0;
}
