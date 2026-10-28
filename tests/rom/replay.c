/*
 * replay.c: the console's hardware held to the reference's, its CPU taken
 * out. The reference runs a test cartridge and records each read and
 * write its S-CPU makes, with the clock it makes it at (tests/oracle
 * --cpulog); this cartridge makes the same accesses on nativeshvc at the
 * same clocks, and every read must answer as the reference's did. What
 * the console does between them (the picture, DMA and HDMA, the counters,
 * the IRQ's flag, the joypads' read, the multiplier) is the console's
 * own, so the reads and the pictures (tests/rom/run.py) check its
 * hardware to the clock, with nothing of the reference's CPU in it but
 * when its accesses come. Its code fetches are made too, as reads: the
 * open bus holds the last byte on it, a fetch's as much as data's.
 *
 * Built on the console's public calls only: the cartridge's own chip
 * raises the IRQ line at the next access's start, masked, and a wait
 * (machine.h nshvc_wait_until) stops at a rise of it. Each access takes
 * its bus's time (contract 2.1; no cost profile here, so nothing more): it
 * starts a cycle before the reference's clock for it, so a read takes its
 * value, and a write lands, at that clock. The S-CPU takes no
 * interrupt here (the I flag held set; the NMI's enable taken out of each
 * $4200 write: RDNMI's flag is the console's still), since the reference's
 * interrupt sequences are in the record as its accesses.
 *
 *   replay --rom ROM --log CPULOG [nshvc_main's options]
 *
 * At exit, on stderr: "replay: N accesses, R reads, B differ, L late",
 * then each read that differs (up to 20) and each access the console made
 * the CPU late for (its clock past the reference's: a DMA, the refresh);
 * exit status 1 when any differs or is late.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/cart.h"
#include "nshvc/machine.h"

/* the reference's CPU clock is the console's sound unit's: 4 clocks
   before the console's (src/console/apu.c apu_clock) */
#define CLOCK_OFFSET 4
/* the reset's own accesses (its stack and vector reads) come before the
   CPU's first instruction, where the console starts its C (src/console
   snes.h SNES_POWER_CLOCK): not made here */
#define POWER_CLOCK 182
/* The DMA unit takes the bus at the CPU's cycles: a DMA starts at the
   CPU's second cycle after its $420B write, an HDMA at the start of the
   cycle after the one it is taken up at, and the bus comes back on the
   CPU's own cycle. The reference's cycles are its instructions'; the
   replay sees only its accesses, and nativeshvc's C makes no cycles (its
   DMA starts at the write, an HDMA in a wait at the wait's cycles). So the
   CPU's accesses in the two cycles after a $420B write (up to 24 clocks)
   come after the DMA here, and one after the unit's work may come up to
   three of the CPU's cycles (36 clocks) late: the CPU's time, not held.
   Anything later is the console's. A DMA's own length is held to the
   reference's by its last byte: nativeshvc's DMA, started a cycle earlier
   here, ends (the bus given back) 0 to 6 clocks before the reference's
   last byte, by how the two starts fall on the unit's steps of 8 (measured
   over the suite: -6, -4, -2, 0), so a byte (8 clocks) more or less shows.
   Not with HDMA on (HDMAEN set as it is written): an HDMA in the DMA ends
   with its table's reads, which write nothing to measure by; its bytes
   and what it does are held as the rest are. Nor when the $420B write
   itself started late (within the CPU's cycles, above: the unit's start
   then falls on another of its steps of 8); those are counted as not
   measured. */
#define DMA_START_CLOCKS 24
#define CYCLES_LATE 36
#define DMA_END_FROM (-6)
#define DMA_END_TO 0

typedef struct {
    uint64_t at;
    uint32_t addr;
    uint8_t kind, value;        /* 0 a read, 1 a write, 2 a fetch, 3 a DMA's write */
} Access;

static Access *log_;
static size_t log_n, done, reads, differ, late, dma_start, aligned, dma_short, dma_unmeasured;

static void report(void)
{
    fprintf(stderr,
            "replay: %zu of %zu accesses, %zu reads, %zu differ, %zu late, %zu DMAs' lengths differ (%zu before a DMA's start, "
            "%zu within the CPU's cycles, %zu DMAs not measured)\n",
            done, log_n, reads, differ, late, dma_short, dma_start, aligned, dma_unmeasured);
}

static void finish(void)
{
    report();
    fflush(stderr);
    if (differ || late || dma_short)
        _Exit(1);
}

/* ---- the chip: no registers, its IRQ line the replay's clock ---- */
static int no_read(uint32_t addr, uint8_t *v) { (void)addr; (void)v; return 0; }
static int no_write(uint32_t addr, uint8_t v) { (void)addr; (void)v; return 0; }
static const nshvc_chip clock_chip = { .name = "replay", .read = no_read, .write = no_write };

static int reached(void *ctx) { return nshvc_cart_clock() >= *(uint64_t *)ctx; }

/* MEMSEL as last written: FastROM's 6-clock cycles in banks $80-$FF */
static uint8_t memsel;

/* an access's cycle, by the console's memory speeds (src/console/bus.c) */
static unsigned cycle(uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    int fast = (memsel & 1) && (bank & 0x80);
    if (bank & 0x40)
        return fast && bank >= 0xC0 ? 6 : 8;
    if (off >= 0x8000)
        return fast ? 6 : 8;
    if (off < 0x2000 || off >= 0x6000)
        return 8;
    if (off >= 0x4000 && off < 0x4200)
        return 12;
    return 6;
}

/* the reference's clock of the last $420B write that started a DMA,
   nativeshvc's when its DMA ended (0: the access after it not yet seen),
   and the reference's last DMA byte since */
static uint64_t dma_written = UINT64_MAX, dma_ended, dma_last_byte;
static uint8_t hdmaen;          /* HDMAEN as last written */

static void abi_reset(void)
{
    cpu.e = 0;
    cpu.p = (uint8_t)(cpu.p | P_I);
    for (done = 0; done < log_n; done++) {
        const Access *a = &log_[done];
        uint64_t at = a->at - CLOCK_OFFSET, start;
        /* its cycle's start: a read's value 4 clocks before its end, a
           write's landing at its end */
        start = at - (a->kind == 1 ? cycle(a->addr) : cycle(a->addr) - 4);
        if (nshvc_cart_clock() < start) {
            nshvc_cart_irq_at(0, 0);
            nshvc_cart_irq_at(1, start);
            nshvc_wait_until(reached, &start);
            nshvc_cart_irq_at(0, 0);
        }
        if (at < POWER_CLOCK)
            continue;               /* the reset's own: before the CPU's first instruction */
        if (a->kind == 3) {
            /* the reference's DMA and HDMA: nativeshvc's own makes them */
            dma_last_byte = at;
            continue;
        }
        if (dma_ended && at > dma_written + DMA_START_CLOCKS) {
            /* the first of the CPU's accesses after the DMA's start: the
               DMA's end against the reference's last byte */
            long long d = (long long)dma_ended - (long long)dma_last_byte;
            if (getenv("NSHVC_REPLAY_DMA_END")) {
                /* NSHVC_REPLAY_DMA_END=FILE: each DMA's end against the
                   reference's last byte, a line each (the window's
                   measure) */
                FILE *m = fopen(getenv("NSHVC_REPLAY_DMA_END"), "a");
                if (m) {
                    fprintf(m, "%lld\n", d);
                    fclose(m);
                }
            }
            if ((d < DMA_END_FROM || d > DMA_END_TO) && dma_short++ < 20)
                fprintf(stderr, "replay: the DMA written at %llu ended at %llu, the reference's last byte at %llu\n",
                        (unsigned long long)dma_written, (unsigned long long)dma_ended,
                        (unsigned long long)dma_last_byte);
            dma_ended = 0;
        }
        if (nshvc_cart_clock() != start && at >= POWER_CLOCK && dma_written != UINT64_MAX &&
            at <= dma_written + DMA_START_CLOCKS) {
            dma_start++;
        } else if (nshvc_cart_clock() > start && nshvc_cart_clock() <= start + CYCLES_LATE && at >= POWER_CLOCK) {
            aligned++;
        } else if (nshvc_cart_clock() != start && at >= POWER_CLOCK) {
            if (late++ < 20)
                fprintf(stderr, "replay: access %zu ($%06X) started at clock %llu, the reference's at %llu\n", done,
                        (unsigned)a->addr, (unsigned long long)nshvc_cart_clock(), (unsigned long long)start);
        }
        cpu.p = (uint8_t)(cpu.p | P_I);
        if (a->kind == 1) {
            uint8_t v = a->value;
            int on_time = nshvc_cart_clock() == start;
            if ((a->addr & 0x40FFFF) == 0x4200)
                v &= 0x7F;
            wr8(a->addr, v);
            if ((a->addr & 0x40FFFF) == 0x420C)
                hdmaen = v;
            if ((a->addr & 0x40FFFF) == 0x420D)
                memsel = v;
            if ((a->addr & 0x40FFFF) == 0x420B && v) {
                dma_written = at;
                dma_ended = hdmaen || !on_time ? 0 : nshvc_cart_clock();
                dma_unmeasured += !hdmaen && !on_time;
            }
        } else {
            uint8_t v = rd8(a->addr);
            reads++;
            if (v != a->value && differ++ < 20)
                fprintf(stderr, "replay: read %zu of $%06X at clock %llu: %02X, the reference's %02X\n", done,
                        (unsigned)a->addr, (unsigned long long)at, v, a->value);
        }
    }
    cpu_loop_forever(0x008000);
}

static void abi_none(void) {}

int main(int argc, char **argv)
{
    nshvc_cart cart;
    const char *path = NULL, *logpath = NULL;
    static uint8_t *rom;
    uint32_t rom_size;
    FILE *f;
    int i;
    uint16_t nmi, irq;
    static nshvc_routine table[3];
    for (i = 1; i + 1 < argc; i++) {
        if (!strcmp(argv[i], "--rom"))
            path = argv[i + 1];
        if (!strcmp(argv[i], "--log"))
            logpath = argv[i + 1];
    }
    if (!path || !logpath) {
        fprintf(stderr, "replay: --rom ROM --log CPULOG\n");
        return 2;
    }
    if (!(f = fopen(path, "rb"))) {
        fprintf(stderr, "replay: cannot read %s\n", path);
        return 2;
    }
    rom = (uint8_t *)malloc(16u << 20);
    rom_size = (uint32_t)fread(rom, 1, 16u << 20, f);
    fclose(f);
    if (!(f = fopen(logpath, "rb"))) {
        fprintf(stderr, "replay: cannot read %s\n", logpath);
        return 2;
    }
    {
        uint8_t rec[16];
        size_t cap = 1 << 16;
        log_ = (Access *)malloc(cap * sizeof *log_);
        while (fread(rec, 1, 16, f) == 16) {
            Access *a;
            int k;
            if (log_n == cap)
                log_ = (Access *)realloc(log_, (cap *= 2) * sizeof *log_);
            a = &log_[log_n++];
            a->at = 0;
            for (k = 7; k >= 0; k--)
                a->at = a->at << 8 | rec[k];
            a->addr = (uint32_t)(rec[8] | rec[9] << 8 | rec[10] << 16 | (uint32_t)rec[11] << 24) & 0xFFFFFF;
            a->kind = rec[12];
            a->value = rec[13];
        }
        fclose(f);
    }
    atexit(finish);
    /* the mapping, from where snestest.py put the header (as testcart.c) */
    {
        uint32_t h = 0x7FC0;
        nshvc_map map = NSHVC_LOROM;
        if (rom_size > 0x407FDF && (rom[0x407FD5] & 0xEF) == 0x20) {
            h = 0x407FC0;
            map = NSHVC_EXLOROM;
        } else if (rom_size > 0x40FFDF && rom[0x40FFD5] == 0x25) {
            h = 0x40FFC0;
            map = NSHVC_EXHIROM;
        } else if (rom_size > 0xFFDF && rom[0xFFD5] == 0x21) {
            h = 0xFFC0;
            map = NSHVC_HIROM;
        }
        memset(&cart, 0, sizeof cart);
        cart.map = map;
        cart.sram_size = rom[h + 0x18] ? 1024u << rom[h + 0x18] : 0;
        cart.region = rom[h + 0x19] >= 0x02 && rom[h + 0x19] <= 0x0C ? NSHVC_PAL : NSHVC_NTSC;
        nmi = (uint16_t)(rom[h + 0x2A] | rom[h + 0x2B] << 8);
        irq = (uint16_t)(rom[h + 0x2E] | rom[h + 0x2F] << 8);
    }
    /* the reset's C, and the handlers the S-CPU never takes here */
    table[0].addr = 0x008000;
    table[0].abi = abi_reset;
    table[0].name = "reset";
    table[1].addr = nmi;
    table[1].abi = abi_none;
    table[1].name = "nmi";
    table[2].addr = irq;
    table[2].abi = abi_none;
    table[2].name = "irq";
    for (i = 0; i < 3; i++) {
        int j;
        for (j = i + 1; j < 3; j++)
            if (table[j].addr < table[i].addr) {
                nshvc_routine t = table[i];
                table[i] = table[j];
                table[j] = t;
            }
    }
    cart.title = "nativeshvc replay";
    cart.save_name = "nshvc_replay";
    cart.rom = rom;
    cart.rom_size = rom_size;
    cart.chip = &clock_chip;
    cart.routines = table;
    cart.routine_count = 3;
    return nshvc_main(&cart, argc, argv);
}
