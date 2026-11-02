/*
 * window_edges.c: the windows on a larger screen (nshvc_ext_window_edges),
 * on a cartridge of a few lines of C: BG1 one colour on a 320-wide screen
 * (the console's picture at columns 32-287), window 1 masking it on the
 * main screen, the backdrop another colour. For each scene, every column
 * of a line against the colour worked out here from the rule:
 *
 *   0  edges shut, window 0-255 inverted: BG1 inside the picture only
 *   1  edges open, the same: BG1 across the screen
 *   2  edges open, window 10-255 inverted: the left past the picture
 *      masked (its edge is not the picture's), the right open
 *   3  edges open, window 0-255: masked across the screen
 *   4  edges shut, window 0-255: BG1 past the picture only
 *
 * Exit 0 when every column holds. With arguments, the scenes run through
 * the front end instead (nshvc_main), for NSHVC_COMPOSECHECK=1 (the
 * compositor against the frame from its record) and NSHVC_RRCHECK=1.
 */
#include <stdio.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

#define NSCENES 5
#define GREEN 0xFF00FF00u   /* BG1: colour 1, 0x03E0 */
#define RED 0xFFFF0000u     /* the backdrop: colour 0, 0x001F */

static void w(unsigned reg, unsigned v) { wr8(0x2100 + reg, (uint8_t)v); }

static void game(void)
{
    static const struct { int open; unsigned sel, left; } sc[NSCENES] = {
        { 0, 0x03, 0 }, { 1, 0x03, 0 }, { 1, 0x03, 10 }, { 1, 0x02, 0 }, { 0, 0x02, 0 },
    };
    int k, i;
    wr8(0x004200, 0x00);
    w(0x00, 0x80);
    nshvc_ext_screen(320, 224);
    /* tile 1: every pixel colour 1 (2 bpp, low plane set); the map at word
       0x400, every entry tile 1 */
    w(0x15, 0x80);
    w(0x16, 0x08);
    w(0x17, 0x00);
    for (i = 0; i < 8; i++) {
        w(0x18, 0xFF);
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
    w(0x05, 0x00);   /* mode 0 */
    w(0x07, 0x04);   /* BG1's map at word 0x400 */
    w(0x0B, 0x00);
    w(0x2C, 0x01);   /* BG1 on the main screen */
    w(0x2E, 0x01);   /* windowed there */
    w(0x27, 0xFF);
    for (k = 0; k < NSCENES; k++) {
        w(0x00, 0x80);
        nshvc_ext_window_edges(sc[k].open);
        w(0x23, sc[k].sel);
        w(0x26, sc[k].left);
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
    int hx = x - 32, left = k == 2 ? 10 : 0, open = k == 1 || k == 2 || k == 3, inv = k <= 2, in;
    int cx = hx;
    if (open && hx < 0 && left == 0)
        cx = 0;
    if (open && hx > 255)
        cx = 255;
    in = cx >= left && cx <= 255;
    return (inv ? !in : in) ? RED : GREEN;   /* masked: the backdrop */
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
            int pw, ph, x, bad = 0;
            const uint32_t *px = nshvc_picture(&pw, &ph);
            for (x = 0; x < pw; x++) {
                uint32_t got = px[(size_t)(ph / 2) * (size_t)pw + (size_t)x], exp = want(id - 1, x);
                if (got != exp) {
                    if (!bad)
                        printf("  first at column %d: got %08x want %08x\n", x, got, exp);
                    bad++;
                }
            }
            printf("%s scene %d (%dx%d): %d columns differ\n", bad || pw != 320 ? "FAIL" : "ok  ", id - 1, pw, ph, bad);
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
