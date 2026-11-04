/*
 * woven_check.c: interlaced frames presented (an interlaced frame woven
 * with the frame before it, ppu.c ppu_woven), held to the reference with
 * NSHVC_RRCHECK=1 on each renderer. Mode 1, BG1 and BG2's tiles at random,
 * sprites over them, SETINI's interlace on; scenes:
 *
 *   steady     interlaced throughout, the scroll moving each frame
 *   switch     interlace off for a frame, then on again (the frame before
 *              not interlaced: both its parities' rows)
 *   held       a frame where the game waits two vblanks (a parity's rows
 *              older than the frame before)
 *
 *   woven_check --headless --frames 60      (with NSHVC_RRCHECK=1)
 */
#include <stdio.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

static unsigned rng = 1234;
static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}
static void w(unsigned reg, unsigned v) { wr8(0x2100 + reg, (uint8_t)v); }

static void setup(void)
{
    int i;
    w(0x00, 0x80);
    w(0x15, 0x80);
    w(0x16, 0);
    w(0x17, 0);
    for (i = 0; i < 0x8000; i++) {
        unsigned v = rnd() & 0xFFFF;
        w(0x18, v & 0xFF);
        w(0x19, v >> 8);
    }
    w(0x21, 0);
    for (i = 0; i < 256; i++) {
        unsigned c = rnd() & 0x7FFF;
        w(0x22, c & 0xFF);
        w(0x22, c >> 8);
    }
    w(0x01, 0x00);
    w(0x02, 0);
    w(0x03, 0);
    for (i = 0; i < 128; i++) {
        w(0x04, rnd() & 0xFF);
        w(0x04, rnd() % 220u);
        w(0x04, rnd() & 0xFF);
        w(0x04, (rnd() & 0x3E) | 0x30);
    }
    for (i = 0; i < 32; i++)
        w(0x04, 0);
    w(0x05, 0x01);
    w(0x07, 0x00);
    w(0x08, 0x08);
    w(0x0B, 0x40);
    w(0x2C, 0x13);
    w(0x33, 0x01);                     /* interlace */
    w(0x00, 0x0F);
}

static void game(void)
{
    int f;
    wr8(0x004200, 0x00);
    setup();
    for (f = 0;; f++) {
        w(0x0D, (unsigned)f & 0xFF);
        w(0x0D, 0);
        if (f % 20 == 7)
            w(0x33, 0x00);             /* switch: off a frame */
        if (f % 20 == 8)
            w(0x33, 0x01);
        nshvc_wait_vblank();
        if (f % 20 == 14)
            nshvc_wait_vblank();       /* held */
    }
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, game, "reset", 0 } };

int main(int argc, char **argv)
{
    nshvc_cart cart;
    memset(&cart, 0, sizeof cart);
    cart.title = "woven_check";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    return nshvc_main(&cart, argc, argv);
}
