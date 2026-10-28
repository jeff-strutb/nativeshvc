/*
 * chip_irq.c: a chip's IRQ seen at the chip's own clock (cart.h
 * nshvc_cart_irq_at). The chip runs its C on a coroutine of its own, a
 * master clock a step, and stops at a clock the S-CPU sets, raising its
 * IRQ there, as a Super FX does at its STOP. The S-CPU takes it:
 *
 *   in a stretch of its time (cyc, cpu_idle): at that clock itself
 *   polling the chip's status: at the clock, inside the stretch between
 *       the reads, or at the end of the read it rises in
 *   in WAI: at the end of the cycle after the one that begins with it up
 *   masked: not at all, until the I flag is clear
 *
 * and, risen while work RAM's refresh stops the S-CPU (40 clocks a line),
 * at the refresh's end.
 *
 * The clock it is taken at is the console's as its interrupt sequence
 * begins (snes.h snes_interrupt_hook, wrapped here).
 *
 *   chip_irq          runs the checks; exit 0 when every one holds
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/cart.h"
#include "nshvc/machine.h"
#include "snes.h"

static int fails;

static void check(int ok, const char *what, long long d, long long got, long long want)
{
    printf("%s %s (D %lld: %lld, want %lld)\n", ok ? "ok  " : "FAIL", what, d, got, want);
    fails += !ok;
}

/* ---- the chip: $3000/$3001 the clocks to its stop (the write to $3001
   starts it), $3030 bit 5 while it runs, a read of $3031 takes its IRQ
   back ---- */
static nshvc_coro *chip_co, *cpu_co;
static uint64_t clk, stop_at;
static int running;
static uint8_t lo;

static void chip_main(void *arg)
{
    (void)arg;
    for (;;) {
        while (running && clk < nshvc_cart_clock()) {
            clk++;
            if (clk == stop_at) {
                running = 0;
                nshvc_cart_irq_at(1, clk);
            }
        }
        nshvc_coro_switch(chip_co, cpu_co);
    }
}

static void chip_sync(void)
{
    if (!running)
        return;
    cpu_co = nshvc_coro_current();
    nshvc_coro_switch(cpu_co, chip_co);
}

static int is_reg(uint32_t addr, uint16_t o)
{
    return (uint8_t)(addr >> 16) < 0x40 && (uint16_t)addr == o;
}

static int chip_read(uint32_t addr, uint8_t *v)
{
    if (is_reg(addr, 0x3030)) {
        chip_sync();
        *v = running ? 0x20 : 0x00;
        return 1;
    }
    if (is_reg(addr, 0x3031)) {
        chip_sync();
        nshvc_cart_irq(0);
        *v = 0;
        return 1;
    }
    return 0;
}

static uint64_t started;            /* the clock the chip was started at */

static int chip_write(uint32_t addr, uint8_t v)
{
    if (is_reg(addr, 0x3000)) {
        lo = v;
        return 1;
    }
    if (is_reg(addr, 0x3001)) {
        chip_sync();
        started = clk = nshvc_cart_clock();
        stop_at = clk + (uint16_t)(lo | v << 8);
        running = 1;
        return 1;
    }
    return 0;
}

static int chip_peek(uint32_t addr, uint8_t *v)
{
    if (is_reg(addr, 0x3030)) {
        *v = running ? 0x20 : 0x00;
        return 1;
    }
    return 0;
}

static int chip_busy(void) { return running; }

static const nshvc_chip toy = {
    .name = "clocked",
    .read = chip_read,
    .write = chip_write,
    .peek = chip_peek,
    .sync = chip_sync,
    .busy = chip_busy,
};

/* ---- the code ---- */
static int taken;
static uint64_t taken_at, taken_line;     /* the clock, and its line's start */
static void (*take)(int nmi);

static void took_it(int nmi)
{
    taken_at = snes_clock;
    taken_line = snes_clock - snes_line_clock();
    take(nmi);
}

/* ---- the time the S-CPU's clock passes, refresh and all, as the
   console's is (src/console/bus.c: REFRESH_AT 534, less the DMA unit's
   clock, DMA_PHASE 4, as the line begins; lines of 1364 clocks) ---- */
static uint64_t refresh_of(uint64_t t)
{
    uint64_t ls = taken_line;
    while (ls > t)
        ls -= 1364;
    while (ls + 1364 <= t)
        ls += 1364;
    return ls + 534 - ((ls - 4 + 4) & 7);
}

/* an S-CPU cycle of c clocks from t: the refresh, when it falls in it,
   after it */
static uint64_t cycle_end(uint64_t t, unsigned c)
{
    uint64_t r = refresh_of(t), u = t + c;
    if (t < r && r <= u)
        u += 40;
    return u;
}

/* a rise at t inside a stretch: taken there, or at the end of the refresh
   it falls in */
static uint64_t in_stretch(uint64_t t)
{
    uint64_t r = refresh_of(t);
    return t >= r && t < r + 40 ? r + 40 : t;
}

static void abi_irq(void)
{
    taken++;
    rd8(0x003031);
    cyc(6);
}

static void start(uint16_t d)
{
    wr8(0x003000, (uint8_t)d);
    wr8(0x003001, (uint8_t)(d >> 8));
}

/* the clock the S-CPU took the IRQ at */
static long long took(void) { return (long long)taken_at; }

static void abi_reset(void)
{
    static const uint16_t ds[] = { 40, 41, 47, 100, 123, 333, 640, 999, 1200, 1777, 2400 };
    unsigned i;
    int n;

    take = snes_interrupt_hook;
    snes_interrupt_hook = took_it;
    cpu.e = 0;
    cpu.p = (uint8_t)(cpu.p & ~P_I);

    /* in a stretch: at the chip's clock */
    for (i = 0; i < sizeof ds / sizeof ds[0]; i++) {
        n = taken;
        start(ds[i]);
        cyc(3000);
        check(taken == n + 1 && took() == (long long)in_stretch(stop_at), "in cyc(3000): taken at the chip's clock",
              ds[i], took(), (long long)in_stretch(stop_at));
    }
    for (i = 0; i < sizeof ds / sizeof ds[0]; i++) {
        n = taken;
        start(ds[i]);
        cpu_idle(3000);
        check(taken == n + 1 && took() == (long long)in_stretch(stop_at),
              "in cpu_idle(3000): taken at the chip's clock", ds[i], took(), (long long)in_stretch(stop_at));
    }

    /* polling the status: at the chip's clock in the stretch between two
       reads, or at the end of the read it rises in (a read of $3030 is 6
       clocks; the hardware's line, seen at its clock) */
    for (i = 0; i < 40; i++) {
        uint16_t d = (uint16_t)(60 + i);
        uint64_t read_at[64], end = 0;
        int k = 0, in_read = -1, j;
        uint8_t v;
        n = taken;
        start(d);
        do {
            read_at[k] = nshvc_cart_clock();
            v = rd8(0x003030);
            if (taken == n)
                cyc(14);
            k++;
        } while ((v & 0x20) && taken == n && k < 64);
        if (taken == n)
            cyc(14);
        for (j = 0; j < k; j++)
            if (stop_at > read_at[j] && stop_at <= cycle_end(read_at[j], 6)) {
                in_read = j;
                end = cycle_end(read_at[j], 6);
            }
        if (in_read >= 0)
            check(taken == n + 1 && took() == (long long)end, "polling, rising in a read: taken at its end", d, took(),
                  (long long)end);
        else
            check(taken == n + 1 && took() == (long long)in_stretch(stop_at),
                  "polling, rising between reads: taken at its clock", d, took(), (long long)in_stretch(stop_at));
    }

    /* WAI: each internal cycle begins with the line looked at; the one that
       finds it up is the last but one */
    for (i = 0; i < 12; i++) {
        uint16_t d = (uint16_t)(30 + 5 * i);
        uint64_t a, t;
        n = taken;
        start(d);
        a = nshvc_cart_clock();
        cpu_wai(0x008200);
        /* the opcode's fetch (8), then cycles of 6 until one begins with the
           line up, then two more */
        t = cycle_end(a, 8);
        while (t < stop_at)
            t = cycle_end(t, 6);
        t = cycle_end(cycle_end(t, 6), 6);
        check(taken == n + 1 && took() == (long long)t, "WAI: taken two cycles after the one it is seen in", d, took(),
              (long long)t);
    }

    /* masked: not taken; taken once the I flag is clear */
    cpu.p = (uint8_t)(cpu.p | P_I);
    n = taken;
    start(200);
    cyc(3000);
    check(taken == n, "masked: not taken", 200, taken - n, 0);
    cpu.p = (uint8_t)(cpu.p & ~P_I);
    cyc(6);
    check(taken == n + 1, "unmasked: taken", 200, taken - n, 1);

    printf("%d check(s) failed\n", fails);
    exit(fails ? 1 : 0);
}

static uint8_t rom[0x8000];

static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x009000, abi_irq, "irq", 0 },
};

int main(void)
{
    nshvc_cart cart;
    memset(&cart, 0, sizeof cart);
    rom[0x7FD5] = 0x20;
    rom[0x7FFC] = 0x00;
    rom[0x7FFD] = 0x80;
    rom[0x7FEE] = 0x00;
    rom[0x7FEF] = 0x90;
    cart.title = "chip_irq";
    cart.map = NSHVC_LOROM;
    cart.rom = rom;
    cart.rom_size = sizeof rom;
    cart.chip = &toy;
    cart.routines = routines;
    cart.routine_count = 2;
    chip_co = nshvc_coro_new(chip_main, NULL, 64 * 1024);
    nshvc_power(&cart);
    for (;;)
        nshvc_run_frame();
}
