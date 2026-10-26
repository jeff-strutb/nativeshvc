/*
 * bus.c: the S-CPU's bus and clock.
 *
 * rd8 and wr8 are the decomp's view of the machine (machine.h): one byte of
 * the 24-bit address space each. Here they reach the work RAM, the
 * cartridge (its ROM and RAM where its mapping puts them, nshvc.h
 * nshvc_map; the Super FX's registers at $3000), the PPU and the sound
 * ports on the B bus ($2100-$21FF), the joypads ($4016-$4017) and the CPU's
 * own registers ($4200-$421F, DMA at $4300).
 *
 * Each access moves the master clock on by the time the bus takes for it:
 * 8 clocks for work RAM, the cartridge's RAM and ROM (6 for ROM in banks
 * $80-$FF when MEMSEL selects FastROM), 6 for most registers, 12 for
 * $4000-$41FF (the joypads' serial ports). The opcode and operand fetches
 * and internal cycles between two accesses are the time the C states (cyc:
 * snes_cpu_clocks), the original's own. Passing a line's horizontal blank
 * draws that line and runs HDMA; passing the vertical blank's line (225,
 * or 240 with overscan) starts the vertical blank (the NMI flag, the
 * joypads' automatic read, the frame's hook).
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_internal.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif

uint8_t rd8(uint32_t addr);
void wr8(uint32_t addr, uint8_t value);

nshvc_cart snes_cart;
uint64_t snes_clock;
uint8_t snes_mdr;
int nshvc_rom_unheld;

void (*snes_interrupt_hook)(int nmi);
int (*snes_irq_masked)(void);
void (*snes_frame_hook)(void);
void (*snes_write_hook)(uint32_t addr, uint8_t value);

static uint8_t wram[0x20000];
static uint32_t wram_port;
static uint8_t *sram;
static uint32_t sram_mask;

/* the CPU's registers */
static uint8_t nmitimen, wrio = 0xFF, wrmpya = 0xFF, wrmpyb, wrdivb, memsel;
static uint16_t wrdiv = 0xFFFF, rddiv, rdmpy;
static uint16_t htime = 0x1FF, vtime = 0x1FF;
static uint8_t rdnmi_flag, timeup_flag;
static int nmi_pending, cart_irq;

static void latch_counters(void);
static void *audit_at;   /* where the last audited access came from (NSHVC_AUDIT) */

static int audit_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("NSHVC_AUDIT") != NULL;
    return on;
}

static void audit_site(uint32_t addr, int w)
{
    static void *seen[1024];
    static int n;
    int i;
    for (i = 0; i < n; i++)
        if (seen[i] == audit_at)
            return;
    if (n < 1024)
        seen[n++] = audit_at;
#ifndef _WIN32
    {
        Dl_info di;
        if (dladdr(audit_at, &di) && di.dli_sname) {
            fprintf(stderr, "audit: $%06X %s by %s+%ld\n", (unsigned)addr, w ? "written" : "read", di.dli_sname,
                    (long)((char *)audit_at - (char *)di.dli_saddr));
            return;
        }
    }
#endif
    fprintf(stderr, "audit: $%06X %s at %p\n", (unsigned)addr, w ? "written" : "read", audit_at);
}

/* the joypads */
static uint16_t joy_held[2], joy_auto[2];
static uint16_t joy_shift[2];
static uint8_t joy_strobe;
static uint64_t joy_busy_until;

/* the beam: the line now and where it began */
static unsigned vline, lines = SNES_LINES_NTSC, vblank_line = 225;
static uint64_t line_start;
static uint32_t frames, vblanks;
int snes_untimed_on, snes_in_interrupt;
static int line_done_hblank, line_done_refresh;
static uint64_t irq_at, next_event;
#define NEVER UINT64_MAX
#define HBLANK_AT 1096                    /* dot 274 */
/* the work RAM's refresh: the S-CPU stopped for 40 clocks once a line, at
   dot 134 */
#define REFRESH_AT 536
#define REFRESH_CLOCKS 40

/* where an interrupt may not be taken: the access straight after an access
   of the same kind to the byte before it (the second byte of one word);
   the byte after that starts the next word, so a run of accesses up
   through memory (a copy, a fill) still has its places */
static uint32_t last_read = 0xFFFFFFFFu, last_write = 0xFFFFFFFFu;
static unsigned dbg_irqs, dbg_nmis;          /* for NSHVC_DEBUG */
static int last_was_read, last_was_write;

void snes_fatal(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "nativeshvc: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(70);
}

uint32_t snes_frame(void) { return frames; }
unsigned snes_vcounter(void) { return vline; }
/* A line's 340 dots are 4 master clocks each but dots 323 and 327, which
   are 6 (1,364 clocks a line). */
static unsigned dot_of(uint64_t c)
{
    if (c < 323 * 4)
        return (unsigned)(c / 4);
    if (c < 323 * 4 + 6)
        return 323;
    if (c < 323 * 4 + 6 + 12)
        return (unsigned)(324 + (c - (323 * 4 + 6)) / 4);
    if (c < 323 * 4 + 6 + 12 + 6)
        return 327;
    return (unsigned)(328 + (c - (323 * 4 + 6 + 12 + 6)) / 4);
}

/* the clock in its line dot h starts at */
static uint64_t clock_of(unsigned h)
{
    return (uint64_t)h * 4 + (h > 323 ? 2 : 0) + (h > 327 ? 2 : 0);
}

unsigned snes_hcounter(void) { return dot_of(snes_clock - line_start); }
uint32_t snes_line_clock(void) { return (uint32_t)(snes_clock - line_start); }
unsigned snes_lines(void) { return lines; }
int snes_pal(void) { return snes_cart.region == NSHVC_PAL; }
unsigned snes_vblank_line(void) { return vblank_line; }
const uint8_t *snes_wram(void) { return wram; }
uint8_t *snes_wram_rw(void) { return wram; }
uint8_t *snes_sram(void) { return snes_cart.map == NSHVC_SUPERFX ? gsu_ram_mem : sram; }

/* ---- interrupts and the beam --------------------------------------------------- */
/* This line's IRQ time. At the line's start (written 0) every time on the
   line counts, however far the clock has run on; after a write to the
   registers mid-line, a time already passed gives no IRQ this line. */
static void irq_schedule(int written)
{
    unsigned mode = (nmitimen >> 4) & 3;
    uint64_t at = NEVER;
    if (mode == 1 && htime < 340)
        at = line_start + clock_of(htime) + 14;
    else if (mode == 2 && vline == vtime)
        at = line_start + 10;
    else if (mode == 3 && vline == vtime && htime < 340)
        at = line_start + clock_of(htime) + 14;
    irq_at = at;
    if (written && at != NEVER && at < snes_clock)
        irq_at = NEVER;
}

static void events_next(void)
{
    uint64_t e = !line_done_refresh ? line_start + REFRESH_AT
               : line_done_hblank ? line_start + SNES_LINE_CLOCKS : line_start + HBLANK_AT;
    next_event = irq_at < e ? irq_at : e;
}

static void line_begin(void)
{
    if (vline == vblank_line) {
        vblanks++;
        rdnmi_flag = 1;
        if (nmitimen & 0x80)
            nmi_pending = 1;
        ppu_vblank_start();
        if (getenv("NSHVC_DEBUG")) {
            char a[128];
            apu_debug(a, sizeof a);
            fprintf(stderr, "frame %u: %s; irqs %u nmis %u nmitimen %02X vtime %u htime %u; last read %06X, "
                    "Super FX %s, APU in %02X %02X %02X %02X out %02X %02X %02X %02X\n",
                    frames, a, dbg_irqs, dbg_nmis, nmitimen, vtime, htime, last_read,
                    gsu_running() ? "running" : "idle",
                    apu_debug_port(0), apu_debug_port(1), apu_debug_port(2), apu_debug_port(3),
                    apu_port_read(0), apu_port_read(1), apu_port_read(2), apu_port_read(3));
            dbg_irqs = dbg_nmis = 0;
        }
        /* the frame is done: the front end shows it and sets the buttons
           the joypads' read now takes */
        if (snes_frame_hook)
            snes_frame_hook();
        if (nmitimen & 1) {
            joy_auto[0] = joy_held[0];
            joy_auto[1] = joy_held[1];
            joy_shift[0] = joy_shift[1] = 0xFFFF;
            joy_busy_until = snes_clock + 130 + 4224;
        }
    } else if (vline == 0) {
        rdnmi_flag = 0;
        frames++;
        /* overscan (SETINI bit 2) as the frame starts: 239 lines shown */
        vblank_line = ppu_overscan() ? 240 : 225;
        ppu_frame_start();
        hdma_init();
    }
    irq_schedule(0);
}

static void run_events(void)
{
    for (;;) {
        if (irq_at != NEVER && snes_clock >= irq_at) {
            timeup_flag = 1;
            irq_at = NEVER;
            continue;
        }
        if (!line_done_refresh && snes_clock >= line_start + REFRESH_AT) {
            line_done_refresh = 1;
            snes_clock += REFRESH_CLOCKS;
            continue;
        }
        if (!line_done_hblank && snes_clock >= line_start + HBLANK_AT) {
            line_done_hblank = 1;
            if (vline >= 1 && vline < vblank_line)
                ppu_render_line(vline);
            if (vline < vblank_line)
                hdma_line();
            gsu_sync();
            apu_sync();
            continue;
        }
        if (snes_clock >= line_start + SNES_LINE_CLOCKS) {
            /* an IRQ time past the line's end (H 339 and the IRQ's own
               delay) comes early in the next line */
            uint64_t carried = irq_at;
            line_start += SNES_LINE_CLOCKS;
            vline = vline + 1 == lines ? 0 : vline + 1;
            line_done_hblank = line_done_refresh = 0;
            irq_at = NEVER;
            line_begin();
            if (carried != NEVER && carried < irq_at)
                irq_at = carried;
            continue;
        }
        break;
    }
    events_next();
}

uint32_t snes_vblanks(void) { return vblanks; }

void snes_tick(unsigned clocks)
{
    if (snes_untimed_on && !snes_in_interrupt)
        return;
    snes_clock += clocks;
    if (snes_clock >= next_event)
        run_events();
}

void snes_cart_irq(int level) { cart_irq = level; }

/* nshvc_hold_interrupts: due interrupts wait while held */
static int interrupts_held;
void nshvc_hold_interrupts(int on) { interrupts_held = on != 0; }

static void check_interrupts(void)
{
    if (interrupts_held)
        return;
    if (nmi_pending) {
        nmi_pending = 0;
        dbg_nmis++;
        if (snes_interrupt_hook)
            snes_interrupt_hook(1);
        return;
    }
    if ((timeup_flag || cart_irq) && snes_interrupt_hook && !(snes_irq_masked && snes_irq_masked())) {
        dbg_irqs++;
        snes_interrupt_hook(0);
    }
}

/* ---- the CPU's registers ---------------------------------------------------------- */
static uint8_t cpu_reg_read(uint16_t off)
{
    uint8_t v;
    switch (off) {
    case 0x4210:
        /* bit 7 the NMI flag (cleared by the read), bits 0-3 the CPU's
           version (2) */
        v = (uint8_t)(rdnmi_flag << 7 | (snes_mdr & 0x70) | 0x02);
        rdnmi_flag = 0;
        return v;
    case 0x4211:
        v = (uint8_t)(timeup_flag << 7 | (snes_mdr & 0x7F));
        timeup_flag = 0;
        return v;
    case 0x4212: {
        uint64_t pos = snes_clock - line_start;
        v = (uint8_t)(snes_mdr & 0x3E);
        if (vline >= vblank_line)
            v |= 0x80;
        if (pos >= HBLANK_AT || pos < 4)
            v |= 0x40;
        if (snes_clock < joy_busy_until)
            v |= 0x01;
        return v;
    }
    case 0x4213: return wrio;
    case 0x4214: return (uint8_t)rddiv;
    case 0x4215: return (uint8_t)(rddiv >> 8);
    case 0x4216: return (uint8_t)rdmpy;
    case 0x4217: return (uint8_t)(rdmpy >> 8);
    case 0x4218: return (uint8_t)joy_auto[0];
    case 0x4219: return (uint8_t)(joy_auto[0] >> 8);
    case 0x421A: return (uint8_t)joy_auto[1];
    case 0x421B: return (uint8_t)(joy_auto[1] >> 8);
    case 0x421C: case 0x421D: case 0x421E: case 0x421F: return 0;
    }
    if (off >= 0x4300 && off < 0x4380)
        return dma_read(off);
    return snes_mdr;
}

static void cpu_reg_write(uint16_t off, uint8_t v)
{
    switch (off) {
    case 0x4200: {
        uint8_t was = nmitimen;
        nmitimen = v;
        /* the NMI enabled during the vertical blank, its flag still set */
        if (!(was & 0x80) && (v & 0x80) && rdnmi_flag)
            nmi_pending = 1;
        if (!(v & 0x30))
            timeup_flag = 0;
        irq_schedule(1);
        events_next();
        return;
    }
    case 0x4201:
        if ((wrio & 0x80) && !(v & 0x80))
            latch_counters();
        wrio = v;
        return;
    case 0x4202: wrmpya = v; return;
    case 0x4203:
        wrmpyb = v;
        rdmpy = (uint16_t)(wrmpya * wrmpyb);
        rddiv = wrmpyb;
        return;
    case 0x4204: wrdiv = (uint16_t)((wrdiv & 0xFF00) | v); return;
    case 0x4205: wrdiv = (uint16_t)((wrdiv & 0x00FF) | v << 8); return;
    case 0x4206:
        wrdivb = v;
        if (v) {
            rddiv = (uint16_t)(wrdiv / v);
            rdmpy = (uint16_t)(wrdiv % v);
        } else {
            rddiv = 0xFFFF;
            rdmpy = wrdiv;
        }
        return;
    case 0x4207: htime = (uint16_t)((htime & 0x100) | v); irq_schedule(1); events_next(); return;
    case 0x4208: htime = (uint16_t)((htime & 0xFF) | (v & 1) << 8); irq_schedule(1); events_next(); return;
    case 0x4209: vtime = (uint16_t)((vtime & 0x100) | v); irq_schedule(1); events_next(); return;
    case 0x420A: vtime = (uint16_t)((vtime & 0xFF) | (v & 1) << 8); irq_schedule(1); events_next(); return;
    case 0x420B: dma_start(v); return;
    case 0x420C: dma_hdmaen = v; return;
    case 0x420D: memsel = v; return;
    }
    if (off >= 0x4300 && off < 0x4380)
        dma_write(off, v);
}

/* the joypads' serial ports: a strobe latches the buttons, then each read
   shifts out one (B first); past the 16th, a standard pad reads 1 */
static uint8_t joy_serial_read(int port)
{
    uint8_t bit;
    if (joy_strobe & 1)
        return (uint8_t)(joy_held[port] >> 15 & 1);
    bit = (uint8_t)(joy_shift[port] >> 15 & 1);
    joy_shift[port] = (uint16_t)(joy_shift[port] << 1 | 1);
    return bit;
}

static void joy_strobe_write(uint8_t v)
{
    if ((joy_strobe & 1) && !(v & 1)) {
        joy_shift[0] = joy_held[0];
        joy_shift[1] = joy_held[1];
    }
    joy_strobe = v;
}

void snes_set_joypad(int port, uint16_t buttons)
{
    if (port == 0 || port == 1)
        joy_held[port] = (uint16_t)(buttons & 0xFFF0);
}

/* ---- the counters' latch (the PPU's $2137, $213C-$213D, $213F) ---- */
static void latch_counters(void)
{
    ppu_latch_counters((uint16_t)snes_hcounter(), (uint16_t)snes_vcounter());
}

/* ---- the B bus --------------------------------------------------------------------- */
uint8_t bus_read_b(uint8_t reg)
{
    if (reg < 0x34)
        return ppu_open1(reg) ? ppu_read(reg) : snes_mdr;
    if (reg >= 0x34 && reg < 0x40) {
        if (reg == 0x37) {
            /* a read of SLHV latches the counters (when WRIO's bit 7 lets
               it) and answers with the open bus */
            if (wrio & 0x80)
                latch_counters();
            return snes_mdr;
        }
        return ppu_read(reg);
    }
    if (reg >= 0x40 && reg < 0x80) {
        apu_sync();
        return apu_port_read(reg & 3);
    }
    if (reg == 0x80) {
        uint8_t v = wram[wram_port];
        wram_port = (wram_port + 1) & 0x1FFFF;
        return v;
    }
    return snes_mdr;
}

void bus_write_b(uint8_t reg, uint8_t v)
{
    if (reg < 0x34) {
        ppu_write(reg, v);
        return;
    }
    if (reg >= 0x40 && reg < 0x80) {
        apu_sync();
        apu_port_write(reg & 3, v);
        return;
    }
    switch (reg) {
    case 0x80:
        wram[wram_port] = v;
        wram_port = (wram_port + 1) & 0x1FFFF;
        return;
    case 0x81: wram_port = (wram_port & 0x1FF00) | v; return;
    case 0x82: wram_port = (wram_port & 0x100FF) | (uint32_t)v << 8; return;
    case 0x83: wram_port = (wram_port & 0x0FFFF) | (uint32_t)(v & 1) << 16; return;
    }
}

/* ---- the cartridge ---------------------------------------------------------------- */
/* A ROM whose size is not a power of two is chips of power-of-two sizes,
   the largest first; an offset past the end falls in the last chip's
   mirror: the offset, its top bits taken off one at a time, until it
   lands inside. */
uint32_t snes_rom_mirror(uint32_t off)
{
    uint32_t size = snes_cart.rom_size, base = 0, mask = 1u << 24;
    if (!size)
        return 0;
    if (off < size)
        return off;
    off &= (mask << 1) - 1;
    while (off >= size) {
        while (mask && !(off & mask))
            mask >>= 1;
        off -= mask;
        if (size > mask) {
            size -= mask;
            base += mask;
        }
        mask >>= 1;
    }
    return base + off;
}

/* What a cartridge address is: ROM at an offset, the cartridge's RAM at
   an offset, or neither (open bus). */
enum { CART_NONE, CART_ROM, CART_RAM };
static int cart_decode(uint8_t bank, uint16_t off, uint32_t *at)
{
    uint8_t b = bank & 0x7F;
    switch (snes_cart.map) {
    case NSHVC_LOROM:
    case NSHVC_EXLOROM: {
        uint32_t hi = 0;
        if (snes_cart.map == NSHVC_EXLOROM && !(bank & 0x80))
            hi = 0x400000;
        /* the cartridge's RAM in banks $70-$7D and $F0-$FF: their lower
           halves, or the whole banks when the ROM is 2 MB or less (no ROM
           above it there) */
        if (b >= 0x70 && (b < 0x7E || bank >= 0xFE) && sram
            && (off < 0x8000 || snes_cart.rom_size <= 0x200000)) {
            *at = ((uint32_t)(b - 0x70) << 15 | (off & 0x7FFF)) & sram_mask;
            return CART_RAM;
        }
        if (off >= 0x8000) {
            *at = snes_rom_mirror(hi | (uint32_t)b << 15 | (off & 0x7FFF));
            return CART_ROM;
        }
        return CART_NONE;
    }
    case NSHVC_HIROM:
    case NSHVC_EXHIROM: {
        uint32_t hi = 0;
        if (snes_cart.map == NSHVC_EXHIROM && !(bank & 0x80))
            hi = 0x400000;
        if (b < 0x40) {
            if (off >= 0x6000 && off < 0x8000 && b >= 0x20 && sram) {
                *at = ((uint32_t)(b & 0x1F) << 13 | (off - 0x6000)) & sram_mask;
                return CART_RAM;
            }
            if (off >= 0x8000) {
                *at = snes_rom_mirror(hi | (uint32_t)(b & 0x3F) << 16 | off);
                return CART_ROM;
            }
            return CART_NONE;
        }
        *at = snes_rom_mirror(hi | (uint32_t)(b & 0x3F) << 16 | off);
        return CART_ROM;
    }
    case NSHVC_SUPERFX:
        if (b < 0x40) {
            if (off >= 0x8000) {
                *at = snes_rom_mirror((uint32_t)(b & 0x3F) << 15 | (off & 0x7FFF));
                return CART_ROM;
            }
            if (off >= 0x6000) {
                *at = off - 0x6000;
                return CART_RAM;
            }
            return CART_NONE;
        }
        if (b < 0x60) {
            *at = snes_rom_mirror((uint32_t)(b & 0x1F) << 16 | off);
            return CART_ROM;
        }
        if (b == 0x70 || b == 0x71 || (b < 0x7E && b >= 0x70 && (uint32_t)(b - 0x70) << 16 < gsu_ram_size)) {
            *at = (uint32_t)(b - 0x70) << 16 | off;
            return CART_RAM;
        }
        return CART_NONE;
    }
    return CART_NONE;
}

static inline uint8_t *cart_ram(uint32_t at)
{
    if (snes_cart.map == NSHVC_SUPERFX) {
        gsu_sync();
        return &gsu_ram_mem[gsu_ram_size <= 0x20000 ? (at & 0x1FFFF) & (gsu_ram_size - 1) : at % gsu_ram_size];
    }
    return &sram[at & sram_mask];
}

/* kind: 0 the CPU, 1 DMA's A bus (which reaches no registers) */
static uint8_t bus_read(uint32_t addr, int kind)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    uint32_t at;
    int what;
    if (bank == 0x7E || bank == 0x7F)
        return wram[(uint32_t)(bank & 1) << 16 | off];
    if (!(bank & 0x40) && off < 0x6000) {
        if (off < 0x2000)
            return wram[off];
        if (kind == 1 && bus_a_is_register(addr))
            return snes_mdr;
        if (off >= 0x2100 && off < 0x2200)
            return bus_read_b((uint8_t)off);
        if (off >= 0x3000 && off < 0x3300 && snes_cart.map == NSHVC_SUPERFX) {
            gsu_sync();
            return gsu_reg_read(off);
        }
        if (off == 0x4016)
            return (uint8_t)((snes_mdr & 0xFC) | joy_serial_read(0));
        if (off == 0x4017)
            return (uint8_t)((snes_mdr & 0xE0) | 0x1C | joy_serial_read(1));
        if (off >= 0x4200 && off < 0x4400)
            return cpu_reg_read(off);
        return snes_mdr;
    }
    what = cart_decode(bank, off, &at);
    if (what == CART_ROM)
        return snes_rom_byte(at, kind ? NSHVC_ROM_DMA : NSHVC_ROM_CPU);
    if (what == CART_RAM)
        return *cart_ram(at);
    return snes_mdr;
}

static void bus_write(uint32_t addr, uint8_t v, int kind)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    uint32_t at;
    if (bank == 0x7E || bank == 0x7F) {
        wram[(uint32_t)(bank & 1) << 16 | off] = v;
        return;
    }
    if (!(bank & 0x40) && off < 0x6000) {
        if (off < 0x2000) {
            wram[off] = v;
            return;
        }
        if (kind == 1 && bus_a_is_register(addr))
            return;
        if (off >= 0x2100 && off < 0x2200) {
            bus_write_b((uint8_t)off, v);
            return;
        }
        if (off >= 0x3000 && off < 0x3300 && snes_cart.map == NSHVC_SUPERFX) {
            gsu_sync();
            gsu_reg_write(off, v);
            return;
        }
        if (off == 0x4016) {
            joy_strobe_write(v);
            return;
        }
        if (off >= 0x4200 && off < 0x4400)
            cpu_reg_write(off, v);
        return;
    }
    if (cart_decode(bank, off, &at) == CART_RAM)
        *cart_ram(at) = v;
}

int bus_a_is_register(uint32_t addr)
{
    uint16_t off = (uint16_t)addr;
    if (addr & 0x400000)
        return 0;
    return (off >= 0x2100 && off < 0x2200) || (off >= 0x4000 && off < 0x4400);
}

uint8_t bus_read_a(uint32_t addr)
{
    uint8_t v = bus_read(addr & 0xFFFFFF, 1);
    snes_mdr = v;
    return v;
}

void bus_write_a(uint32_t addr, uint8_t v)
{
    snes_mdr = v;
    bus_write(addr & 0xFFFFFF, v, 1);
}

uint8_t snes_peek(uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    uint32_t at;
    int what;
    if (bank == 0x7E || bank == 0x7F)
        return wram[(uint32_t)(bank & 1) << 16 | off];
    if (!(bank & 0x40) && off < 0x6000)
        return off < 0x2000 ? wram[off] : 0;
    what = cart_decode(bank, off, &at);
    if (what == CART_ROM)
        return snes_rom_byte(at, NSHVC_ROM_PEEK);
    if (what == CART_RAM)
        return snes_cart.map == NSHVC_SUPERFX
               ? gsu_ram_mem[gsu_ram_size <= 0x20000 ? (at & 0x1FFFF) & (gsu_ram_size - 1) : at % gsu_ram_size]
               : sram[at & sram_mask];
    return 0;
}

/* ---- the decomp's bus -------------------------------------------------------------- */
unsigned snes_access_clocks(uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    int fast = (memsel & 1) && (bank & 0x80);
    if (bank & 0x40) {
        if (fast && bank >= 0xC0)
            return 6;
        return 8;
    }
    if (off >= 0x8000)
        return fast ? 6 : 8;
    if (off < 0x2000 || off >= 0x6000)
        return 8;
    if (off >= 0x4000 && off < 0x4200)
        return 12;
    return 6;
}

/* A read of the cartridge's ROM by the C: the C states its time with the
   instruction around it (a decomp's checks place the C's reads of RAM and
   the registers on the original's, and leave a ROM read in the time
   between), so it adds none here. */
static int rom_data(uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    uint32_t at;
    if (bank == 0x7E || bank == 0x7F)
        return 0;
    if (!(bank & 0x40) && off < 0x6000)
        return 0;
    return cart_decode(bank, off, &at) == CART_ROM;
}

/* No interrupt is taken between the two bytes of one word: a read
   straight after a read of the byte before it. */
/* Untimed, a loop that waits for time (a status bit, a counter, the
   sound unit's reply) never ends; such a loop reads and does not write.
   20,000,000 reads with no write between them are taken for one. */
static uint32_t untimed_reads;

static void untimed_read(uint32_t addr)
{
    if (++untimed_reads >= 20000000u)
        snes_fatal("untimed, the S-CPU made 20,000,000 reads and no write (the last of $%06X): it waits for time "
                   "that does not pass (nshvc_wait_vblank before such a wait)", (unsigned)addr);
}

static uint8_t access_rd(uint32_t addr)
{
    uint8_t v;
    int second;
    addr &= 0xFFFFFF;
    if (snes_untimed_on && !snes_in_interrupt)
        untimed_read(addr);
    second = last_was_read && addr == ((last_read + 1) & 0xFFFFFF);
    if (!second)
        check_interrupts();
    last_read = addr;
    last_was_read = !second;
    last_was_write = 0;
    /* the clock first: what the read sees is the machine at its end */
    snes_tick(rom_data(addr) ? 0 : snes_access_clocks(addr));
    v = bus_read(addr, 0);
    snes_mdr = v;
    return v;
}

uint8_t rd8(uint32_t addr)
{
    if (audit_on() && bus_a_is_register(addr & 0xFFFFFF)) {
        audit_at = __builtin_return_address(0);
        audit_site(addr & 0xFFFFFF, 0);
    }
    return access_rd(addr);
}

/* A write is a place an interrupt is taken too, as between any two of the
   original's instructions: a stretch of stores alone (a fill loop) would
   otherwise hold a raster interrupt past its line. Not the second byte of
   a word, and not the interrupt sequence's own pushes (`check` 0). */
static void access_wr(uint32_t addr, uint8_t value, int check)
{
    int second;
    addr &= 0xFFFFFF;
    untimed_reads = 0;
    second = last_was_write && addr == ((last_write + 1) & 0xFFFFFF);
    if (check && !second)
        check_interrupts();
    last_write = addr;
    last_was_write = !second;
    last_was_read = 0;
    snes_tick(snes_access_clocks(addr));
    snes_mdr = value;
    if (snes_write_hook)
        snes_write_hook(addr, value);
    bus_write(addr, value, 0);
}

void wr8(uint32_t addr, uint8_t value)
{
    if (audit_on() && bus_a_is_register(addr & 0xFFFFFF)) {
        audit_at = __builtin_return_address(0);
        audit_site(addr & 0xFFFFFF, 1);
    }
    access_wr(addr, value, 1);
}

void snes_interrupt_push(uint32_t addr, uint8_t value) { access_wr(addr, value, 0); }

/* A stretch with no access (a delay loop): the clock moves on in short
   steps, interrupts taken between them as between the loop's turns */
void snes_cpu_wait(uint32_t clocks)
{
    while (clocks) {
        unsigned n = clocks > 36 ? 36 : (unsigned)clocks;
        snes_tick(n);
        clocks -= n;
        check_interrupts();
    }
    last_was_read = last_was_write = 0;
}
void snes_cpu_clocks(uint32_t clocks) { snes_tick(clocks); }
int snes_interrupt_due(void) { return nmi_pending || timeup_flag || cart_irq; }
void snes_cpu_interrupts(void) { check_interrupts(); }

/* ---- power ---------------------------------------------------------------------- */
void snes_power(void)
{
    uint32_t n;
    memset(wram, 0, sizeof wram);
    wram_port = 0;
    free(sram);
    sram = NULL;
    sram_mask = 0;
    if (snes_cart.map != NSHVC_SUPERFX && snes_cart.sram_size) {
        for (n = 1; n < snes_cart.sram_size; n <<= 1)
            ;
        sram = (uint8_t *)malloc(n);
        if (!sram)
            snes_fatal("no memory for the cartridge's RAM");
        /* as a cartridge with no save yet powers on: all ones */
        memset(sram, 0xFF, n);
        sram_mask = n - 1;
    }
    snes_clock = 0;
    nmitimen = 0;
    wrio = 0xFF;
    wrmpya = 0xFF;
    wrmpyb = wrdivb = memsel = 0;
    wrdiv = 0xFFFF;
    rddiv = rdmpy = 0;
    htime = vtime = 0x1FF;
    rdnmi_flag = timeup_flag = 0;
    nmi_pending = cart_irq = 0;
    joy_auto[0] = joy_auto[1] = 0;
    joy_shift[0] = joy_shift[1] = 0;
    joy_strobe = 0;
    joy_busy_until = 0;
    lines = snes_cart.region == NSHVC_PAL ? SNES_LINES_PAL : SNES_LINES_NTSC;
    vblank_line = 225;
    vline = 0;
    line_start = 0;
    frames = vblanks = 0;
    snes_untimed_on = snes_in_interrupt = 0;
    interrupts_held = 0;
    line_done_hblank = line_done_refresh = 0;
    irq_at = NEVER;
    last_read = last_write = 0xFFFFFFFFu;
    last_was_read = last_was_write = 0;
    ppu_reset();
    dma_reset();
    gsu_reset();
    apu_reset();
    events_next();
}

/* ---- untimed (nshvc.h) ---------------------------------------------------------- */
void nshvc_set_untimed(int on)
{
    snes_untimed_on = on != 0;
    untimed_reads = 0;
}
int nshvc_untimed(void) { return snes_untimed_on; }

void nshvc_wait_vblank(void)
{
    int was = snes_untimed_on;
    uint32_t v = vblanks;
    snes_untimed_on = 0;
    while (vblanks == v)
        snes_cpu_wait(64);
    snes_untimed_on = was;
}
