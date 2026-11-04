/*
 * ge_vram_check.c: the GPU memory lent to the game (nshvc_ext_ge_vram,
 * contract 2.15) on the PSP, read back on the system, through the front
 * end and its renderer. A cartridge scrolls BG1 over random tiles; each
 * frame its game, while it runs, takes the loan and
 *
 *   lent     557,056 bytes, 8 KB aligned, inside VRAM, not the buffer set
 *            to show
 *   drawn    a list of its own on the GE into it (a 16-bit 4444 target):
 *            a clear and rectangles of known colours, synced, read back
 *            every texel's colour exact (its 12 bits: the alpha nibble is
 *            the stencil)
 *   left     the GE's state wrong for the renderer at the end of it
 *            (blending, a logic op, the colour test, the pixel mask, the
 *            scissor, the offset): the renderer's frames must not see it
 *
 * Run with NSHVC_RRCHECK=1: every presented frame against the reference
 * (front/console.c rr_check). "gevram: N frames, M check(s) failed" at the
 * end, then rrcheck's own line.
 *
 *   ge_vram_check --headless --frames N
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

#define LENT 557056
#define TW 448                         /* the target drawn: TW x TH at a stride of 512 */
#define TH 224

static int fails;
static long frames;
static unsigned int __attribute__((aligned(64))) s_list[4096];

static void check(int ok, const char *what)
{
    if (!ok) {
        if (fails < 20)
            printf("FAIL frame %ld: %s\n", frames, what);
        fails++;
    }
}

static unsigned rng = 77;
static unsigned rnd(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 8;
}
static void w(unsigned reg, unsigned v) { wr8(0x2100 + reg, (uint8_t)v); }

/* a rectangle's colour (4444, 0xABGR) in frame f, k 0-3 */
static uint16_t colour(long f, int k) { return (uint16_t)(0xF000u | (unsigned)((f * 5 + k * 3) & 0xFFF)); }
static void rect(int k, int *x, int *y, int *ww, int *hh)
{
    *x = 13 + k * 97;
    *y = 7 + k * 41;
    *ww = 61 + k * 9;
    *hh = 29 + k * 17;
}

typedef struct {
    uint16_t c;
    int16_t x, y, z;
} V;

static void draw_lent(void)
{
    size_t n = 0;
    void *p = nshvc_ext_ge_vram(&n);
    uintptr_t base = (uintptr_t)sceGeEdramGetAddr(), a = (uintptr_t)p, off = a - base;
    void *top = NULL;
    int bw, fmt, k, x, y;
    uint16_t back = (uint16_t)(0x8000u | (unsigned)(frames * 11 & 0x7FF));
    const uint16_t *t;
    long bad = 0;
    sceDisplayGetFrameBuf(&top, &bw, &fmt, PSP_DISPLAY_SETBUF_NEXTFRAME);
    check(p != NULL && n == LENT, "the loan: 557,056 bytes");
    if (!p || n != LENT)
        return;
    check(off % 8192 == 0, "the loan 8 KB aligned");
    check(a >= base && off + n <= 2u * 1024 * 1024, "the loan inside VRAM");
    check((uintptr_t)top != a, "the loan not the buffer set to show");
    /* the game's own list: a 4444 target, a clear and four rectangles */
    sceGuStart(GU_DIRECT, s_list);
    sceGuDrawBufferList(GU_PSM_4444, (void *)off, 512);
    sceGuOffset(2048 - TW / 2, 2048 - TH / 2);
    sceGuViewport(2048, 2048, TW, TH);
    sceGuScissor(0, 0, TW, TH);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_COLOR_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_COLOR_LOGIC_OP);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DITHER);
    sceGuPixelMask(0);
    sceGuShadeModel(GU_FLAT);
    /* (the clear's colour 32-bit ABGR: back's nibbles each widened) */
    sceGuClearColor((unsigned)(back & 0xF) * 0x11u | (unsigned)(back >> 4 & 0xF) * 0x1100u | (unsigned)(back >> 8 & 0xF) * 0x110000u |
                    (unsigned)(back >> 12 & 0xF) * 0x11000000u);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    for (k = 0; k < 4; k++) {
        int rx, ry, rw, rh;
        V *v = (V *)sceGuGetMemory(2 * sizeof(V));
        rect(k, &rx, &ry, &rw, &rh);
        v[0].c = v[1].c = colour(frames, k);
        v[0].x = (int16_t)rx;
        v[0].y = (int16_t)ry;
        v[1].x = (int16_t)(rx + rw);
        v[1].y = (int16_t)(ry + rh);
        v[0].z = v[1].z = 0;
        sceGuDrawArray(GU_SPRITES, GU_COLOR_4444 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, 0, v);
    }
    /* then the GE left wrong for anyone after */
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0x00404040, 0x00404040);
    sceGuEnable(GU_COLOR_LOGIC_OP);
    sceGuLogicalOp(GU_XOR);
    sceGuEnable(GU_COLOR_TEST);
    sceGuColorFunc(GU_EQUAL, 0x123456, 0xFFFFFF);
    sceGuPixelMask(0x00FF00FF);
    sceGuScissor(5, 5, 9, 9);
    sceGuOffset(1000, 1000);
    sceGuFinish();
    sceGuSync(0, 0);
    /* every texel read back */
    t = (const uint16_t *)(a | 0x40000000u);
    for (y = 0; y < TH; y++)
        for (x = 0; x < TW; x++) {
            uint16_t want = back;
            for (k = 0; k < 4; k++) {
                int rx, ry, rw, rh;
                rect(k, &rx, &ry, &rw, &rh);
                if (x >= rx && x < rx + rw && y >= ry && y < ry + rh)
                    want = colour(frames, k);
            }
            /* (the colours' 12 bits: a 4444 target's alpha is the stencil,
               written by the GE's own stencil rules) */
            if (((t[(size_t)y * 512 + (size_t)x] ^ want) & 0x0FFFu) && !bad++ && fails < 3)
                printf("  first at (%d, %d): got %04x want %04x\n", x, y, t[(size_t)y * 512 + (size_t)x], want);
        }
    check(bad == 0, "the target's texels read back");
}

static void game(void)
{
    int i;
    wr8(0x004200, 0x00);
    w(0x00, 0x80);
    w(0x15, 0x80);
    w(0x16, 0);
    w(0x17, 0);
    for (i = 0; i < 0x8000; i++) {
        unsigned v = i < 0x400 ? (rnd() % 512u) | (rnd() & 0xDC00u) : rnd() & 0xFFFF;
        w(0x18, v & 0xFF);
        w(0x19, v >> 8);
    }
    w(0x21, 0);
    for (i = 0; i < 512; i++)
        w(0x22, rnd() & (i & 1 ? 0x7F : 0xFF));
    w(0x05, 0x01);
    w(0x07, 0x00);
    w(0x0B, 0x01);
    w(0x2C, 0x01);
    w(0x00, 0x0F);
    for (;;) {
        nshvc_wait_vblank();
        w(0x0D, (unsigned)(frames * 3) & 0xFF);
        w(0x0D, 0);
        w(0x0E, (unsigned)frames & 0xFF);
        w(0x0E, 0);
        draw_lent();
        frames++;
    }
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, game, "reset", 0 } };

static void report(void) { printf("gevram: %ld frames, %d check(s) failed\n", frames, fails); }

int main(int argc, char **argv)
{
    nshvc_cart cart;
    memset(&cart, 0, sizeof cart);
    cart.title = "ge_vram_check";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    atexit(report);
    return nshvc_main(&cart, argc, argv);
}
