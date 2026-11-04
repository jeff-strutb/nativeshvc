/*
 * bus_cost.c: what the S-CPU's accesses cost on the PSP, by its own clock
 * (sceKernelGetSystemTimeWide), each kind timed over many: a read and a
 * write of WRAM, a read of ROM, a word, a read-modify-write, a write of
 * the sound unit's port (its sync), a DMA to VRAM in the display (the
 * timed path) and in forced blank (the direct one). The display on, BG1
 * shown, so the frame's events run as a game's do. Prints one line a
 * kind: "buscost KIND: N in T us, P ns each".
 *
 *   bus_cost --headless
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psprtc.h>
#include <pspkernel.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

static uint64_t now(void) { return sceKernelGetSystemTimeWide(); }

static void report(const char *kind, long n, uint64_t t0, uint64_t t1)
{
    long us = (long)(t1 - t0);
    printf("buscost %s: %ld in %ld us, %ld ns each\n", kind, n, us, n ? us * 1000 / n : 0);
}

#define N 200000L

static void dma_vram(int forced)
{
    long i, reps = 50;
    uint64_t t0, t1;
    wr8(0x2100, forced ? 0x80 : 0x0F);
    t0 = now();
    for (i = 0; i < reps; i++) {
        wr8(0x2115, 0x80);
        wr8(0x2116, 0);
        wr8(0x2117, 0);
        wr8(0x4300, 0x01);
        wr8(0x4301, 0x18);
        wr8(0x4302, 0x00);
        wr8(0x4303, 0x00);
        wr8(0x4304, 0x7F);
        wr8(0x4305, 0x00);
        wr8(0x4306, 0x10); /* 4,096 bytes */
        wr8(0x420B, 0x01);
    }
    t1 = now();
    report(forced ? "dma 4096 B forced blank" : "dma 4096 B display", reps, t0, t1);
    printf("buscost %s per byte: %ld ns\n", forced ? "dma forced" : "dma display", (long)(t1 - t0) * 1000 / (reps * 4096));
    wr8(0x2100, 0x0F);
}

static void game(void)
{
    long i;
    volatile uint32_t sink = 0;
    uint64_t t0, t1;
    wr8(0x4200, 0x00);
    wr8(0x2100, 0x80);
    wr8(0x2105, 0x01);
    wr8(0x212C, 0x01);
    wr8(0x2100, 0x0F);
    nshvc_wait_vblank();

    /* the console's own frame, no access in it: what each timing below
       carries a frame of its time (8 clocks an access: 44,700 a frame) */
    t0 = now();
    for (i = 0; i < 20; i++)
        nshvc_wait_vblank();
    t1 = now();
    report("frame, waited", 20, t0, t1);

    t0 = now();
    for (i = 0; i < N; i++)
        sink += (uint32_t)i;
    t1 = now();
    report("empty loop", N, t0, t1);

    t0 = now();
    for (i = 0; i < N; i++)
        sink += rd8(0x7E0100 + (uint32_t)(i & 0xFF));
    t1 = now();
    report("rd8 wram", N, t0, t1);

    t0 = now();
    for (i = 0; i < N; i++)
        wr8(0x7E0100 + (uint32_t)(i & 0xFF), (uint8_t)i);
    t1 = now();
    report("wr8 wram", N, t0, t1);

    t0 = now();
    for (i = 0; i < N; i++)
        sink += rd8(0x008000 + (uint32_t)(i & 0xFF));
    t1 = now();
    report("rd8 rom", N, t0, t1);

    t0 = now();
    for (i = 0; i < N; i++)
        sink += rd16(0x7E0100 + (uint32_t)(i & 0xFE));
    t1 = now();
    report("rd16 wram", N, t0, t1);

    t0 = now();
    for (i = 0; i < N; i++) {
        uint8_t v = rd8_rmw(0x7E0100 + (uint32_t)(i & 0xFF));
        wr8(0x7E0100 + (uint32_t)(i & 0xFF), (uint8_t)(v + 1));
    }
    t1 = now();
    report("rmw wram", N, t0, t1);

    t0 = now();
    for (i = 0; i < N / 10; i++)
        wr8(0x2140, (uint8_t)(i & 1)); /* (never $CC: the boot ROM's transfer) */
    t1 = now();
    report("wr8 apu port", N / 10, t0, t1);

    t0 = now();
    for (i = 0; i < N / 10; i++)
        sink += rd8(0x2140);
    t1 = now();
    report("rd8 apu port", N / 10, t0, t1);

    dma_vram(0);
    dma_vram(1);
    printf("buscost done %u\n", (unsigned)sink);
    exit(0);
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, game, "reset", 0 } };

int main(int argc, char **argv)
{
    nshvc_cart cart;
    memset(&cart, 0, sizeof cart);
    cart.title = "bus_cost";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 1;
    return nshvc_main(&cart, argc, argv);
}
