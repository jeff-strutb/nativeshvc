/*
 * canvas32_psp.c: a full-colour screen canvas (nshvc_ext_canvas_screen32)
 * over the PSP's whole screen, a texel a screen pixel (k 0, 480 x 272
 * texels), presented by the PSP's renderer: each frame against the
 * reference's drawing with NSHVC_RRCHECK=1 (src/front/console.c, within 1
 * a channel), and the frame's time, a scene at a time. The console's
 * picture 256 x 224 (320 x 240 with CANVAS32_SCREEN), BG1 the canvas (4 bpp, palette 5) over BG2's tiles;
 * its texels: 0 (clear), an index, or a colour 0xFFrrggbb at random:
 *
 *   plain      no colour maths
 *   fade       brightness 9
 *   clip       the colour window (window 2, columns 200-255) clipping to
 *              black inside, no maths
 *   fixed      BG1 adding the fixed colour, halved
 *   fixedsub   BG1 subtracting the fixed colour (HDMA's, each line)
 *   addsub     BG1 on the main screen adding BG2 on the sub screen, halved,
 *              the backdrop too, at brightness 11
 *   onsub      the canvas on the sub screen: BG2 and the backdrop adding it
 *   both       the canvas on both screens, the main adding the sub, halved
 *   prevented  maths on BG1 and BG2 but prevented everywhere (CGWSEL $32);
 *              BG1 masked by window 2 (its edges HDMA's, each line), BG2 by
 *              window 1 (nshvc_ext_window_columns); the canvas's colour 1
 *              (CGRAM 81) rewritten each line by HDMA
 *
 *   canvas32_psp --headless --frames 64      (with NSHVC_RRCHECK=1)
 *
 * Prints each scene's time a frame; the renderer check's report at exit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef PSP
#include <pspkernel.h>
#include <psprtc.h>
static uint64_t now_us(void) { return sceKernelGetSystemTimeWide(); }
#else
#include <time.h>
static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u;
}
#endif

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

/* the console's picture: its own 256 x 224, or (CANVAS32_SCREEN) a larger
   screen (nshvc_ext_screen) */
#ifdef CANVAS32_SCREEN
#define SC_W 320
#define SC_H 240
#else
#define SC_W 256
#define SC_H 224
#endif
#define TEX_W 480
#define TEX_H 272
#define PAL 5
#define FRAMES 8                       /* frames a scene */

enum { S_PLAIN, S_FADE, S_CLIP, S_FIXED, S_FIXEDSUB, S_ADDSUB, S_ONSUB, S_BOTH, S_PREVENTED, NSCENES };
static const char *const scene_name[NSCENES] = { "plain", "fade", "clip", "fixed", "fixedsub", "addsub", "onsub", "both", "prevented" };

static unsigned rng = 4711;
static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}
static void w(unsigned reg, unsigned v) { wr8(0x2100 + reg, (uint8_t)v); }

static uint32_t tx[TEX_W * TEX_H];

/* HDMA, channel 0: COLDATA's red and green each line, channel 2 its blue */
static void hdma_tables(void)
{
    uint32_t a = 0x7E2000, c = 0x7E2800;
    int half, i;
    for (half = 0; half < 2; half++) {
        wr8(a++, 0xF0);
        wr8(c++, 0xF0);
        for (i = 0; i < 112; i++) {
            unsigned line = (unsigned)(half * 112 + i);
            wr8(a++, (uint8_t)(0x20 | ((line * 5u + 3u) & 31)));
            wr8(a++, (uint8_t)(0x40 | ((line * 11u + 7u) & 31)));
            wr8(c++, (uint8_t)(0x80 | ((line * 3u + 20u) & 31)));
        }
    }
    wr8(a, 0);
    wr8(c, 0);
}

/* HDMA, channels 1-3: CGADD (81) and CGDATA each line, WH2 and WH3 each line */
static void hdma_prevented_tables(void)
{
    uint32_t a = 0x7E3000, c = 0x7E3200, wh = 0x7E3600;
    int half, i;
    for (half = 0; half < 2; half++) {
        wr8(a++, 0xF0);
        wr8(c++, 0xF0);
        wr8(wh++, 0xF0);
        for (i = 0; i < 112; i++) {
            unsigned line = (unsigned)(half * 112 + i), col = (line * 37u + 5u) & 0x7FFFu;
            wr8(a++, 81);
            wr8(c++, (uint8_t)(col & 0xFF));
            wr8(c++, (uint8_t)(col >> 8));
            wr8(wh++, (uint8_t)(20u + (line * 3u) % 90u));
            wr8(wh++, (uint8_t)(130u + (line * 7u) % 120u));
        }
    }
    wr8(a, 0);
    wr8(c, 0);
    wr8(wh, 0);
}

static void hdma_prevented(void)
{
    static const uint8_t ch[3][5] = { { 0x00, 0x21, 0x00, 0x30, 0x7E }, { 0x02, 0x22, 0x00, 0x32, 0x7E }, { 0x01, 0x28, 0x00, 0x36, 0x7E } };
    int k, i;
    hdma_prevented_tables();
    for (k = 0; k < 3; k++)
        for (i = 0; i < 5; i++)
            wr8(0x004310 + 0x10u * (uint32_t)k + (uint32_t)i, ch[k][i]);
    wr8(0x00420C, 0x0E);
}

static void hdma(int on)
{
    static const uint8_t ch[2][5] = { { 0x02, 0x32, 0x00, 0x20, 0x7E }, { 0x00, 0x32, 0x00, 0x28, 0x7E } };
    int i;
    for (i = 0; i < 5; i++) {
        wr8(0x004300 + (uint32_t)i, ch[0][i]);
        wr8(0x004320 + (uint32_t)i, ch[1][i]);
    }
    wr8(0x00420C, (uint8_t)(on ? 0x05 : 0));
}

static void setup(int sc)
{
    int i;
    hdma(0);
    nshvc_ext_window_columns(1, -1, 0);
#ifdef CANVAS32_SCREEN
    nshvc_ext_screen(SC_W, SC_H);
#endif
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
        unsigned c = rnd() & 0x7FFF;
        w(0x22, c & 0xFF);
        w(0x22, c >> 8);
    }
    w(0x05, 0x01);
    w(0x06, 0x00);
    w(0x07, 0x00);
    w(0x08, 0x08);
    w(0x0B, 0x40);
    for (i = 0x23; i <= 0x31; i++)
        w((unsigned)i, 0);
    w(0x32, 0xE0);
    w(0x33, 0);
    switch (sc) {
    case S_PLAIN:
    case S_FADE:
        w(0x2C, 0x03);
        break;
    case S_CLIP:
        w(0x2C, 0x03);
        w(0x28, 200);
        w(0x29, 255);
        w(0x25, 0x80);                 /* window 2 the colour window */
        w(0x30, 0x80);                 /* clip to black inside it */
        break;
    case S_FIXED:
        w(0x2C, 0x03);
        w(0x31, 0x41);                 /* BG1 adds the fixed colour, halved */
        w(0x32, 0x20 | 9);
        w(0x32, 0x40 | 20);
        w(0x32, 0x80 | 4);
        break;
    case S_FIXEDSUB:
        w(0x2C, 0x01);
        w(0x31, 0x81);                 /* BG1 subtracts the fixed colour */
        hdma(1);
        break;
    case S_ADDSUB:
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
    case S_BOTH:
        w(0x2C, 0x03);
        w(0x2D, 0x01);
        w(0x30, 0x02);
        w(0x31, 0x41);
        break;
    case S_PREVENTED:
        w(0x2C, 0x03);
        w(0x2E, 0x03);                 /* BG1, BG2 windowed on the main screen */
        w(0x23, 0x28);                 /* BG1 window 2, BG2 window 1 */
        w(0x30, 0x32);                 /* maths prevented everywhere, the sub screen's */
        w(0x31, 0x03);
        nshvc_ext_window_columns(1, 60, 190);
        hdma_prevented();
        break;
    }
}

static void make_texels(void)
{
    size_t i;
    for (i = 0; i < (size_t)TEX_W * TEX_H; i++) {
        unsigned v = rnd() & 15, r = rnd() & 3;
#ifdef CANVAS32_INDICES
        (void)r;
        tx[i] = v;                     /* (indices alone: the canvas path as before) */
#else
        tx[i] = !v || r == 0 ? 0u : r == 1 ? v : 0xFF000000u | (rnd() & 0xFFFFFFu);
#endif
    }
}

static void game(void)
{
    int sc, f;
    wr8(0x004200, 0x00);
    hdma_tables();
    for (sc = 0; sc < NSCENES; sc++) {
        uint64_t t0, lo, hi;
        w(0x00, 0x80);
        setup(sc);
        make_texels();
        nshvc_ext_canvas_screen32(0, tx, TEX_W, TEX_H, 0, sc == S_PLAIN ? 0 : 1, PAL);
        w(0x00, sc == S_FADE ? 0x09 : sc == S_ADDSUB ? 0x0B : 0x0F);
        nshvc_wait_vblank();
        t0 = now_us();
        lo = ~0ull;
        hi = 0;
        for (f = 0; f < FRAMES; f++) {
            uint64_t t1 = now_us(), dt;
            nshvc_wait_vblank();
            dt = now_us() - t1;
            lo = dt < lo ? dt : lo;
            hi = dt > hi ? dt : hi;
        }
        printf("canvas32_psp %-9s %llu us a frame (%llu-%llu)\n", scene_name[sc], (unsigned long long)((now_us() - t0) / FRAMES),
               (unsigned long long)lo, (unsigned long long)hi);
    }
    for (;;)
        nshvc_wait_vblank();
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, game, "reset", 0 } };

int main(int argc, char **argv)
{
    nshvc_cart cart;
    memset(&cart, 0, sizeof cart);
    cart.title = "canvas32_psp";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    return nshvc_main(&cart, argc, argv);
}
