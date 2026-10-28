/*
 * canvas_check.c: a layer's canvas (nshvc_ext_canvas_chars, contract 2.4),
 * a background's characters drawn at the window's resolution from the
 * game's texels, checked on a cartridge of a few lines of C that shows
 * scenes of it (BG1 at 8 bpp scrolled, with flips and tiles outside the
 * canvas; BG1 at 4 bpp in 16 x 16 tiles and palettes; BG2 at 2 bpp in Mode
 * 0; a canvas under and over other layers, windows, colour maths and
 * brightness; a hires line and mosaic, where it does not show; the canvas
 * turned off). For each scene's frame, at one to one, scales up, of
 * another shape, down and a large window:
 *
 *   ref      the canvas layer alone on the main screen: each output pixel
 *            inside one console pixel against the canvas's texel under its
 *            centre, placed by the layer's map, scroll and flips as written
 *            again here, independently of ppu.c and rr_mode7.h
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

#define NSCENES 8
static int fails;
static long compared;
static int front;                      /* through the front end: the canvas at the window's scale */

/* each scene: mode, the canvas's layer, its colour depth, 16 x 16 tiles,
   the scroll, the canvas's texels a console pixel (unit checks), whether
   the canvas layer is alone on the main screen (ref), its texels one
   value a console pixel (flat), and whether it shows at all */
static const struct {
    int mode, layer, bpp, big, hofs, vofs, k, alone, flat, shows;
} scene[NSCENES] = {
    { 3, 0, 8, 0, 37, 11, 3, 1, 0, 1 },     /* 0: BG1 8 bpp, flips, tiles past the canvas */
    { 1, 0, 4, 1, 300, 501, 2, 1, 0, 1 },   /* 1: BG1 4 bpp, 16 x 16, palettes */
    { 0, 1, 2, 0, 5, 250, 4, 1, 0, 1 },     /* 2: BG2 2 bpp, Mode 0's palette offset */
    { 1, 0, 4, 0, 7, 3, 3, 0, 1, 1 },       /* 3: under and over BG2 and BG3, windows, maths: flat */
    { 1, 0, 4, 0, 7, 3, 5, 0, 0, 1 },       /* 4: the same, texels of their own */
    { 5, 0, 4, 0, 0, 0, 3, 0, 0, 0 },       /* 5: a hires mode: the layer's tiles */
    { 1, 0, 4, 0, 0, 0, 3, 0, 0, 0 },       /* 6: BG1 under mosaic: its tiles */
    { 1, 0, 4, 0, 0, 0, 3, 1, 0, 0 },       /* 7: the canvas off */
};
#define CV_ADDR 0x1000u                /* the canvas's first character's word address (the layer's characters' base) */
#define CV_CHARS 64

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
static void make_canvas(int sc, int k)
{
    int c, y, x, mask = (1 << scene[sc].bpp) - 1;
    size_t per = (size_t)64 * (size_t)k * (size_t)k;
    free(texels);
    texels = malloc(CV_CHARS * per);
    for (c = 0; c < CV_CHARS; c++)
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
    nshvc_ext_canvas_chars(scene[sc].layer, CV_ADDR, CV_CHARS, texels, k);
    if (front)
        fprintf(stderr, "canvas_check: scene %d's canvas at k %d\n", sc, k);
}

static void setup(int sc)
{
    int i, n = scene[sc].layer;
    /* VRAM: random words; the map at $0000 (32 x 32), characters from
       $1000 (the first 64 of them the canvas's), BG2's and BG3's maps and
       characters likewise further on */
    w(0x15, 0x80);
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
    const uint16_t *v = f->vram[f->version[r + 1]], *cg = f->palette[r + 1];
    int n = scene[sc].layer, bpp = scene[sc].bpp, ts = scene[sc].big ? 16 : 8, k = f->cv_k;
    unsigned lx = (unsigned)(h + st->bghofs[n]) & 1023u, ly = (unsigned)(r + 1 + st->bgvofs[n]) & 1023u;
    unsigned base = (unsigned)(st->bgsc[n] & 0xFC) << 8, e = v[(base + ((ly / (unsigned)ts) & 31) * 32 + ((lx / (unsigned)ts) & 31)) & 0x7FFF];
    unsigned px = lx % (unsigned)ts, py = ly % (unsigned)ts, ch = e & 0x3FF, nba, ca, t, pal = (e >> 10) & 7;
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
    if (ca >= CV_ADDR && (ca - CV_ADDR) % ((unsigned)bpp * 4) == 0 && (ca - CV_ADDR) / ((unsigned)bpp * 4) < CV_CHARS) {
        unsigned ci = (ca - CV_ADDR) / ((unsigned)bpp * 4);
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
    int X, Y, k = f->cv_k;
    long bad = 0, n = 0;
    char what[64];
    for (Y = 0; Y < oh; Y++) {
        double y0 = (double)Y * 224 / oh, y1 = (double)(Y + 1) * 224 / oh, v = (Y + 0.5) * 224 / oh;
        int r = (int)floor(v), sy;
        double fy = v - r;
        if (floor(y0) != ceil(y1) - 1 || near_edge(fy * k))
            continue;
        sy = (int)floor(fy * k);
        for (X = 0; X < ow; X++) {
            double x0 = (double)X * 256 / ow, x1 = (double)(X + 1) * 256 / ow, u = (X + 0.5) * 256 / ow;
            int h = (int)floor(u), sx;
            double fx = u - h;
            if (floor(x0) != ceil(x1) - 1 || near_edge(fx * k))
                continue;
            sx = (int)floor(fx * k);
            n++;
            if (got[(size_t)Y * (size_t)ow + (size_t)X] != ref_colour(f, sc, h, r, sx, sy)) {
                if (!bad)
                    printf("  first at (%d, %d): got %08x want %08x\n", X, Y, got[(size_t)Y * (size_t)ow + (size_t)X],
                           ref_colour(f, sc, h, r, sx, sy));
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
        for (r = 0; r < 224; r++)
            for (h = 0; h < 256; h++)
                bad += pic[(size_t)r * (size_t)pw + (size_t)h] != ref_colour(f, sc, h, r, k / 2, k / 2);
        report("centre: the picture's texels", sc, bad, 256L * 224);
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
        long cv = 0, past = 0, hf = 0, vf = 0, pair = 0, x, y;
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
                ok = cv > 5000 && past > 5000 && hf > 1000 && vf > 1000 && (scene[sc].bpp == 8 || npal >= 4) && pair > 100;
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
