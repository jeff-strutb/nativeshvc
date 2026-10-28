/*
 * waits.c: the S-CPU's time (machine.h): the C's work takes none; the
 * clock moves on in its waits, each change the wait looks for seen at its
 * own clock, the interrupts taken at theirs, and in a DMA, for the DMA
 * unit's real time; the multiplier and divider done when read. Checked on
 * a cartridge of a few lines of C, against clocks worked out here from
 * the console's timing (src/console/bus.c).
 *
 *   waits             runs the checks; exit 0 when every one holds
 *   waits --poll      a loop polling a status bit with no wait: must stop
 *                     the run (exit 70)
 *   waits --held      a wait with interrupts held: must stop the run
 *                     (exit 70)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/machine.h"
#include "snes.h"

static int fails;
static const char *mode = "";

static void check(int ok, const char *what, long long got, long long want)
{
    printf("%s %s (%lld, want %lld)\n", ok ? "ok  " : "FAIL", what, got, want);
    fails += !ok;
}

/* the line's start, the clock now less the line's clocks */
static uint64_t line_start(void) { return snes_clock - snes_line_clock(); }

/* ---- the conditions waited on ---- */
static int hblank(void *ctx) { (void)ctx; return (rd8(0x004212) & 0x40) != 0; }
static int not_hblank(void *ctx) { (void)ctx; return !(rd8(0x004212) & 0x40); }
static int joy_done(void *ctx) { (void)ctx; return !(rd8(0x004212) & 0x01); }
static int joy_busy(void *ctx) { (void)ctx; return (rd8(0x004212) & 0x01) != 0; }
static int not_vblank(void *ctx) { (void)ctx; return !(rd8(0x004212) & 0x80); }
static int flag_set(void *ctx) { return *(volatile int *)ctx != 0; }

/* ---- the handlers ---- */
static int nmis, irqs, nmi_wait;
static uint64_t nmi_at, irq_at, nmi_waited_to, nmi_irq_seen;

static void abi_nmi(void)
{
    nmis++;
    nmi_at = snes_clock;
    rd8(0x004210);
    if (nmi_wait) {
        /* a wait in the handler, the IRQ due in it held by the I flag */
        nshvc_wait_until(not_hblank, NULL);
        nshvc_wait_until(hblank, NULL);
        nmi_waited_to = snes_clock;
        nmi_irq_seen = irqs;
    }
}

static void abi_irq(void)
{
    irqs++;
    irq_at = snes_clock;
    rd8(0x004211);
}

static void abi_reset(void)
{
    uint64_t t0, t1, w, ls, want;
    uint32_t f0;
    int i;
    cpu.e = 0;
    wr8(0x004200, 0x00);

    if (!strcmp(mode, "--poll")) {
        /* no wait: the vertical blank never comes */
        while (!(rd8(0x004212) & 0x80))
            ;
    }
    if (!strcmp(mode, "--held")) {
        nshvc_hold_interrupts(1);
        nshvc_wait_vblank();
    }

    /* the C's work takes no time, and its writes land */
    t0 = snes_clock;
    f0 = nshvc_frames();
    for (i = 0; i < 100000; i++)
        wr8(0x7E2000u + (unsigned)(i & 0xFFF), (uint8_t)(i ^ 0x5A));
    check(snes_clock == t0, "100,000 writes take no time", (long long)(snes_clock - t0), 0);
    check(nshvc_frames() == f0, "no frame passes in them", nshvc_frames() - f0, 0);
    check(rd8(0x7E2000 + 7) == ((4096 * 24 + 7) ^ 0x5A) % 256, "the writes land", rd8(0x7E2007),
          ((4096 * 24 + 7) ^ 0x5A) % 256);

    /* the vertical blank's start: line 225's first clock */
    nshvc_wait_vblank();
    check(snes_vcounter() == 225 && snes_line_clock() == 0, "nshvc_wait_vblank ends at line 225's start",
          (long long)snes_vcounter() * 10000 + snes_line_clock(), 2250000);
    t0 = snes_clock;
    nshvc_wait_vblank();
    t1 = snes_clock;
    check(t1 - t0 == 262 * 1364 || t1 - t0 == 262 * 1364 - 4, "one vertical blank to the next is a frame",
          (long long)(t1 - t0), 262 * 1364);

    /* HVBJOY's horizontal blank: seen at its first clock (1,092 into the
       line: the counters' dot 274), the clocks waited returned */
    nshvc_wait_until(not_hblank, NULL);
    t0 = snes_clock;
    w = nshvc_wait_until(hblank, NULL);
    check(snes_line_clock() == 1092, "a wait for the horizontal blank ends at its start", snes_line_clock(), 1092);
    check(w == snes_clock - t0, "nshvc_wait_until returns the clocks waited", (long long)w,
          (long long)(snes_clock - t0));
    w = nshvc_wait_until(hblank, NULL);
    check(w == 0, "a condition that holds already: no wait", (long long)w, 0);

    /* the joypads' read: it starts on its step of 256 in dots 130-384 of
       line 225 and runs 33 edges of 128 clocks; the wait for its end ends
       there */
    wr8(0x004200, 0x01);
    nshvc_wait_vblank();
    ls = line_start();
    {
        uint64_t h0 = 130 + ((256 - ((ls + 130) & 255)) & 255);
        want = ls - 4 + h0 + 33 * 128;
    }
    nshvc_wait_until(joy_busy, NULL);
    nshvc_wait_until(joy_done, NULL);
    check(snes_clock == want, "a wait for the joypads' read ends as it does", (long long)(snes_clock - ls),
          (long long)(want - ls));
    wr8(0x004200, 0x00);

    /* the NMI, taken in a wait at its clock (2 after line 225 begins) */
    nshvc_wait_until(not_hblank, NULL);
    wr8(0x004200, 0x80);
    nmis = 0;
    cpu.unwound = 1;                /* as code that left through its caller's frame and goes on */
    nshvc_wait_until(flag_set, &nmis);
    check(snes_vcounter() == 225 && nmi_at == line_start() + 2, "the NMI is taken in a wait at its clock",
          (long long)(nmi_at - line_start()), 2);
    check(cpu.unwound == 1, "an interrupt in code that left through its caller's frame: no fatal, the flag kept",
          cpu.unwound, 1);
    cpu.unwound = 0;
    wr8(0x004200, 0x00);

    /* the H IRQ, at 4 x HTIME + 14 into its line (bus.c IRQ_DELAY) */
    cpu.p = (uint8_t)(cpu.p & ~P_I);
    wr8(0x004207, 100);
    wr8(0x004208, 0);
    nshvc_wait_vblank();
    irqs = 0;
    wr8(0x004200, 0x10);
    nshvc_wait_until(flag_set, &irqs);
    check(irq_at == line_start() + 4 * 100 + 14, "the H IRQ is taken in a wait at its clock",
          (long long)(irq_at - line_start()), 4 * 100 + 14);
    wr8(0x004200, 0x00);

    /* a wait in the NMI's handler: the horizontal blank seen at its clock,
       the IRQ due meanwhile (line 225, HTIME 200) held by the I flag the
       NMI set, and taken once the handler returns */
    nshvc_wait_until(not_vblank, NULL);
    rd8(0x004210);
    wr8(0x004207, 200);
    wr8(0x004209, 225);
    wr8(0x00420A, 0);
    nmis = irqs = 0;
    nmi_wait = 1;
    wr8(0x004200, 0xB0);
    nshvc_wait_until(flag_set, &irqs);
    nmi_wait = 0;
    wr8(0x004200, 0x00);
    check(nmis == 1 && nmi_waited_to == nmi_at - 2 + 1092, "a wait in the NMI's handler ends at the horizontal blank",
          (long long)(nmi_waited_to - (nmi_at - 2)), 1092);
    check(nmi_irq_seen == 0, "the IRQ is held in the handler's wait", (long long)nmi_irq_seen, 0);
    check(irq_at == nmi_waited_to, "and taken as the handler returns", (long long)(irq_at - nmi_waited_to), 0);

    /* WAI: on at the interrupt, taken there */
    wr8(0x004207, 50);
    nshvc_wait_vblank();
    irqs = 0;
    wr8(0x004200, 0x10);
    cpu_wai(0x008000);
    check(irqs == 1 && irq_at == line_start() + 4 * 50 + 14 && snes_clock == irq_at,
          "WAI ends at the IRQ, taken at its clock", (long long)(irq_at - line_start()), 4 * 50 + 14);
    wr8(0x004200, 0x00);
    /* masked: WAI ends, nothing taken */
    cpu.p = (uint8_t)(cpu.p | P_I);
    nshvc_wait_vblank();
    irqs = 0;
    wr8(0x004200, 0x10);
    cpu_wai(0x008000);
    check(irqs == 0 && snes_line_clock() == 4 * 50 + 14, "WAI masked: on at the IRQ, none taken",
          snes_line_clock(), 4 * 50 + 14);
    wr8(0x004200, 0x00);
    rd8(0x004211);

    /* a DMA takes the DMA unit's time: 8 to start, 8 for the channel, 8 a
       byte, the unit aligned to its clock's steps of 8 and given back on
       an 8-clock cycle, and the refresh (40 a line) it passes */
    nshvc_wait_vblank();
    wr8(0x002115, 0x80);
    wr8(0x002116, 0x00);
    wr8(0x002117, 0x00);
    wr8(0x004300, 0x01);
    wr8(0x004301, 0x18);
    wr8(0x004302, 0x00);
    wr8(0x004303, 0x20);
    wr8(0x004304, 0x7E);
    wr8(0x004305, 0xD0);
    wr8(0x004306, 0x07);
    t0 = snes_clock;
    ls = line_start();
    wr8(0x00420B, 0x01);
    t1 = snes_clock;
    {
        /* the refreshes passed: one each line whose refresh falls in it */
        uint64_t moved = 8 + 8 + 2000 * 8, refresh = 0, l;
        for (l = ls; l < t1; l += 1364)
            if (l + 534 - ((l - 4 + 4) & 7) >= t0 && l + 534 - ((l - 4 + 4) & 7) < t1)
                refresh += 40;
        /* aligned (1-8 clocks to the unit's next step of 8) and given back
           on the cycle (the rest of 8, or 8 when none is left): 8 or 16 */
        uint64_t extra = t1 - t0 - refresh - moved;
        check(extra == 8 || extra == 16, "a DMA of 2,000 bytes: 16,016 clocks, 8 or 16 aligning, the refreshes",
              (long long)extra, 8);
    }

    /* the multiplier and divider: done when read */
    wr8(0x004202, 0xC3);
    wr8(0x004203, 0x7B);
    check(rd16(0x004216) == 0xC3 * 0x7B, "RDMPY: the product", rd16(0x004216), 0xC3 * 0x7B);
    wr16(0x004204, 0xBEEF);
    wr8(0x004206, 0x2D);
    check(rd16(0x004214) == 0xBEEF / 0x2D && rd16(0x004216) == 0xBEEF % 0x2D, "RDDIV, RDMPY: quotient, remainder",
          rd16(0x004214) << 16 | rd16(0x004216), (0xBEEF / 0x2D) << 16 | (0xBEEF % 0x2D));
    wr8(0x004206, 0x00);
    check(rd16(0x004214) == 0xFFFF && rd16(0x004216) == 0xBEEF, "a divide by 0: $FFFF, the dividend",
          rd16(0x004214) << 16 | rd16(0x004216), 0xFFFF << 16 | 0xBEEF);

    printf("%d check(s) failed\n", fails);
    exit(fails ? 1 : 0);
}

static const uint8_t rom[0x8000] = { [0x7FFC] = 0x00, [0x7FFD] = 0x80, [0x7FEA] = 0x00, [0x7FEB] = 0x90,
                                      [0x7FEE] = 0x00, [0x7FEF] = 0xA0, [0x7FD5] = 0x20 };
static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x009000, abi_nmi, "nmi", 0 },
    { 0x00A000, abi_irq, "irq", 0 },
};

int main(int argc, char **argv)
{
    nshvc_cart cart;
    if (argc > 1)
        mode = argv[1];
    memset(&cart, 0, sizeof cart);
    cart.title = "waits";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.routines = routines;
    cart.routine_count = 3;
    nshvc_power(&cart);
    for (;;)
        nshvc_run_frame();
}
