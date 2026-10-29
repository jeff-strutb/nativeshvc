/*
 * screen_check.c: the largest screens (nshvc_ext_screen to 8192 x 8192,
 * contract 2.7), the console's picture placed on them
 * (nshvc_ext_screen_origin) and sprites reaching all of them
 * (nshvc_ext_sprite_page), checked on a cartridge of a few lines of C that
 * shows 8 x 8 sprites of one colour each over the backdrop: at the
 * screen's corners and edges and at random places, on pages either side
 * of the picture, on the console's own Y page (wrapping in 256 rows
 * above the picture, none below), and through nshvc_ext_sprite_x and _y
 * (page 1). Each scene's whole picture is held, pixel by pixel, to the
 * picture made again here from the sprites' places alone:
 *
 *   centre    8192 x 8192, the picture at the centre
 *   corner    8192 x 8192, the picture at the top left
 *   far       8192 x 8192, the picture at the bottom right
 *   tall      256 x 8192, the picture 7000 rows down
 *   hires     8192 x 8192 in Mode 5, interlaced: each sprite pixel two
 *             columns and two rows (16384 x 16384)
 *   mode 7    8192 x 8192 in Mode 7 with sprites: the picture's size
 *             alone here (with a renderer, below)
 *   clip      1024 x 480, BG1 and the sprites kept to the console's
 *             picture (nshvc_ext_layer_clip) over BG2, not kept: BG2
 *             everywhere past the picture, sprites cut at its edges
 *   outside   1024 x 480, BG1's map all tile 0 (colour 1), its map outside
 *             the console's picture (nshvc_ext_layer_outside) all tile 1
 *             (colour 2): colour 1 exactly on the picture, 2 past it
 *   nowrap    256 x 1024, the picture 600 rows down, Y page 0's wrap above
 *             it off (nshvc_ext_sprite_wrap(0)): its sprites nowhere
 *             above line 1, in the console's own 256 rows as ever
 *   clip 7    the same in Mode 7, BG1 a plane of one colour over the
 *             backdrop; and the presentation at one to one (ppu_present,
 *             the Mode 7 records) exactly the picture
 *
 * Exit 0 when every check holds. With arguments, the scenes run through
 * the front end instead (nshvc_main): with NSHVC_RRCHECK=1, each
 * renderer's presentation of these screens held to the reference's.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "ppu.h"

#define NSCENES 10
#define NSPR 40
#define BACKDROP 0x2108u

static int fails;
static long compared;

typedef struct {
    const char *name;
    int w, h, ox, oy;      /* the screen, where the picture is asked for (-1: the centre) */
    int hires;             /* Mode 5, interlaced */
    int mode7;
    int clip;              /* BG1 and the sprites kept to the console's picture */
    int outside;           /* BG1 alone, its map outside the picture another */
    int nowrap;            /* Y page 0's wrap above the picture off */
} Scene;

static const Scene scenes[NSCENES] = {
    { "centre", 8192, 8192, -1, -1, 0, 0 },
    { "corner", 8192, 8192, 0, 0, 0, 0 },
    { "far", 8192, 8192, 7936, 7968, 0, 0 },
    { "tall", 256, 8192, 0, 7000, 0, 0 },
    { "hires", 8192, 8192, -1, -1, 1, 0 },
    { "mode 7", 8192, 8192, 1000, 6000, 0, 1, 0 },
    { "clip", 1024, 480, -1, -1, 0, 0, 1 },
    { "clip 7", 1024, 480, 100, 200, 0, 1, 1, 0 },
    { "outside", 1024, 480, 300, 100, 0, 0, 0, 1, 0 },
    { "nowrap", 256, 1024, 0, 600, 0, 0, 0, 0, 1 },
};

#define BG1_C 0x03E0u     /* BG1's colour (colour 1), green */
#define BG2_C 0x7C00u     /* BG2's (colour 2), blue */

/* a sprite: its table's X (9 bits) and Y, its pages, how they are set
   (0 nshvc_ext_sprite_page, 1 nshvc_ext_sprite_x, 2 _y), its palette */
typedef struct {
    int x9, y, xp, yp, how, pal;
} Spr;

static Spr spr[NSCENES][NSPR];
static int nspr[NSCENES];

static unsigned rng = 2027;
static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}

static int left_of(const Scene *sc) { return sc->ox < 0 ? (sc->w - 256) / 2 : sc->ox; }
static int top_of(const Scene *sc) { return sc->oy < 0 ? (sc->h - 224) / 2 : sc->oy; }

/* a sprite whose top left is at the screen's column c and row r, on the
   pages that put it there (Y page 0 where the row is in the picture's 256,
   wrapping above them) */
static Spr place(const Scene *sc, int c, int r)
{
    Spr s;
    int dx = c - left_of(sc), dy = r - top_of(sc);
    s.xp = (dx + 256 + 512 * 32) / 512 - 32;
    s.x9 = (dx - 512 * s.xp) & 511;
    s.yp = (dy + 256 * 64) / 256 - 64;
    s.y = dy - 256 * s.yp;
    s.how = 0;
    s.pal = (int)(rnd() % 8);
    return s;
}

static void make_sprites(void)
{
    int k, i;
    for (k = 0; k < NSCENES; k++) {
        const Scene *sc = &scenes[k];
        int n = 0, W = sc->w, H = sc->h;
        Spr *s = spr[k];
        if (sc->clip) {
            int L = left_of(sc), T = top_of(sc);
            s[n++] = place(sc, L - 4, T + 50);       /* across the left edge */
            s[n++] = place(sc, L + 252, T + 90);     /* the right */
            s[n++] = place(sc, L + 30, T - 4);       /* the top */
            s[n++] = place(sc, L + 70, T + 220);     /* the bottom */
            s[n++] = place(sc, L + 128, T + 100);    /* inside */
            s[n++] = place(sc, L - 100, T + 100);    /* outside */
            s[n++] = place(sc, L + 128, T - 100);
            nspr[k] = n;
            continue;
        }
        /* the corners, the edges' middles */
        s[n++] = place(sc, 0, 0);
        s[n++] = place(sc, W - 8, 0);
        s[n++] = place(sc, 0, H - 8);
        s[n++] = place(sc, W - 8, H - 8);
        s[n++] = place(sc, W / 2 - 4, 0);
        s[n++] = place(sc, 0, H / 2 - 4);
        s[n++] = place(sc, W - 8, H / 2 + 100);
        s[n++] = place(sc, W / 2 + 40, H - 8);
        /* on the console's Y page, in the picture's columns: at the
           picture's line 1 and, wrapping, every 256 rows above it */
        s[n].x9 = 120, s[n].y = 10, s[n].xp = 0, s[n].yp = 0, s[n].how = 0, s[n].pal = 3;
        n++;
        /* nshvc_ext_sprite_x and _y: page 1 */
        s[n].x9 = 64, s[n].y = 40, s[n].xp = 1, s[n].yp = 0, s[n].how = 1, s[n].pal = 5;
        n++;
        s[n].x9 = 200, s[n].y = 72, s[n].xp = 0, s[n].yp = 1, s[n].how = 2, s[n].pal = 6;
        n++;
        /* at random */
        for (i = 0; n < NSPR; i++)
            s[n++] = place(sc, (int)(rnd() % (unsigned)(W - 7)), (int)(rnd() % (unsigned)(H - 7)));
        nspr[k] = n;
    }
}

/* ---- the cartridge ---- */
static void w(unsigned reg, unsigned v) { wr8(0x2100 + reg, (uint8_t)v); }

static unsigned colour_of(int pal) { return (unsigned)(0x001F + pal * 0x0C63) & 0x7FFF; }

static void setup(int k)
{
    const Scene *sc = &scenes[k];
    int i;
    nshvc_ext_screen(sc->w, sc->h);
    nshvc_ext_screen_origin(sc->ox, sc->oy);
    nshvc_ext_sprite_wrap(!sc->nowrap);
    for (i = 0; i < 5; i++)
        nshvc_ext_layer_clip(i, sc->clip && (i == 0 || i == 4));
    /* tile 1 (4 bpp, OBSEL 0: sprites' tiles from word 0): colour 1 */
    w(0x15, 0x80);
    w(0x16, 16);
    w(0x17, 0);
    for (i = 0; i < 16; i++) {
        w(0x18, i < 8 ? 0xFF : 0);
        w(0x19, 0);
    }
    w(0x01, 0x00);
    w(0x21, 0);
    w(0x22, BACKDROP & 0xFF);
    w(0x22, BACKDROP >> 8);
    w(0x22, BG1_C & 0xFF);
    w(0x22, BG1_C >> 8);
    w(0x22, BG2_C & 0xFF);
    w(0x22, BG2_C >> 8);
    if (sc->outside) {
        /* BG1 (4 bpp, characters at word 0x2000): tile 0 colour 1, tile 1
           colour 2; its map (word 0) all tile 0, outside all tile 1 */
        static uint16_t outmap[1024];
        w(0x15, 0x80);
        w(0x16, 0);
        w(0x17, 0);
        for (i = 0; i < 0x800; i++) {
            w(0x18, 0);
            w(0x19, 0);
        }
        w(0x16, 0);
        w(0x17, 0x20);
        for (i = 0; i < 32; i++) {
            w(0x18, i < 8 ? 0xFF : 0);
            w(0x19, i >= 16 && i < 24 ? 0xFF : 0);
        }
        w(0x07, 0x00);
        w(0x0B, 0x02);
        for (i = 0; i < 1024; i++)
            outmap[i] = 1;
        nshvc_ext_layer_outside(0, outmap, 0);
    } else {
        nshvc_ext_layer_outside(0, NULL, 0);
    }
    if (sc->clip) {
        /* sprites' tiles from word 0x4000 (OBSEL 2), tile 1 colour 1; BG1's
           and BG2's maps (all tile 0) at words 0 and 0x400, their tile 0
           colour 1 (word 0x2000) and 2 (0x3000); in Mode 7, the plane:
           every map entry tile 0, its pixels colour 1 */
        w(0x15, 0x80);
        w(0x16, 0x10);
        w(0x17, 0x40);
        for (i = 0; i < 16; i++) {
            w(0x18, i < 8 ? 0xFF : 0);
            w(0x19, 0);
        }
        w(0x16, 0);
        w(0x17, 0);
        for (i = 0; i < 0x2000; i++) {
            w(0x18, 0);
            w(0x19, sc->mode7 && i < 64 ? 1 : 0);
        }
        if (!sc->mode7) {
            w(0x16, 0);
            w(0x17, 0x20);
            for (i = 0; i < 16; i++) {
                w(0x18, i < 8 ? 0xFF : 0);
                w(0x19, 0);
            }
            w(0x16, 0);
            w(0x17, 0x30);
            for (i = 0; i < 16; i++) {
                w(0x18, 0);
                w(0x19, i < 8 ? 0xFF : 0);
            }
        }
        w(0x07, 0x00);
        w(0x08, 0x04);
        w(0x0B, 0x32);
    }
    for (i = 0; i < 8; i++) {
        w(0x21, 128 + 16 * i + 1);
        w(0x22, colour_of(i) & 0xFF);
        w(0x22, colour_of(i) >> 8);
    }
    /* the table: the scene's sprites, the rest out of every screen */
    w(0x02, 0);
    w(0x03, 0);
    for (i = 0; i < 128; i++) {
        const Spr *s = i < nspr[k] ? &spr[k][i] : NULL;
        w(0x04, s ? (unsigned)s->x9 & 0xFF : 255);
        w(0x04, s ? (unsigned)s->y : 255);
        w(0x04, 1);
        w(0x04, s ? 0x30u | (unsigned)s->pal << 1 : 0x30);
        if (!s)
            nshvc_ext_sprite_page(i, 16, 32);
        else if (s->how == 1) {
            nshvc_ext_sprite_page(i, 0, 0);
            nshvc_ext_sprite_x(i, 1);
        } else if (s->how == 2) {
            nshvc_ext_sprite_page(i, 0, 0);
            nshvc_ext_sprite_y(i, 1);
        } else
            nshvc_ext_sprite_page(i, s->xp, s->yp);
    }
    for (i = 0; i < 32; i++) {
        unsigned hi = 0, j;
        for (j = 0; j < 4; j++) {
            unsigned n = (unsigned)i * 4 + j;
            unsigned x9 = n < (unsigned)nspr[k] ? (unsigned)spr[k][n].x9 : 255;
            hi |= (x9 >> 8 & 1) << (2 * j);
        }
        w(0x04, hi);
    }
    w(0x01, sc->clip ? 0x02 : 0x00);
    w(0x05, sc->hires ? 0x05 : sc->mode7 ? 0x07 : 0x01);
    w(0x33, sc->hires ? 0x01 : 0x00);
    w(0x2C, sc->outside ? 0x01 : sc->clip ? (sc->mode7 ? 0x11 : 0x13) : sc->mode7 ? 0x11 : 0x10);
    /* hires: the even half pixels the sub screen's, so the sprites on both */
    w(0x2D, sc->hires ? 0x10 : 0);
    w(0x30, 0);
    w(0x31, 0);
    w(0x1A, 0);
    w(0x1B, 0x00);
    w(0x1B, 0x01);
    w(0x1C, 0);
    w(0x1C, 0);
    w(0x1D, 0);
    w(0x1D, 0);
    w(0x1E, 0x00);
    w(0x1E, 0x01);
}

static void game(void)
{
    int k;
    wr8(0x004200, 0x00);
    for (k = 0; k < NSCENES; k++) {
        w(0x00, 0x80);
        setup(k);
        wr8(0x7E0100, 0);
        w(0x00, 0x0F);
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

/* ---- the picture made again ---- */
static uint32_t rgb(unsigned c)
{
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* whether sprite s covers the screen's row R (rows from the picture's
   line 1 at top): on Y page 0 the console's 256-row wrap, none at or past
   256; else its Y and 256 more a page */
static int covers_row(const Spr *s, int R, int top, int nowrap)
{
    int r = R - top, yp = s->how == 2 ? 1 : s->how == 1 ? 0 : s->yp;
    if (yp)
        return r >= s->y + 256 * yp && r < s->y + 256 * yp + 8;
    if (r >= 256 || (nowrap && r < 0))
        return 0;
    return ((r - s->y) % 256 + 256) % 256 < 8;
}

static int col_of(const Spr *s, int left)
{
    int xp = s->how == 1 ? 1 : s->how == 2 ? 0 : s->xp, x = s->x9 >= 256 ? s->x9 - 512 : s->x9;
    return left + x + 512 * xp;
}

static void check_scene(int k)
{
    const Scene *sc = &scenes[k];
    int pw, ph, left = left_of(sc), top = top_of(sc), sx = sc->hires ? 2 : 1, sy = sc->hires ? 2 : 1, R, i;
    const uint32_t *pic = nshvc_picture(&pw, &ph);
    long bad = 0, shown = 0;
    uint32_t back = rgb(BACKDROP);
    if (pw != sc->w * sx || ph != sc->h * sy) {
        printf("FAIL %-7s the picture is %dx%d, want %dx%d\n", sc->name, pw, ph, sc->w * sx, sc->h * sy);
        fails++;
        return;
    }
    if (sc->mode7 && !sc->clip) {
        printf("ok   %-7s %dx%d (Mode 7: through a renderer)\n", sc->name, pw, ph);
        return;
    }
    if (sc->clip) {
        /* the presentation at one to one: Mode 7's records, the picture */
        if (sc->mode7) {
            uint32_t *o = malloc((size_t)pw * (size_t)ph * 4);
            long d = 0;
            int j;
            ppu_present(ppu_frame(), pic, o, pw, ph);
            for (j = 0; j < pw * ph; j++)
                d += o[j] != pic[j];
            printf("%s %-7s the presentation at one to one: %ld differ from the picture\n", d ? "FAIL" : "ok  ",
                   sc->name, d);
            fails += d != 0;
            free(o);
        }
        back = rgb(sc->mode7 ? BACKDROP : BG2_C);
    }
    for (R = 0; R < sc->h; R++) {
        static uint32_t want[8192];
        int x;
        int in_rows = R >= top && R < top + 224;
        for (x = 0; x < sc->w; x++)
            want[x] = sc->clip && in_rows && x >= left && x < left + 256 ? rgb(BG1_C) : back;
        if (sc->outside)
            for (x = 0; x < sc->w; x++)
                want[x] = in_rows && x >= left && x < left + 256 ? rgb(BG1_C) : rgb(BG2_C);
        /* from the last, so the first ends on top */
        for (i = nspr[k]; i-- > 0;) {
            const Spr *s = &spr[k][i];
            int c0 = col_of(s, left), c;
            if (sc->outside || !covers_row(s, R, top, sc->nowrap) || (sc->clip && !in_rows))
                continue;
            for (c = c0; c < c0 + 8; c++)
                if (c >= 0 && c < sc->w && (!sc->clip || (c >= left && c < left + 256)))
                    want[c] = rgb(colour_of(s->pal));
        }
        for (i = 0; i < sy; i++) {
            const uint32_t *row = pic + (size_t)(R * sy + i) * (size_t)pw;
            for (x = 0; x < pw; x++) {
                /* a hires line's first half pixel black, at the screen's left edge */
                uint32_t want_x = sx == 2 && x == 0 ? 0xFF000000u : want[x / sx];
                bad += row[x] != want_x;
                shown += want_x != back && want_x != rgb(BG1_C);
            }
        }
    }
    compared += (long)pw * ph;
    printf("%s %-7s %dx%d, the picture at (%d, %d): %ld differ, %ld sprite pixels\n", bad ? "FAIL" : "ok  ", sc->name,
           pw, ph, left, top, bad, shown);
    if (bad)
        fails++;
    /* every sprite in view: each one's pixels counted (an 8 x 8 square, or
       the console's page's repeats, whole or cut by the screen's edge) */
    if (!sc->outside && shown < (long)nspr[k] * 64 * sx * sy / (sc->clip ? 4 : 2)) {
        printf("FAIL %-7s too few sprite pixels\n", sc->name);
        fails++;
    }
}

int main(int argc, char **argv)
{
    nshvc_cart cart;
    int seen = 0;
    memset(&cart, 0, sizeof cart);
    cart.title = "screen_check";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    make_sprites();
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
        if (nshvc_frames() > 100) {
            printf("FAIL: the scenes did not all show (%d)\n", seen);
            return 1;
        }
    }
    printf("screen_check: %d scenes, %ld pixels compared, %d check(s) failed\n", seen, compared, fails);
    return fails ? 1 : 0;
}
