/*
 * canvas_check.c: a layer's canvas (nshvc_ext_canvas_chars, contracts 2.4
 * and 2.5),
 * a background's characters drawn at the window's resolution from the
 * game's texels, checked on a cartridge of a few lines of C that shows
 * scenes of it (BG1 at 8 bpp scrolled, with flips and tiles outside the
 * canvas; BG1 at 4 bpp in 16 x 16 tiles and palettes; BG2 at 2 bpp in Mode
 * 0; a canvas under and over other layers, windows, colour maths and
 * brightness; a hires line and mosaic, where it does not show; the canvas
 * turned off; a layer on VRAM page 1 with a canvas in the layer's page;
 * a layer on page 1 at the top and page 2 below, one canvas across both
 * pages; a screen canvas, nshvc_ext_canvas_screen, contract 2.8, on a
 * 320 x 240 screen with the console's picture off its centre, alone and
 * under windows and maths). For each scene's frame, at one to one, scales up, of
 * another shape, down and a large window:
 *
 *   ref      the canvas layer alone on the main screen: each output pixel
 *            inside one console pixel against the canvas's texel under its
 *            centre, placed by the layer's map, scroll and flips as written
 *            again here, independently of ppu.c and rr_mode7.h (a screen
 *            canvas: the texel by the screen's pixel, none past its size)
 *   centre   the console's picture (nshvc_picture): each canvas pixel the
 *            centre texel of its console pixel
 *   flat     a canvas whose texels are one value in each console pixel:
 *            the presentation is the picture scaled (rr_ref_image), byte
 *            for byte, so a canvas pixel is composed exactly as the
 *            console composes it (the other layers, windows, maths)
 *   off      no canvas on the frame (a hires line, mosaic, the canvas
 *            off): the picture scaled
 *
 * Exit 0 when every check holds. With arguments, the scenes run through
 * the front end instead (nshvc_main), the canvas at the window's scale
 * (nshvc_ext_canvas_scale): with NSHVC_RRCHECK=1, each renderer held to
 * the reference.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "ppu.h"
#include "rr_size.h"
#include "rr_mode7.h"

#define NSCENES 12
static int fails;
static long compared;
static int front;                      /* through the front end: the canvas at the window's scale */

/* each scene: mode, the canvas's layer, its colour depth, 16 x 16 tiles,
   the scroll, the canvas's texels a console pixel (unit checks), whether
   the canvas layer is alone on the main screen (ref), its texels one
   value a console pixel (flat), whether it shows at all, and its VRAM
   pages (0: page 0; 1: the layer on page 1, the canvas in the layer's
   page; 2: the layer on page 1 to line 112 and page 2 below, one canvas
   in VRAM's whole space across both) */
static const struct {
    int mode, layer, bpp, big, hofs, vofs, k, alone, flat, shows, pages, screen;
} scene[NSCENES] = {
    { 3, 0, 8, 0, 37, 11, 3, 1, 0, 1 },     /* 0: BG1 8 bpp, flips, tiles past the canvas */
    { 1, 0, 4, 1, 300, 501, 2, 1, 0, 1 },   /* 1: BG1 4 bpp, 16 x 16, palettes */
    { 0, 1, 2, 0, 5, 250, 4, 1, 0, 1 },     /* 2: BG2 2 bpp, Mode 0's palette offset */
    { 1, 0, 4, 0, 7, 3, 3, 0, 1, 1 },       /* 3: under and over BG2 and BG3, windows, maths: flat */
    { 1, 0, 4, 0, 7, 3, 5, 0, 0, 1 },       /* 4: the same, texels of their own */
    { 5, 0, 4, 0, 0, 0, 3, 0, 0, 0 },       /* 5: a hires mode: the layer's tiles */
    { 1, 0, 4, 0, 0, 0, 3, 0, 0, 0 },       /* 6: BG1 under mosaic: its tiles */
    { 1, 0, 4, 0, 0, 0, 3, 1, 0, 0 },       /* 7: the canvas off */
    { 3, 0, 8, 0, 37, 11, 3, 1, 0, 1, 1 },  /* 8: BG1 on page 1, the canvas in its page */
    { 3, 0, 8, 0, 37, 11, 3, 1, 0, 1, 2 },  /* 9: BG1 on pages 1 and 2, one canvas across them */
    { 1, 0, 4, 0, 0, 0, 3, 1, 0, 1, 0, 1 }, /* 10: a screen canvas, BG1 4 bpp, palette 5, priority 1 */
    { 1, 0, 4, 0, 0, 0, 3, 0, 1, 1, 0, 1 }, /* 11: the same under windows and maths: flat */
};
/* a screen canvas's screen, where the console's picture sits on it, the
   texels' width short of the screen's (none past them), its palette and
   priority */
#define SC_W 320
#define SC_H 240
#define SC_LEFT 20
#define SC_TOP 6
#define SC_SHORT 40
#define SC_PAL 5
#define SC_PRIO 1
#define CV_ADDR 0x1000u                /* the canvas's first character's word address (the layer's characters' base) */
#define CV_CHARS 64
/* the scene's canvas: its word address and characters (across pages, from
   page 1's $1000 to page 2's $1000 + 64 characters at 8 bpp) */
static unsigned cv_addr(int sc) { return scene[sc].pages == 2 ? 0x8000u + CV_ADDR : CV_ADDR; }
static unsigned cv_chars(int sc) { return scene[sc].pages == 2 ? (0x10000u - 0x8000u) / 32u + CV_CHARS : CV_CHARS; }

/* ---- the cartridge ---- */
static unsigned rng = 4242;
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

static uint8_t *texels;
static int scene_k;                    /* the k of the scene's canvas */

/* the scene's canvas at k: random texels (a quarter 0), or one value in
   each console pixel's k x k (flat) */
static int sc_w, sc_h;                 /* a screen canvas's texels across and down */

static void make_canvas(int sc, int k)
{
    int c, y, x, mask = (1 << scene[sc].bpp) - 1;
    size_t per = (size_t)64 * (size_t)k * (size_t)k;
    if (scene[sc].screen) {
        sc_w = (SC_W - SC_SHORT) * k;
        sc_h = SC_H * k;
        free(texels);
        texels = malloc((size_t)sc_w * (size_t)sc_h);
        for (y = 0; y < sc_h; y++)
            for (x = 0; x < sc_w; x++) {
                unsigned v;
                if (scene[sc].flat) {
                    unsigned px = (unsigned)(x / k), py = (unsigned)(y / k), hsh = py * 17u + px * 7u;
                    v = (hsh % 5u == 0) ? 0u : (hsh * 2654435761u >> 13) & (unsigned)mask;
                } else {
                    v = rnd() & (unsigned)mask;
                    if ((rnd() & 3) == 0)
                        v = 0;
                }
                texels[(size_t)y * (size_t)sc_w + (size_t)x] = (uint8_t)v;
            }
        scene_k = k;
        nshvc_ext_canvas_screen(scene[sc].layer, texels, sc_w, sc_h, k, SC_PRIO, SC_PAL);
        if (front)
            fprintf(stderr, "canvas_check: scene %d's canvas at k %d\n", sc, k);
        return;
    }
    free(texels);
    texels = malloc(cv_chars(sc) * per);
    for (c = 0; c < (int)cv_chars(sc); c++)
        for (y = 0; y < 8 * k; y++)
            for (x = 0; x < 8 * k; x++) {
                unsigned v;
                if (scene[sc].flat) {
                    /* one value a console pixel: from the pixel's place alone */
                    unsigned px = (unsigned)(x / k), py = (unsigned)(y / k), hsh = (unsigned)c * 131u + py * 17u + px * 7u;
                    v = (hsh % 5u == 0) ? 0u : (hsh * 2654435761u >> 13) & (unsigned)mask;
                } else {
                    v = rnd() & (unsigned)mask;
                    if ((rnd() & 3) == 0)
                        v = 0;
                }
                texels[(size_t)c * per + (size_t)y * 8u * (size_t)k + (size_t)x] = (uint8_t)v;
            }
    scene_k = k;
    nshvc_ext_canvas_chars(scene[sc].layer, cv_addr(sc), (int)cv_chars(sc), texels, k);
    if (front)
        fprintf(stderr, "canvas_check: scene %d's canvas at k %d\n", sc, k);
}

static void setup(int sc)
{
    int i, n = scene[sc].layer, page;
    nshvc_ext_screen(scene[sc].screen ? SC_W : 256, scene[sc].screen ? SC_H : 224);
    nshvc_ext_screen_origin(scene[sc].screen ? SC_LEFT : -1, scene[sc].screen ? SC_TOP : -1);
    /* VRAM: random words; the map at $0000 (32 x 32), characters from
       $1000 (the first 64 of them the canvas's), BG2's and BG3's maps and
       characters likewise further on; in each page the scene uses */
    w(0x15, 0x80);
    for (page = 0; page <= scene[sc].pages; page++) {
    nshvc_ext_vram_page(page);
    w(0x16, 0);
    w(0x17, 0);
    for (i = 0; i < 0x8000; i++) {
        unsigned v = rnd() & 0xFFFF;
        if (i < 0x400) {
            /* the canvas layer's map: characters 0-95 (64 the canvas's),
               random flips, palettes and priority */
            v = (rnd() % 96u) | (rnd() & 0xFC00u);
        } else if (i >= 0x0800 && i < 0x0C00) {
            v = (rnd() % 256u) | (rnd() & 0xFC00u);   /* BG2's map */
        } else if (i >= 0x0C00 && i < 0x1000) {
            v = (rnd() % 256u) | (rnd() & 0xFC00u);   /* BG3's map */
        }
        w(0x18, v & 0xFF);
        w(0x19, v >> 8);
    }
    }
    nshvc_ext_vram_page(0);
    /* the layer's page: page 1, or by HDMA to its page register ($21C0 +
       layer) page 1 then page 2 from line 112 */
    nshvc_ext_layer_page(n, scene[sc].pages ? 1 : 0);
    wr8(0x00420C, 0);
    if (scene[sc].pages == 2) {
        static const uint8_t table[] = { 0x70, 1, 0x01, 2, 0x00 };
        for (i = 0; i < (int)sizeof table; i++)
            wr8(0x7E2000 + (uint32_t)i, table[i]);
        wr8(0x004300, 0x00);
        wr8(0x004301, (uint8_t)(0xC0 + n));
        wr8(0x004302, 0x00);
        wr8(0x004303, 0x20);
        wr8(0x004304, 0x7E);
        wr8(0x00420C, 0x01);
    }
    w(0x21, 0);
    for (i = 0; i < 256; i++)
        w2(0x22, (int)(rnd() & 0x7FFF));
    w(0x05, (unsigned)scene[sc].mode | (scene[sc].big ? 0x10u << n : 0u));
    w(0x06, sc == 6 ? 0x31 : 0x00);
    /* the canvas layer's map at $0000, its characters at $1000; the other
       layers' maps at $0800 and $0C00, their characters at $4000 */
    for (i = 0; i < 4; i++)
        w(0x07 + (unsigned)i, i == n ? 0x00 : i == 1 || (n == 1 && i == 0) ? 0x08 : 0x0C);
    if (n == 0)
        w(0x0B, 0x41);
    else
        w(0x0B, 0x14);
    w(0x0C, 0x44);
    for (i = 0; i < 4; i++) {
        w(0x0D + 2 * (unsigned)i, i == n ? (unsigned)scene[sc].hofs & 0xFF : (unsigned)(i * 13));
        w(0x0D + 2 * (unsigned)i, i == n ? (unsigned)scene[sc].hofs >> 8 : 0);
        w(0x0E + 2 * (unsigned)i, i == n ? (unsigned)scene[sc].vofs & 0xFF : (unsigned)(i * 29));
        w(0x0E + 2 * (unsigned)i, i == n ? (unsigned)scene[sc].vofs >> 8 : 0);
    }
    if (scene[sc].alone) {
        w(0x2C, 1u << n);
        w(0x2D, 0);
        w(0x2E, 0);
        w(0x2F, 0);
        w(0x30, 0);
        w(0x31, 0);
    } else {
        /* BG1 (the canvas) and BG3 on the main screen, BG2 on the sub
           screen; window 1 (columns 60-180) masks BG1 on the main screen;
           colour maths adds the sub screen to BG1, halved */
        w(0x2C, 0x05);
        w(0x2D, 0x02);
        w(0x2E, 0x01);
        w(0x2F, 0x00);
        w(0x23, 0x02);
        w(0x26, 60);
        w(0x27, 180);
        w(0x2A, 0);
        w(0x30, 0x02);
        w(0x31, 0x41);
        w(0x32, 0xE4);
    }
    w(0x33, 0);
}

static void game(void)
{
    int sc;
    wr8(0x004200, 0x00);
    nshvc_ext_vram(192);
    for (sc = 0; sc < NSCENES; sc++) {
        w(0x00, 0x80);
        setup(sc);
        if (sc == 7)
            nshvc_ext_canvas_chars(0, 0, 0, NULL, 1);
        else
            make_canvas(sc, front ? nshvc_ext_canvas_scale() : scene[sc].k);
        wr8(0x7E0100, 0);
        w(0x00, sc == 3 || sc == 4 ? 0x0C : 0x0F);
        nshvc_wait_vblank();           /* a frame the canvas began with */
        wr8(0x7E0100, (uint8_t)(sc + 1));
        nshvc_wait_vblank();           /* the scene's whole frame */
        wr8(0x7E0100, 0);
        /* at the window's scale, a canvas made again when it changed */
        while (front && sc != 7 && scene_k != nshvc_ext_canvas_scale()) {
            make_canvas(sc, nshvc_ext_canvas_scale());
            nshvc_wait_vblank();
            nshvc_wait_vblank();
        }
    }
    wr8(0x7E0100, 0xFF);
    for (;;)
        nshvc_wait_vblank();
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, game, "reset", 0 } };

/* ---- the reference: the canvas layer alone, placed again here ---- */
static uint32_t rgb(unsigned c)
{
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* the colour of console pixel (h, r) of the layer, its texel (sx, sy) of
   the pixel's k x k where the canvas covers the tile, else the tile's pixel */
static uint32_t ref_colour(const PpuFrame *f, int sc, int h, int r, int sx, int sy)
{
    const PpuScreen *st = &f->line[r + 1];
    const uint16_t *cg = f->palette[r + 1];
    int n = scene[sc].layer, bpp = scene[sc].bpp, ts = scene[sc].big ? 16 : 8, k = f->cv_k;
    /* the layer's page on this line */
    const uint16_t *v = f->vram[f->version[r + 1]] + ((unsigned)st->page[n] << 15);
    unsigned lx = (unsigned)(h + st->bghofs[n]) & 1023u, ly = (unsigned)(r + 1 + st->bgvofs[n]) & 1023u;
    unsigned base = (unsigned)(st->bgsc[n] & 0xFC) << 8, e = v[(base + ((ly / (unsigned)ts) & 31) * 32 + ((lx / (unsigned)ts) & 31)) & 0x7FFF];
    unsigned px = lx % (unsigned)ts, py = ly % (unsigned)ts, ch = e & 0x3FF, nba, ca, a, t, pal = (e >> 10) & 7;
    if (e & 0x4000) {
        px = (unsigned)ts - 1 - px;
        sx = k - 1 - sx;
    }
    if (e & 0x8000) {
        py = (unsigned)ts - 1 - py;
        sy = k - 1 - sy;
    }
    if (px >= 8) ch += 1;
    if (py >= 8) ch += 16;
    nba = (unsigned)((n < 2 ? st->bgnba[0] : st->bgnba[1]) >> ((n & 1) * 4) & 0x0F) << 12;
    ca = (nba + (ch & 0x3FF) * (unsigned)bpp * 4) & 0x7FFF;
    /* the canvas's address for the character: in the layer's page, or
       from $8000 in VRAM's whole space */
    a = cv_addr(sc) >= 0x8000 ? (unsigned)st->page[n] << 15 | ca : ca;
    if (a >= cv_addr(sc) && (a - cv_addr(sc)) % ((unsigned)bpp * 4) == 0 &&
        (a - cv_addr(sc)) / ((unsigned)bpp * 4) < cv_chars(sc)) {
        unsigned ci = (a - cv_addr(sc)) / ((unsigned)bpp * 4);
        t = texels[(size_t)ci * 64u * (size_t)k * (size_t)k + ((size_t)(py & 7) * (size_t)k + (size_t)sy) * 8u * (size_t)k +
                   (size_t)(px & 7) * (size_t)k + (size_t)sx];
    } else {
        unsigned p, bit = 7 - (px & 7);
        t = 0;
        for (p = 0; p < (unsigned)bpp; p += 2) {
            unsigned word = v[(ca + p * 4 + (py & 7)) & 0x7FFF];
            t |= ((word >> bit) & 1u) << p;
            t |= ((word >> (bit + 8)) & 1u) << (p + 1);
        }
    }
    if (!t)
        return rgb(cg[0]);
    if (bpp == 8)
        return rgb(cg[t]);
    if (bpp == 4)
        return rgb(cg[pal * 16 + t]);
    return rgb(cg[(scene[sc].mode == 0 ? (unsigned)n * 32 : 0) + pal * 4 + t]);
}

/* a screen canvas's colour at the screen's pixel (h, r), its texel (sx,
   sy) of the pixel's k x k: by the screen's pixel, none past the texels */
static uint32_t ref_screen(const PpuFrame *f, int h, int r, int sx, int sy)
{
    int k = f->cv_k, line = r - f->top + 1;
    const uint16_t *cg = f->palette[line < 1 ? 1 : line > f->height ? f->height : line];
    long ex = (long)h * k + sx, ey = (long)r * k + sy;
    unsigned t = ex < sc_w && ey < sc_h ? texels[(size_t)ey * (size_t)sc_w + (size_t)ex] & 15u : 0u;
    return t ? rgb(cg[SC_PAL * 16 + t]) : rgb(cg[0]);
}

static void report(const char *what, int sc, long bad, long n)
{
    compared += n;
    printf("%s scene %d %-40s: %ld of %ld differ\n", bad ? "FAIL" : "ok  ", sc, what, bad, n);
    if (bad)
        fails++;
}

/* a centre's place in texels close to a texel's edge, but not on it: there
   the reference's 1/1024 steps and this check's doubles may round apart
   (exactly on an edge, as every centre at one to one, they agree) */
static int near_edge(double t)
{
    double e = fabs(t - floor(t + 0.5));
    return e > 0 && e < 0.01;
}

/* each output pixel wholly inside one console pixel (no blend across
   pixels), its centre's texel well inside one of the pixel's k x k */
static void ref_check(const PpuFrame *f, int sc, const uint32_t *got, int ow, int oh, double scx, double scy)
{
    int X, Y, k = f->cv_k, pw = scene[sc].screen ? SC_W : 256, ph = scene[sc].screen ? SC_H : 224;
    long bad = 0, n = 0;
    char what[64];
    for (Y = 0; Y < oh; Y++) {
        double y0 = (double)Y * ph / oh, y1 = (double)(Y + 1) * ph / oh, v = (Y + 0.5) * ph / oh;
        int r = (int)floor(v), sy;
        double fy = v - r;
        if (floor(y0) != ceil(y1) - 1 || near_edge(fy * k))
            continue;
        sy = (int)floor(fy * k);
        for (X = 0; X < ow; X++) {
            double x0 = (double)X * pw / ow, x1 = (double)(X + 1) * pw / ow, u = (X + 0.5) * pw / ow;
            uint32_t want;
            int h = (int)floor(u), sx;
            double fx = u - h;
            if (floor(x0) != ceil(x1) - 1 || near_edge(fx * k))
                continue;
            sx = (int)floor(fx * k);
            n++;
            want = scene[sc].screen ? ref_screen(f, h, r, sx, sy) : ref_colour(f, sc, h, r, sx, sy);
            if (got[(size_t)Y * (size_t)ow + (size_t)X] != want) {
                if (!bad)
                    printf("  first at (%d, %d): got %08x want %08x\n", X, Y, got[(size_t)Y * (size_t)ow + (size_t)X], want);
                bad++;
            }
        }
    }
    snprintf(what, sizeof what, "ref x%.2fx%.2f", scx, scy);
    report(what, sc, bad, n);
    if (n < (long)ow * oh / 20) {
        printf("FAIL scene %d ref x%.2fx%.2f: too few pixels compared (%ld)\n", sc, scx, scy, n);
        fails++;
    }
}

static void check_scene(int sc)
{
    const PpuFrame *f = ppu_frame();
    int pw, ph, si, j;
    const uint32_t *pic = ppu_picture(&pw, &ph);
    const double size[5][2] = { { 1, 1 }, { 2.5, 2.5 }, { 3.37, 2.91 }, { 0.6, 0.77 }, { 6, 6 } };
    uint32_t *a = malloc((size_t)pw * 6 * (size_t)ph * 6 * 4), *b = malloc((size_t)pw * 6 * (size_t)ph * 6 * 4);
    char what[80];
    int shown = ppu_frame_mode7(f);
    printf("scene %d: %dx%d picture, canvas %s (k %d)\n", sc, pw, ph, shown ? "shown" : "not shown", f->cv_on ? f->cv_k : 0);
    if (shown != scene[sc].shows) {
        printf("FAIL scene %d: the canvas expected %s\n", sc, scene[sc].shows ? "shown" : "not shown");
        fails++;
    }
    if (scene[sc].alone && scene[sc].shows) {
        /* the console's picture: the centre texel */
        long bad = 0;
        int k = f->cv_k, r, h;
        for (r = 0; r < ph; r++)
            for (h = 0; h < pw; h++)
                bad += pic[(size_t)r * (size_t)pw + (size_t)h] !=
                       (scene[sc].screen ? ref_screen(f, h, r, k / 2, k / 2) : ref_colour(f, sc, h, r, k / 2, k / 2));
        report("centre: the picture's texels", sc, bad, (long)pw * ph);
    }
    for (si = 0; si < 5; si++) {
        int ow = (int)(pw * size[si][0] + 0.5), oh = (int)(ph * size[si][1] + 0.5), d[4] = { 0, 0, ow, oh };
        ppu_present(f, pic, a, ow, oh);
        if (scene[sc].alone && scene[sc].shows && size[si][0] >= 1)
            ref_check(f, sc, a, ow, oh, size[si][0], size[si][1]);
        if (scene[sc].flat || !scene[sc].shows) {
            long bad = 0;
            for (j = 0; j < ow * oh; j++)
                b[j] = 0xFF000000u;
            rr_ref_image(b, ow, oh, pic, pw, ph, d);
            for (j = 0; j < ow * oh; j++)
                if (a[j] != b[j]) {
                    if (!bad)
                        printf("  first at (%d, %d): got %08x want %08x\n", j % ow, j / ow, a[j], b[j]);
                    bad++;
                }
            snprintf(what, sizeof what, "%s: rr_ref_image x%.2fx%.2f", scene[sc].flat ? "flat" : "off", size[si][0],
                     size[si][1]);
            report(what, sc, bad, (long)ow * oh);
        }
    }
    if (scene[sc].shows) {
        /* the scene holds what it is to check: canvas pixels, tiles past the
           canvas on its rows, flipped tiles each way, more than one palette,
           and picture pixels side by side with one record (zeros) and two
           colours, which the presentation must blend as two */
        RrMode7 mm;
        long cv = 0, past = 0, hf = 0, vf = 0, pair = 0, x, y, lo = 0, hi = 0;
        unsigned pals = 0;
        if (!ppu_mode7(f, pic, &mm)) {
            printf("FAIL scene %d: no record\n", sc);
            fails++;
        } else {
            for (y = 0; y < mm.ph; y++)
                for (x = 0; x < mm.pw && mm.row_m7[y]; x++) {
                    const RrM7Px *q = &mm.px[y * mm.pw + x];
                    if (q->w2 & RR_CV) {
                        cv++;
                        hf += q->w3 >> 20 & 1;
                        vf += q->w3 >> 21 & 1;
                        pals |= 1u << (q->w3 >> 22 & 7);
                        if ((q->w3 & 0x3FFF) < CV_CHARS)
                            lo++;
                        else if ((q->w3 & 0x3FFF) >= 1024)
                            hi++;
                    } else if (q->w2 & RR_M7_NOT) {
                        past++;
                        if (x + 1 < mm.pw && !memcmp(q, q + 1, sizeof *q) && pic[y * pw + x] != pic[y * pw + x + 1])
                            pair++;
                    }
                }
            {
                int npal = 0, b2;
                int ok;
                for (b2 = 0; b2 < 8; b2++)
                    npal += pals >> b2 & 1;
                if (scene[sc].screen)
                    /* a screen canvas: every pixel of the layer the canvas's,
                       none flipped, its one palette */
                    ok = cv == (long)mm.pw * mm.ph && hf == 0 && vf == 0 && pals == 1u << SC_PAL;
                else
                    ok = cv > 5000 && past > 5000 && hf > 1000 && vf > 1000 && (scene[sc].bpp == 8 || npal >= 4) &&
                         pair > 100;
                if (scene[sc].pages) {
                    /* the layer's pages on its lines, and for the canvas
                       across pages, characters of both pages' ranges */
                    long on1 = 0, on2 = 0;
                    for (y = 1; y <= 224; y++) {
                        on1 += f->line[y].page[scene[sc].layer] == 1;
                        on2 += f->line[y].page[scene[sc].layer] == 2;
                    }
                    if (scene[sc].pages == 1)
                        ok = ok && on1 == 224;
                    else
                        ok = ok && on1 > 100 && on2 > 100 && lo > 2000 && hi > 2000;
                    printf("%s scene %d pages: %ld lines on page 1, %ld on page 2; %ld canvas pixels from page 1's "
                           "characters, %ld from page 2's\n", ok ? "ok  " : "FAIL", sc, on1, on2, lo, hi);
                }
                printf("%s scene %d holds: %ld canvas pixels, %ld past it, %ld flipped across, %ld down, %d palettes, %ld "
                       "pairs of one record and two colours\n",
                       ok ? "ok  " : "FAIL", sc, cv, past, hf, vf, npal, pair);
                fails += !ok;
            }
        }
    }
    if (sc == 3) {
        /* (the scene must show more than the canvas: the windows' and the
           maths' colours) */
        long other = 0;
        for (j = 0; j < pw * ph; j++)
            other += pic[j] != pic[0];
        printf("%s scene 3 pixels of more than one colour: %ld\n", other > pw * ph / 4 ? "ok  " : "FAIL", other);
        fails += other <= pw * ph / 4;
    }
    free(a);
    free(b);
}

int main(int argc, char **argv)
{
    nshvc_cart cart;
    int seen = 0;
    memset(&cart, 0, sizeof cart);
    cart.title = "canvas_check";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    if (argc > 1) {
        front = 1;
        return nshvc_main(&cart, argc, argv);
    }
    nshvc_power(&cart);
    while (seen < NSCENES) {
        uint8_t id;
        nshvc_run_frame();
        id = nshvc_wram()[0x100];
        if (id >= 1 && id <= NSCENES && id == seen + 1) {
            check_scene(id - 1);
            seen++;
        }
        if (nshvc_frames() > 300) {
            printf("FAIL: the scenes did not all show (%d)\n", seen);
            return 1;
        }
    }
    printf("canvas_check: %d scenes, %ld pixels compared, %d check(s) failed\n", seen, compared, fails);
    return fails ? 1 : 0;
}
