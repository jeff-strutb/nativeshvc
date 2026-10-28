/*
 * chip_irq.c: a chip's IRQ seen at the chip's own clock (cart.h
 * nshvc_cart_irq_at). The chip runs its C on a coroutine of its own, a
 * master clock a step, and stops at a clock the S-CPU sets, raising its
 * IRQ there, as a Super FX does at its STOP. The S-CPU, waiting for it
 * (machine.h nshvc_wait_until on its status, cpu_wai), takes it:
 *
 *   unmasked: at that clock, its handler run before the wait looks again
 *   masked: not at all, the wait seeing the chip stopped at that clock;
 *       taken once the I flag is clear
 *   in WAI: at that clock
 *
 * and, risen while work RAM's refresh stops the S-CPU (40 clocks a line),
 * at the refresh's end. No cost profile (contract 2.1), so each access is
 * its bus's time alone: a chip register's 6 clocks, its value 2 in;
 * the interrupt's four pushes and RTI's four pulls in work RAM 32 each.
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

#define REG 6           /* a read of the chip's register */
#define STACK 32        /* the interrupt's pushes, or RTI's pulls */

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
static int taken, acked;
static uint64_t taken_at;

/* the clock the S-CPU's refresh in the line of clock t starts at (src/
   console/bus.c: REFRESH_AT 534, less the DMA unit's clock, DMA_PHASE 4,
   as the line begins; lines of 1364 clocks, line 0 at clock 0 of a frame
   whose lines are all 1364 long here, NTSC not interlaced, the short
   line 240 of field 1 aside) */
static uint64_t refresh_of(uint64_t t, uint64_t line_now)
{
    uint64_t ls = line_now;
    while (ls > t)
        ls -= 1364;
    while (ls + 1364 <= t)
        ls += 1364;
    return ls + 534 - ((ls - 4 + 4) & 7);
}

/* a rise at t: seen there, or at the end of the refresh it falls in */
static uint64_t seen_at(uint64_t t)
{
    uint64_t r = refresh_of(t, snes_clock - snes_line_clock());
    return t >= r && t < r + 40 ? r + 40 : t;
}

static void abi_irq(void)
{
    taken++;
    taken_at = snes_clock;
    rd8(0x003031);
    acked = 1;
}

static void start(uint16_t d)
{
    wr8(0x003000, (uint8_t)d);
    wr8(0x003001, (uint8_t)(d >> 8));
}

/* the wait's reads of the status: where each began */
static uint64_t read_at[4096];
static int reads;

static int stopped(void *ctx)
{
    (void)ctx;
    if (reads < 4096)
        read_at[reads++] = snes_clock;
    return !(rd8(0x003030) & 0x20);
}

/* the clock after `n` clocks of accesses from t, a refresh in them
   included (one that falls at its end too: the console makes it as the
   clock reaches it) */
static uint64_t pass(uint64_t t, unsigned n)
{
    uint64_t r = refresh_of(t + n, snes_clock - snes_line_clock());
    return r > t && r <= t + n ? t + n + 40 : t + n;
}

/* the S-CPU's accesses of `n` clocks each, `k` of them, from t */
static uint64_t accesses(uint64_t t, unsigned n, int k)
{
    while (k--)
        t = pass(t, n);
    return t;
}

/* where the S-CPU takes the rise at t: at it (or the refresh's end), or at
   the end of the wait's read it rises in; *seen 1 when that read took its
   value (2 clocks in) at or after t, so it saw the chip stopped and the
   wait ends with it */
static uint64_t taken_point(uint64_t t, int *seen)
{
    int i;
    *seen = 0;
    for (i = 0; i < reads; i++)
        if (read_at[i] < t && t < pass(read_at[i], REG)) {
            *seen = t <= read_at[i] + 2;
            return pass(read_at[i], REG);
        }
    return seen_at(t);
}

static void abi_reset(void)
{
    unsigned i;
    int n;

    cpu.e = 0;
    cpu.p = (uint8_t)(cpu.p & ~P_I);

    /* waiting on the status, the IRQ unmasked: taken at the chip's clock,
       the handler's acknowledge before the wait sees the chip stopped */
    for (i = 0; i < 300; i++) {
        uint16_t d = (uint16_t)(37 + i * 47);
        uint64_t at;
        n = taken;
        acked = 0;
        reads = 0;
        start(d);
        nshvc_wait_until(stopped, NULL);
        /* taken at the rise (the handler after the pushes); then its
           acknowledge, RTI's pulls, and the wait's read that sees it */
        {
            int seen;
            uint64_t back;
            at = accesses(taken_point(stop_at, &seen), 8, 4);
            back = accesses(pass(taken_at, REG), 8, 4);
            if (!seen)
                back = pass(back, REG);
            check(taken == n + 1 && taken_at == at && snes_clock == back && acked,
                  "waiting, unmasked: the wait ends after the handler", d, (long long)(snes_clock - started),
                  (long long)(back - started));
        }
        check(taken == n + 1 && taken_at == at && acked,
              "waiting, unmasked: taken at the chip's clock, then the wait ends", d, (long long)(taken_at - started),
              (long long)(at - started));
    }

    /* masked: the wait sees the chip stopped at its clock; none taken */
    cpu.p = (uint8_t)(cpu.p | P_I);
    for (i = 0; i < 300; i++) {
        uint16_t d = (uint16_t)(41 + i * 53);
        n = taken;
        reads = 0;
        start(d);
        nshvc_wait_until(stopped, NULL);
        /* the wait ends with the first of its reads that takes its value
           (2 clocks in) at or after the chip stopped */
        {
            int last = reads - 1;
            int ok = last >= 0 && read_at[last] + 2 >= stop_at && (last == 0 || read_at[last - 1] + 2 < stop_at) &&
                     snes_clock == pass(read_at[last], REG);
            check(taken == n && ok, "waiting, masked: the wait ends with the first read after the chip's clock", d,
                  (long long)(last >= 0 ? read_at[last] + 2 - started : 0), (long long)(stop_at - started));
        }
        rd8(0x003031);
    }
    /* taken once the I flag is clear, at the next point between two of
       the C's operations */
    start(200);
    nshvc_wait_until(stopped, NULL);
    n = taken;
    cpu.p = (uint8_t)(cpu.p & ~P_I);
    {
        uint64_t before = snes_clock;
        rd8(0x7E0000);
        check(taken == n + 1 && taken_at == accesses(pass(before, 8), 8, 4), "unmasked: taken at the next read's end",
              200, (long long)(taken_at - before), (long long)(accesses(pass(before, 8), 8, 4) - before));
    }

    /* WAI: on at the chip's clock, taken there */
    for (i = 0; i < 100; i++) {
        uint16_t d = (uint16_t)(30 + 97 * i);
        n = taken;
        start(d);
        cpu_wai(0x008200);
        check(taken == n + 1 && taken_at == accesses(seen_at(stop_at), 8, 4), "WAI: taken at the chip's clock", d,
              (long long)(taken_at - started), (long long)(accesses(seen_at(stop_at), 8, 4) - started));
    }

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
