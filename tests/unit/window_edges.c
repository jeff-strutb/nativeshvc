/*
 * window_edges.c: the windows on a larger screen (nshvc_ext_window_edges),
 * on a cartridge of a few lines of C: on a 320-wide screen (the console's
 * picture at columns 32-287), BG1 one colour, or a sprite past the
 * picture's right edge, window 1 masking it on the main screen, the
 * backdrop another colour. For each scene, every column of a line against
 * the colour worked out here from the rule:
 *
 *   0  edges shut, window 0-255 inverted: BG1 inside the picture only
 *   1  edges open, the same: BG1 across the screen
 *   2  edges open, window 10-255 inverted: the left past the picture
 *      masked (its edge is not the picture's), the right open
 *   3  edges open, window 0-255: masked across the screen
 *   4  edges shut, window 0-255: BG1 past the picture only
 *   5  edges open, window 178-255 inverted, its left edge written each
 *      line by HDMA (WH0 0 otherwise): BG1 from 178 on, past the right
 *   6  the same window over the sprites: a sprite at the console's column
 *      264 (X page 1, nshvc_ext_sprite_page) shown past the right edge
 *   7  the same with the edges shut: the sprite masked
 *
 * Exit 0 when every column holds. With arguments, the scenes run through
 * the front end instead (nshvc_main), for NSHVC_COMPOSECHECK=1 (the
 * compositor against the frame from its record) and NSHVC_RRCHECK=1.
 */
#include <stdio.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

#define NSCENES 8
#define GREEN 0xFF00FF00u   /* BG1: colour 1, 0x03E0 */
#define RED 0xFFFF0000u     /* the backdrop: colour 0, 0x001F */
#define BLUE 0xFF0000FFu    /* the sprite: colour 129, 0x7C00 */
#define SPRITE_X 264        /* the console's column, X page 1 */
#define ROW 112             /* the line checked (the sprite covers it) */

static const struct {
    int open, inv, left, hdma, obj;
} sc[NSCENES] = {
    { 0, 1, 0, 0, 0 }, { 1, 1, 0, 0, 0 }, { 1, 1, 10, 0, 0 }, { 1, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 }, { 1, 1, 178, 1, 0 }, { 1, 1, 178, 1, 1 }, { 0, 1, 178, 1, 1 },
};

static void w(unsigned reg, unsigned v) { wr8(0x2100 + reg, (uint8_t)v); }

static void game(void)
{
    int k, i;
    wr8(0x004200, 0x00);
    w(0x00, 0x80);
    nshvc_ext_screen(320, 224);
    /* BG tile 1: every pixel colour 1 (2 bpp, low plane set); the map at
       word 0x400, every entry tile 1. Sprite tile 2 (4 bpp, words 0x20-0x2F):
       every pixel colour 1 */
    w(0x15, 0x80);
    w(0x16, 0x08);
    w(0x17, 0x00);
    for (i = 0; i < 8; i++) {
        w(0x18, 0xFF);
        w(0x19, 0x00);
    }
    w(0x16, 0x20);
    w(0x17, 0x00);
    for (i = 0; i < 16; i++) {
        w(0x18, i < 8 ? 0xFF : 0x00);
        w(0x19, 0x00);
    }
    w(0x16, 0x00);
    w(0x17, 0x04);
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
    /* every sprite off the screen but sprite 0: at the console's column
       264 (X 0x108, 9 bits: -248; X page 1 adds 512), line 108, tile 2 */
    w(0x02, 0);
    w(0x03, 0);
    for (i = 0; i < 128; i++) {
        w(0x04, i ? 0x00 : SPRITE_X & 0xFF);
        w(0x04, i ? 0xF0 : 108);
        w(0x04, i ? 0x00 : 2);
        w(0x04, 0x30);
    }
    for (i = 0; i < 32; i++)
        w(0x04, i ? 0x00 : 0x01);   /* sprite 0's X bit 8 */
    nshvc_ext_sprite_page(0, 1, 0);
    w(0x01, 0x00);   /* 8x8 sprites, their tiles at word 0 */
    w(0x05, 0x00);   /* mode 0 */
    w(0x07, 0x04);   /* BG1's map at word 0x400 */
    w(0x0B, 0x00);
    w(0x27, 0xFF);
    /* HDMA channel 4: WH0 178 on every line, one byte, mode 0 */
    for (i = 0; i < 2; i++) {
        wr8(0x7E2000u + (uint32_t)(2 * i), 0x70);
        wr8(0x7E2001u + (uint32_t)(2 * i), 178);
    }
    wr8(0x7E2004, 0);
    wr8(0x004340, 0x00);
    wr8(0x004341, 0x26);
    wr8(0x004342, 0x00);
    wr8(0x004343, 0x20);
    wr8(0x004344, 0x7E);
    for (k = 0; k < NSCENES; k++) {
        w(0x00, 0x80);
        nshvc_ext_window_edges(sc[k].open);
        w(0x2C, sc[k].obj ? 0x10 : 0x01);   /* BG1 or the sprites on the main screen */
        w(0x2E, sc[k].obj ? 0x10 : 0x01);   /* windowed there */
        w(0x23, sc[k].obj ? 0x00 : (sc[k].inv ? 0x03 : 0x02));
        w(0x25, sc[k].obj ? (sc[k].inv ? 0x03 : 0x02) : 0x00);
        w(0x26, sc[k].hdma ? 0 : sc[k].left);
        wr8(0x00420C, sc[k].hdma ? 0x10 : 0x00);
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

/* the colour column x (the screen's) should have in scene k */
static uint32_t want(int k, int x)
{
    int hx = x - 32, cx = hx, in, shown;
    if (sc[k].open && hx < 0 && sc[k].left == 0)
        cx = 0;
    if (sc[k].open && hx > 255)
        cx = 255;
    in = cx >= sc[k].left && cx <= 255;
    shown = sc[k].inv ? in : !in;   /* not masked */
    if (sc[k].obj)
        return shown && hx >= SPRITE_X && hx < SPRITE_X + 8 ? BLUE : RED;
    return shown ? GREEN : RED;
}

int main(int argc, char **argv)
{
    nshvc_cart cart;
    int seen = 0, fails = 0;
    memset(&cart, 0, sizeof cart);
    cart.title = "window_edges";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    if (argc > 1)
        return nshvc_main(&cart, argc, argv);
    nshvc_power(&cart);
    while (seen < NSCENES) {
        uint8_t id;
        nshvc_run_frame();
        id = nshvc_wram()[0x100];
        if (id >= 1 && id <= NSCENES && id == seen + 1) {
            int pw, ph, x, bad = 0, sprite = 0;
            const uint32_t *px = nshvc_picture(&pw, &ph);
            for (x = 0; x < pw; x++) {
                uint32_t got = px[(size_t)ROW * (size_t)pw + (size_t)x], exp = want(id - 1, x);
                sprite += got == BLUE;
                if (got != exp) {
                    if (!bad)
                        printf("  first at column %d: got %08x want %08x\n", x, got, exp);
                    bad++;
                }
            }
            printf("%s scene %d (%dx%d): %d columns differ%s\n", bad || pw != 320 ? "FAIL" : "ok  ", id - 1, pw, ph, bad,
                   sc[id - 1].obj ? (sprite ? ", the sprite shown" : ", the sprite masked") : "");
            fails += bad || pw != 320;
            seen++;
        }
        if (nshvc_frames() > 100) {
            printf("FAIL: the scenes did not all show (%d)\n", seen);
            return 1;
        }
    }
    printf("window_edges: %d scenes, %d failed\n", seen, fails);
    return fails ? 1 : 0;
}
