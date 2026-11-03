/*
 * window_wide.h: the cartridge and the check window_map.c and
 * window_columns.c share: a few lines of C drawing, scene by scene, BG1
 * one colour over the whole screen, or a 64 x 64 sprite at the console's
 * column 0 (lines 100-163), masked by a window on the main screen, or the
 * colour window clipping BG1 to black, the backdrop another colour. Each
 * scene names the screen's columns the window mask covers (the same on
 * every row), worked out by hand from the rule in nshvc.h, or (hdma) has
 * WH0 and WH1 written each line by HDMA: line L's L - 1 and L + 30, the
 * rows above and below the console's lines line 1's and 224's, mapped
 * here with 64-bit arithmetic. Every pixel of the picture is held to the
 * colour that gives.
 */
#include <stdio.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

#define GREEN 0xFF00FF00u   /* BG1: colour 1, 0x03E0 */
#define RED 0xFFFF0000u     /* the backdrop: colour 0, 0x001F */
#define BLUE 0xFF0000FFu    /* the sprite: colour 129, 0x7C00 */
#define BLACK 0xFF000000u   /* clipped by the colour window */

enum { T_BG, T_OBJ, T_COLOR };

typedef struct {
    int width, height;      /* the screen (the console's picture at its centre) */
    int target;             /* T_BG: BG1 windowed; T_OBJ: the sprite; T_COLOR: the colour window */
    int map, mq, mo;        /* nshvc_ext_window_map: on, its scale (16.16) and offset */
    int edges;              /* nshvc_ext_window_edges */
    int wh[4];              /* WH0-WH3 */
    int sel, logic;         /* the target's four window bits (W12SEL's BG1 nibble's layout), its logic */
    int hdma;               /* WH0, WH1 by HDMA each line (L - 1, L + 30) */
    int cn;                 /* windows 1, 2 (bits 0, 1) given columns (nshvc_ext_window_columns) */
    int cols[2][2];         /* their left and right */
    int release;            /* window 1 given columns 100-200, then given back (left -1) */
    int ns;                 /* how many spans of columns the mask covers */
    int spans[4][2];        /* each one's first and last column */
} Scene;

static const Scene *ww_sc;
static int ww_n;

static void w(unsigned reg, unsigned v) { wr8(0x2100 + reg, (uint8_t)v); }

static void game(void)
{
    int k, i, width = 0, height = 0;
    wr8(0x004200, 0x00);
    w(0x00, 0x80);
    /* sprite tiles 0-127 (4 bpp, words 0-0x7FF): every pixel colour 1;
       BG1's tile 1 (2 bpp, characters at word 0x1000): every pixel colour
       1; its map at word 0x2000, every entry tile 1 */
    w(0x15, 0x80);
    w(0x16, 0x00);
    w(0x17, 0x00);
    for (i = 0; i < 0x800; i++) {
        w(0x18, i % 16 < 8 ? 0xFF : 0x00);
        w(0x19, 0x00);
    }
    w(0x16, 0x08);
    w(0x17, 0x10);
    for (i = 0; i < 8; i++) {
        w(0x18, 0xFF);
        w(0x19, 0x00);
    }
    w(0x16, 0x00);
    w(0x17, 0x20);
    for (i = 0; i < 0x400; i++) {
        w(0x18, 0x01);
        w(0x19, 0x00);
    }
    w(0x21, 0);
    w(0x22, 0x1F);
    w(0x22, 0x00);
    w(0x22, 0xE0);
    w(0x22, 0x03);
    w(0x21, 129);
    w(0x22, 0x00);
    w(0x22, 0x7C);
    /* sprite 0 large (64 x 64) at the console's column 0, Y 99, tile 0;
       the rest 8 x 8 at Y 240, off the screen */
    w(0x02, 0);
    w(0x03, 0);
    for (i = 0; i < 128; i++) {
        w(0x04, 0x00);
        w(0x04, i ? 0xF0 : 99);
        w(0x04, 0x00);
        w(0x04, 0x30);
    }
    for (i = 0; i < 32; i++)
        w(0x04, i ? 0x00 : 0x02);   /* sprite 0's size bit */
    w(0x01, 0x40);   /* 8 x 8 and 64 x 64 sprites, their tiles at word 0 */
    w(0x05, 0x00);   /* mode 0 */
    w(0x07, 0x20);   /* BG1's map at word 0x2000 */
    w(0x0B, 0x01);   /* its characters at word 0x1000 */
    w(0x2D, 0x00);
    w(0x2F, 0x00);
    w(0x31, 0x00);
    /* HDMA channel 4: WH0 and WH1 (mode 1), one line an entry: entry k
       k and k + 31 */
    for (i = 0; i < 224; i++) {
        wr8(0x7E2000u + (uint32_t)(3 * i), 0x01);
        wr8(0x7E2001u + (uint32_t)(3 * i), (uint8_t)i);
        wr8(0x7E2002u + (uint32_t)(3 * i), (uint8_t)(i + 31));
    }
    wr8(0x7E2000u + 3 * 224, 0);
    wr8(0x004340, 0x01);
    wr8(0x004341, 0x26);
    wr8(0x004342, 0x00);
    wr8(0x004343, 0x20);
    wr8(0x004344, 0x7E);
    for (k = 0; k < ww_n; k++) {
        const Scene *c = &ww_sc[k];
        w(0x00, 0x80);
        if (c->width != width || c->height != height)
            nshvc_ext_screen(width = c->width, height = c->height);
        nshvc_ext_window_map(c->map, c->mq, c->mo);
        nshvc_ext_window_edges(c->edges);
        for (i = 0; i < 2; i++)
            nshvc_ext_window_columns(i + 1, (c->cn >> i & 1) ? c->cols[i][0] : -1,
                                     (c->cn >> i & 1) ? c->cols[i][1] : 0);
        if (c->release) {
            nshvc_ext_window_columns(1, 100, 200);
            nshvc_ext_window_columns(1, -1, 0);
        }
        w(0x2C, c->target == T_OBJ ? 0x10 : 0x01);
        w(0x2E, c->target == T_BG ? 0x01 : c->target == T_OBJ ? 0x10 : 0x00);
        w(0x23, c->target == T_BG ? (unsigned)c->sel : 0);
        w(0x24, 0);
        w(0x25, c->target == T_OBJ ? (unsigned)c->sel : c->target == T_COLOR ? (unsigned)c->sel << 4 : 0);
        w(0x2A, c->target == T_BG ? (unsigned)c->logic : 0);
        w(0x2B, c->target == T_OBJ ? (unsigned)c->logic : c->target == T_COLOR ? (unsigned)c->logic << 2 : 0);
        w(0x30, c->target == T_COLOR ? 0x80 : 0x00);   /* clip to black inside the colour window */
        for (i = 0; i < 4; i++)
            w(0x26 + (unsigned)i, (unsigned)c->wh[i]);
        wr8(0x00420C, c->hdma ? 0x10 : 0x00);
        wr8(0x7E0100, 0);
        w(0x00, 0x0F);
        nshvc_wait_vblank();
        wr8(0x7E0100, (uint8_t)(k + 1));
        nshvc_wait_vblank();
        wr8(0x7E0100, 0);
    }
    wr8(0x7E0100, 0xFF);
    for (;;)
        nshvc_wait_vblank();
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, game, "reset", 0 } };

/* whether the mask covers screen column x on the console's line `line`
   (1-224) in scene c */
static int masked(const Scene *c, int x, int line)
{
    int k;
    if (c->hdma && !c->cn) {
        /* window 1 alone, enabled, not inverted: L - 1 to L + 30 */
        int l = line - 1, r = line + 30;
        if (c->map)
            return x >= (int)((int64_t)l * c->mq / 65536) + c->mo && x < (int)((int64_t)(r + 1) * c->mq / 65536) + c->mo;
        return x - (c->width - 256) / 2 >= l && x - (c->width - 256) / 2 <= r;
    }
    for (k = 0; k < c->ns; k++)
        if (x >= c->spans[k][0] && x <= c->spans[k][1])
            return 1;
    return 0;
}

/* the colour the scene's column x of picture row y must be, or 0 for a row
   at the sprite's top or bottom edge (not what is checked) */
static uint32_t want(const Scene *c, int x, int y)
{
    int top = (c->height - 224) / 2, line = y - top + 1, sl = (c->width - 256) / 2, m;
    line = line < 1 ? 1 : line > 224 ? 224 : line;
    m = masked(c, x, line);
    if (c->target == T_BG)
        return m ? RED : GREEN;
    if (c->target == T_COLOR)
        return m ? BLACK : GREEN;
    if ((y - top + 1 >= 98 && y - top + 1 <= 101) || (y - top + 1 >= 162 && y - top + 1 <= 165))
        return 0;
    return y - top + 1 >= 100 && y - top + 1 <= 163 && x >= sl && x < sl + 64 && !m ? BLUE : RED;
}

static int ww_main(const char *title, const Scene *sc, int n, int argc, char **argv)
{
    nshvc_cart cart;
    int seen = 0, fails = 0;
    ww_sc = sc;
    ww_n = n;
    memset(&cart, 0, sizeof cart);
    cart.title = title;
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    if (argc > 1 && !strcmp(argv[1], "--bad")) {
        /* a window that is not 1 or 2: fatal (exit 70) */
        nshvc_power(&cart);
        nshvc_ext_window_columns(3, 0, 10);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--badmap")) {
        /* a map of scale 0: fatal (exit 70) */
        nshvc_power(&cart);
        nshvc_ext_window_map(1, 0, 0);
        return 0;
    }
    if (argc > 1)
        return nshvc_main(&cart, argc, argv);
    nshvc_power(&cart);
    while (seen < n) {
        uint8_t id;
        nshvc_run_frame();
        id = nshvc_wram()[0x100];
        if (id >= 1 && id <= n && id == seen + 1) {
            const Scene *c = &sc[id - 1];
            int pw, ph, x, y, bad = 0, checked = 0, hit = 0;
            const uint32_t *px = nshvc_picture(&pw, &ph);
            if (pw != c->width || ph != c->height) {
                printf("FAIL scene %d: the picture is %dx%d, not %dx%d\n", id - 1, pw, ph, c->width, c->height);
                fails++;
                seen++;
                continue;
            }
            for (y = 0; y < ph; y++)
                for (x = 0; x < pw; x++) {
                    uint32_t got = px[(size_t)y * (size_t)pw + (size_t)x], exp = want(c, x, y);
                    if (!exp)
                        continue;
                    checked++;
                    hit += exp == (c->target == T_BG ? RED : c->target == T_COLOR ? BLACK : BLUE);
                    if (got != exp) {
                        if (!bad)
                            printf("  first at column %d row %d: got %08x want %08x\n", x, y, got, exp);
                        bad++;
                    }
                }
            printf("%s scene %d (%dx%d): %d of %d pixels differ (%d %s)\n", bad ? "FAIL" : "ok  ", id - 1, pw, ph, bad,
                   checked, hit, c->target == T_OBJ ? "of the sprite shown" : "masked");
            fails += bad != 0;
            seen++;
        }
        if (nshvc_frames() > 200) {
            printf("FAIL: the scenes did not all show (%d)\n", seen);
            return 1;
        }
    }
    printf("%s: %d scenes, %d failed\n", title, seen, fails);
    return fails ? 1 : 0;
}
