/*
 * untimed.c: nshvc_set_untimed, nshvc_wait_vblank and the untimed guard,
 * checked on a cartridge of a few lines of C.
 *
 *   untimed           runs the checks; exit 0 when every one holds
 *   untimed --poll    an untimed loop polling a status bit: must stop the
 *                     run (exit 70)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"

extern uint64_t snes_clock;

static int fails;
static int poll_mode;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

/* the NMI's handler: untimed set in it, then interrupts held in it */
static int nmi_seen;

static void abi_nmi(void)
{
    uint64_t t0;
    nmi_seen = 1;
    nshvc_set_untimed(1);
    check(nshvc_untimed() == 0, "untimed set in a handler: the handler stays timed");
    t0 = snes_clock;
    cyc(1000);
    check(snes_clock - t0 >= 1000, "untimed in a handler: its stated clocks take their time");
    nshvc_hold_interrupts(1);
    check(nshvc_untimed() == 1, "interrupts held in a handler: its code is untimed");
    t0 = snes_clock;
    cyc(1000);
    check(snes_clock == t0, "interrupts held in a handler: its stated clocks take none");
    nshvc_hold_interrupts(0);
    nshvc_set_untimed(0);
}

static void abi_reset(void)
{
    uint64_t t0, t1;
    uint32_t f0;
    int i;
    wr8(0x004200, 0x00);
    /* timed: a thousand accesses take their time */
    t0 = snes_clock;
    for (i = 0; i < 1000; i++)
        wr8(0x7E1000u + (unsigned)i, (uint8_t)i);
    t1 = snes_clock;
    /* 8,000 clocks, and the work RAM's refresh (40 a line) in the 6 or 7
       lines they cross */
    check(t1 - t0 == 8000 + 40 * 6 || t1 - t0 == 8000 + 40 * 7,
          "timed: 1000 work RAM writes take 8,000 master clocks and the lines' refresh");
    /* untimed: the same take none, and the values land */
    nshvc_set_untimed(1);
    check(nshvc_untimed() == 1, "nshvc_untimed reports on");
    t0 = snes_clock;
    f0 = nshvc_frames();
    for (i = 0; i < 1000; i++) {
        wr8(0x7E2000u + (unsigned)i, (uint8_t)(i ^ 0x5A));
        cyc(1000);
    }
    check(snes_clock == t0, "untimed: 1000 writes and 1,000,000 stated clocks take none");
    check(nshvc_frames() == f0, "untimed: no frame passes");
    check(rd8(0x7E2000 + 7) == (7 ^ 0x5A), "untimed: the writes land");
    /* the vertical blank waited for, timed, untimed put back after */
    nshvc_wait_vblank();
    check(nshvc_untimed() == 1, "nshvc_wait_vblank puts untimed back");
    check(nshvc_frames() == f0 + 1, "nshvc_wait_vblank: the frame ends there");
    nshvc_set_untimed(0);
    t0 = snes_clock;
    nshvc_wait_vblank();
    t1 = snes_clock;
    check(t1 - t0 >= 262 * 1364 - 64 && t1 - t0 <= 262 * 1364 + 64, "timed: a vertical blank to the next is a frame");
    /* the NMI's handler, timed but for what runs with interrupts held */
    wr8(0x004200, 0x80);
    nshvc_wait_vblank();
    for (i = 0; i < 100000 && !nmi_seen; i++) {
        cpu_boundary();
        rd8(0x7E0000);
    }
    wr8(0x004200, 0x00);
    check(nmi_seen, "the NMI's handler ran");
    check(nshvc_untimed() == 0, "untimed off again after the handler");
    if (poll_mode) {
        /* in the vertical blank, untimed: the blank never ends */
        nshvc_set_untimed(1);
        while (rd8(0x004212) & 0x80)
            ;
    }
    printf("%d check(s) failed\n", fails);
    exit(fails ? 1 : 0);
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FFA] = 0x00, [0x7FFB] = 0x90,
                                      [0x7FEA] = 0x00, [0x7FEB] = 0x90, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = { { 0x008000, abi_reset, "reset", 0 }, { 0x009000, abi_nmi, "nmi", 0 } };

int main(int argc, char **argv)
{
    nshvc_cart cart;
    poll_mode = argc > 1 && !strcmp(argv[1], "--poll");
    memset(&cart, 0, sizeof cart);
    cart.title = "untimed";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 2;
    nshvc_power(&cart);
    for (;;)
        nshvc_run_frame();
}
