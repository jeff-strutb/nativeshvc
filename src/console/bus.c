/*
 * bus.c: the S-CPU's bus and clock.
 *
 * rd8 and wr8 are the decomp's view of the machine (machine.h): one byte of
 * the 24-bit address space each. Here they reach the work RAM, the
 * cartridge (its ROM and RAM where its mapping puts them, nshvc.h
 * nshvc_map; what its chip decodes, cart.h), the PPU and the sound
 * ports on the B bus ($2100-$21FF), the joypads ($4016-$4017) and the CPU's
 * own registers ($4200-$421F, DMA at $4300).
 *
 * The S-CPU's C takes no time: its accesses are made at the clock as it
 * stands. The clock moves on where the S-CPU meets the hardware: its waits
 * (machine.h nshvc_wait_until, nshvc_wait_vblank, cpu_wai,
 * cpu_loop_forever), each in short steps of the poll loop it stands for,
 * and a DMA, which stops the S-CPU for the DMA unit's real time. Passing a
 * line's horizontal blank draws that line and runs HDMA; passing the
 * vertical blank's line (225, or 240 with overscan) starts the vertical
 * blank (the NMI flag, the joypads' automatic read, the frame's hook).
 * Interrupts are taken where they come due in a wait, or at the end of the
 * access a DMA's time made them due in.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_internal.h"
#include "nshvc/cart.h"
#include "coro.h"
#ifndef _WIN32
#include <dlfcn.h>
#endif

uint8_t rd8(uint32_t addr);
void wr8(uint32_t addr, uint8_t value);

nshvc_cart snes_cart;
uint64_t snes_clock;
uint8_t snes_mdr;

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
/* The multiplier and divider: 8 steps for a multiply, 16 for a divide,
   RDDIV and RDMPY changing as they go. The S-CPU's C takes no time, so its
   next read comes after every step: a start runs them all (alu_run) */
static uint8_t mpy_left, div_left;
static uint32_t alu_shift;

static void alu_edge(void)
{
    if (mpy_left) {
        mpy_left--;
        if (rddiv & 1)
            rdmpy = (uint16_t)(rdmpy + alu_shift);
        rddiv >>= 1;
        alu_shift <<= 1;
    }
    if (div_left) {
        div_left--;
        rddiv = (uint16_t)(rddiv << 1);
        alu_shift >>= 1;
        if (rdmpy >= alu_shift) {
            rdmpy = (uint16_t)(rdmpy - alu_shift);
            rddiv |= 1;
        }
    }
}
static void alu_run(void)
{
    while (mpy_left || div_left)
        alu_edge();
}

static uint16_t htime = 0x1FF, vtime = 0x1FF;
static uint8_t rdnmi_flag, timeup_flag;
/* when each interrupt became due */
static uint64_t nmi_at, timeup_at;
static int nmi_pending, cart_irq;
static uint64_t cart_irq_at;    /* the clock the cartridge's IRQ line rose at */

static void latch_counters(void);
static void joy_run(void);

/* the joypads */
/* the joypads' automatic read (ports.c the devices): from a 256-clock
   step of the CPU's counter at dot 130-384 of line vdisp (its counter
   snes_clock + 4, as the counters' dots stand 4 clocks on: measured,
   joy_busy), an edge each 128 clocks: the latch set at the first, let go
   at the second, then each pair a bit, read at the first edge and shifted
   in at the second, 16 of them; 33 the read not under way */
static uint16_t joy_reg[4];
static unsigned joy_counter = 33;
static int joy_cpu_latch, joy_auto_latch;
static unsigned joy_bits[2];
static uint64_t joy_next;

/* the beam: the line now and where it began */
static unsigned vline, lines = SNES_LINES_NTSC, vblank_line = 225;
static uint64_t line_start;
/* this line's length: 1,364 clocks, but NTSC's line 240 in field 1 when not
   interlaced (1,360, its dots all 4 clocks) and PAL's line 311 in field 1
   when interlaced (1,368, a dot 340 after 339) */
static unsigned line_len = SNES_LINE_CLOCKS;
static uint32_t frames, vblanks;
static int line_done_hblank, line_done_refresh, line_done_hdma, line_done_setup, line_done_nmi_flag, line_done_edge;
/* the NMI's condition (the counters' line at or past the vertical blank's) as
   last tested, and its rise in this line */
static int nmi_valid, nmi_rose;
static uint64_t irq_at, next_event;
#define NEVER UINT64_MAX
#define HBLANK_AT 1096                    /* dot 274 */
/* the work RAM's refresh: the S-CPU stopped for 40 clocks once a line,
   near dot 133 (as the DMA unit's clock stands as the line begins) */
#define REFRESH_AT 534
#define REFRESH_CLOCKS 40
/* HDMA's line transfers (lines 0-224) and, in line 0, the frame's setup:
   due here, made at the start of the CPU's next cycle */
#define HDMA_AT 1100
#define HDMA_SETUP_AT 8

/* The DMA unit's clock: the master clock in steps of 8, as the unit sees
   it (its phase against snes_clock measured: irq_entry, frame_clock) */
#define DMA_PHASE 4
static unsigned dma_counter(uint64_t clock) { return (unsigned)((clock + DMA_PHASE) & 7); }
/* the line's refresh and HDMA setup times, by the unit's clock as the
   line begins (4 clocks before line_start by the counters) */
static uint64_t refresh_at(void) { return line_start + REFRESH_AT - dma_counter(line_start - 4); }
static uint64_t setup_at(void) { return line_start + HDMA_SETUP_AT + dma_counter(line_start - 4); }

/* The DMA unit's work waiting (MDMAEN's transfers; HDMA's setup, mode 0,
   or a line, mode 1) and whether it is taking the bus: work that arrives
   is taken up at the start of the CPU's next cycle and made at the start
   of the one after (unit_edge) */
static int dma_pending, hdma_pending, hdma_mode, dma_active;
static unsigned unit_cc;        /* the CPU's cycle the unit's work comes in, its clocks */

static unsigned dbg_irqs, dbg_nmis;          /* for NSHVC_DEBUG */
static uint32_t dbg_last_read;                /* for NSHVC_DEBUG */

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
   are 6 (1,364 clocks a line); a short line's are all 4. */
static unsigned dot_of(uint64_t c)
{
    if (c < 323 * 4 || line_len < SNES_LINE_CLOCKS)
        return (unsigned)(c / 4);
    if (c < 323 * 4 + 6)
        return 323;
    if (c < 323 * 4 + 6 + 12)
        return (unsigned)(324 + (c - (323 * 4 + 6)) / 4);
    if (c < 323 * 4 + 6 + 12 + 6)
        return 327;
    return (unsigned)(328 + (c - (323 * 4 + 6 + 12 + 6)) / 4);
}

/* The counters as the PPU shows them now: 4 clocks on from the line's
   clock (into the next line in this line's last 4) */
static unsigned frame_lines(void);
static void counters(unsigned *v, uint64_t *h)
{
    *v = vline;
    *h = snes_clock - line_start + 4;
    if (*h >= line_len) {
        *h -= line_len;
        *v = vline + 1 >= frame_lines() ? 0 : vline + 1;
    }
}

unsigned snes_hcounter(void) { return dot_of(snes_clock - line_start); }
uint32_t snes_line_clock(void) { return (uint32_t)(snes_clock - line_start); }
unsigned snes_lines(void) { return lines; }
int snes_pal(void) { return snes_cart.region == NSHVC_PAL; }
unsigned snes_vblank_line(void) { return vblank_line; }

/* the vertical blank's first line as SETINI stands now (the frame's picture
   keeps the one it began with: snes_vblank_line) */
unsigned snes_vdisp(void) { return ppu_overscan() ? 240 : 225; }
const uint8_t *snes_wram(void) { return wram; }
uint8_t *snes_wram_rw(void) { return wram; }
/* the cartridge's expansion chip (nshvc_cart.chip, cart.h), or NULL */
static const nshvc_chip *chip;

static void chip_sync(void)
{
    if (chip && chip->sync)
        chip->sync();
}

/* the cartridge's RAM: the chip's when it owns it */
uint8_t *snes_sram(void)
{
    uint32_t n;
    return chip && chip->ram ? chip->ram(&n) : sram;
}
uint32_t snes_sram_size(void)
{
    uint32_t n = 0;
    if (chip && chip->ram) {
        chip->ram(&n);
        return n;
    }
    return snes_cart.sram_size;
}

/* ---- interrupts and the beam --------------------------------------------------- */
/* The IRQ is seen by the CPU 14 clocks after 4 x HTIME (the timer
   compares in steps of 4 clocks, the long dots not counted; with the CPU's
   sampling of the line; measured: irq_fixed, irq_latch) */
#define IRQ_DELAY 14
/* the NMI, 2 clocks after its line begins (the same sampling) */
#define NMI_DELAY 2

/* This line's IRQ time. At the line's start (written 0) every time on the
   line counts, however far the clock has run on; after a write to the
   registers mid-line, a time already passed gives no IRQ this line. */
static void irq_schedule(int written)
{
    unsigned mode = (nmitimen >> 4) & 3;
    uint64_t at = NEVER;
    if (mode == 1 && htime < 340)
        at = line_start + 4u * htime + IRQ_DELAY;
    else if (mode == 2 && vline == vtime)
        at = line_start + 10;
    else if (mode == 3 && vline == vtime && htime < 340)
        at = line_start + 4u * htime + IRQ_DELAY;
    /* none at dot 339 of the frame's last line: its compare falls on the
       counters' first dot of the next field */
    if (htime == 339 && (mode == 1 || mode == 3) && vline + 1 == frame_lines())
        at = NEVER;
    irq_at = at;
    if (written && at != NEVER && at < snes_clock)
        irq_at = NEVER;
}

static void events_next(void)
{
    uint64_t e = line_start + line_len;
    if (irq_at != NEVER && irq_at - 4 < e)
        e = irq_at - 4;
    if (!line_done_edge && line_start + line_len - 4 < e)
        e = line_start + line_len - 4;
    if (!line_done_nmi_flag && line_start + line_len - 2 < e)
        e = line_start + line_len - 2;
    if (!line_done_refresh && refresh_at() < e)
        e = refresh_at();
    if (!line_done_hblank && line_start + HBLANK_AT < e)
        e = line_start + HBLANK_AT;
    if (!line_done_hdma && line_start + HDMA_AT < e)
        e = line_start + HDMA_AT;
    if (!line_done_setup && setup_at() < e)
        e = setup_at();
    next_event = irq_at < e ? irq_at : e;
}

/* The frame's lines: 262 (PAL 312), and one more in an interlaced frame's
   field 0 (interlaced as SETINI stood when the frame began), settled as the
   counters reach line 128 */
static int frame_interlace;
static unsigned frame_len = SNES_LINES_NTSC;

static unsigned frame_lines(void)
{
    return frame_len;
}

static unsigned line_length(void)
{
    if (!snes_pal() && vline == 240 && !frame_interlace && ppu_field())
        return SNES_LINE_CLOCKS - 4;
    if (snes_pal() && vline == 311 && frame_interlace && ppu_field())
        return SNES_LINE_CLOCKS + 4;
    return SNES_LINE_CLOCKS;
}

static void line_begin(void)
{
    if (vline == snes_vdisp()) {
        unsigned h0;
        joy_run();
        /* the light guns see the finished picture; the joypads' read
           starts on its step of 256 in dots 130-384 */
        ports_frame();
        h0 = 130 + ((256 - (unsigned)((line_start + 130) & 255)) & 255);
        joy_next = line_start - 4 + h0;
        joy_counter = 34;
    }
    if (nmi_rose) {
        nmi_rose = 0;
        vblanks++;
        if (nmitimen & 0x80) {
            nmi_pending = 1;
            nmi_at = line_start + NMI_DELAY;
        }
        ppu_vblank_start();
        if (getenv("NSHVC_DEBUG")) {
            char a[128];
            apu_debug(a, sizeof a);
            fprintf(stderr, "frame %u: %s; irqs %u nmis %u nmitimen %02X vtime %u htime %u; last read %06X, "
                    "chip %s, APU in %02X %02X %02X %02X out %02X %02X %02X %02X\n",
                    frames, a, dbg_irqs, dbg_nmis, nmitimen, vtime, htime, (unsigned)dbg_last_read,
                    chip && chip->busy ? (chip->busy() ? "running" : "idle") : "none",
                    apu_debug_port(0), apu_debug_port(1), apu_debug_port(2), apu_debug_port(3),
                    apu_port_read(0), apu_port_read(1), apu_port_read(2), apu_port_read(3));
            dbg_irqs = dbg_nmis = 0;
        }
        /* the frame is done: the front end shows it and sets what the
           players hold, which the joypads' read now takes */
        if (snes_frame_hook)
            snes_frame_hook();
    } else if (vline == 0) {
        frames++;
        /* overscan (SETINI bit 2) as the frame starts: 239 lines shown */
        vblank_line = ppu_overscan() ? 240 : 225;
        frame_interlace = ppu_interlace();
        ppu_frame_start();
    }
    irq_schedule(0);
}

/* the unit takes the bus: it waits for its clock's next step of 8 */
static void unit_align(void)
{
    unsigned align = 8 - dma_counter(snes_clock);
    snes_tick(align);
    dma_clocks = align;
}

/* and gives it back on the CPU's own clock: a whole number of the cycle it
   came in on (unit_cc) after it took it */
static void unit_back(void)
{
    if (unit_cc)
        snes_tick(unit_cc - dma_clocks % unit_cc);
}

/* An edge of the DMA unit: the start of each CPU cycle, and inside a DMA
   after its start and each byte (dma_edge). HDMA first: aligned and given
   back on its own when no DMA is under way, else in the DMA's time; then
   a DMA waiting. */
static void unit_edge(void)
{
    if (dma_active) {
        if (hdma_pending) {
            hdma_pending = 0;
            if (hdma_enabled()) {
                if (!dma_mdmaen)
                    unit_align();
                if (hdma_mode == 0)
                    hdma_init();
                else
                    hdma_line();
                if (!dma_mdmaen) {
                    unit_back();
                    dma_active = 0;
                }
            }
        }
        if (dma_pending) {
            dma_pending = 0;
            if (dma_mdmaen) {
                unit_align();
                dma_run();
                unit_back();
                dma_active = 0;
            }
        }
    }
    if (!dma_active && (dma_pending || hdma_pending))
        dma_active = 1;
}

void dma_edge(void) { unit_edge(); }

/* MDMAEN written: the unit takes the bus at once (HDMA due first), the
   S-CPU stopped until it gives it back, on a cycle of the S-CPU's next
   fetch's length (8) */
static void dma_now(void)
{
    unsigned keep = unit_cc;
    dma_pending = 1;
    dma_active = 1;
    unit_cc = 8;
    unit_edge();
    unit_cc = keep;
}

static void run_events(void)
{
    for (;;) {
        /* the IRQ line rises (TIMEUP reads it) 4 clocks before the CPU
           sees it */
        if (irq_at != NEVER && snes_clock + 4 >= irq_at) {
            timeup_flag = 1;
            timeup_at = irq_at;
            irq_at = NEVER;
            continue;
        }
        /* the NMI flag rises (and falls at the frame's end) as the counters
           reach the line, 2 clocks before the line's start here */
        /* the counters reach the next line 4 clocks before its start here:
           a new frame's field, and at line 128 the frame's length */
        if (!line_done_edge && snes_clock + 4 >= line_start + line_len) {
            unsigned next = vline + 1 >= frame_lines() ? 0 : vline + 1;
            line_done_edge = 1;
            if (next == 0) {
                ppu_field_flip();
                frame_len = lines;
            } else if (next == 128) {
                frame_len = lines + (frame_interlace && !ppu_field());
            }
            continue;
        }
        /* the NMI's condition is tested 2 clocks later: its rise sets the
           flag (and the NMI), its fall clears the flag */
        if (!line_done_nmi_flag && snes_clock + 2 >= line_start + line_len) {
            unsigned next = vline + 1 >= frame_lines() ? 0 : vline + 1;
            int valid = next >= snes_vdisp();
            line_done_nmi_flag = 1;
            if (valid != nmi_valid) {
                rdnmi_flag = nmi_rose = valid;
                nmi_valid = valid;
            }
            continue;
        }
        if (!line_done_refresh && snes_clock >= refresh_at()) {
            line_done_refresh = 1;
            snes_clock += REFRESH_CLOCKS;
            continue;
        }
        if (!line_done_hblank && snes_clock >= line_start + HBLANK_AT) {
            line_done_hblank = 1;
            if (vline >= 1 && vline < vblank_line)
                ppu_render_line(vline);
            chip_sync();
            apu_sync();
            continue;
        }
        if (!line_done_setup && snes_clock >= setup_at()) {
            line_done_setup = 1;
            if (hdma_enabled()) {
                hdma_pending = 1;
                hdma_mode = 0;
            }
            continue;
        }
        if (!line_done_hdma && snes_clock >= line_start + HDMA_AT) {
            line_done_hdma = 1;
            if (vline < snes_vdisp() && hdma_active()) {
                hdma_pending = 1;
                hdma_mode = 1;
            }
            continue;
        }
        if (snes_clock >= line_start + line_len) {
            /* HDMA the CPU made no cycle for (a stop) is made at the line's
               end */
            if (hdma_pending) {
                unsigned keep = unit_cc;
                dma_active = 1;
                unit_cc = 0;
                unit_edge();
                unit_cc = keep;
            }
            /* an IRQ time past the line's end (H 339 and the IRQ's own
               delay) comes early in the next line */
            uint64_t carried = irq_at;
            line_start += line_len;
            vline = vline + 1 >= frame_lines() ? 0 : vline + 1;
            line_len = line_length();
            line_done_hblank = line_done_refresh = line_done_hdma = line_done_nmi_flag = line_done_edge = 0;
            line_done_setup = vline != 0;
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

static int interrupts_held;

void snes_tick(unsigned clocks)
{
    snes_clock += clocks;
    if (snes_clock >= next_event)
        run_events();
}

/* The cartridge's IRQ line, changed at the chip's clock `at`: a rise is
   due from then, the earliest kept while the line stays high. */
void snes_cart_irq(int level, uint64_t at)
{
    if (!level) {
        cart_irq = 0;
        return;
    }
    if (!cart_irq) {
        cart_irq = 1;
        cart_irq_at = at;
    }
}

/* The chip's clock: a sync brings it to nshvc_cart_clock, the console's
   own, or, run ahead through a stretch with no access of the S-CPU's in
   it, that stretch's end, brought back to the clock its IRQ rises at so
   the chip stops there (chip_ahead). */
static uint64_t chip_to;
static int chip_ahead;

/* whether a rise of the chip's IRQ could be taken now: a chip that syncs,
   running (busy, when it says), its line low, interrupts not held, and
   (`unmasked`) the IRQ not masked */
static int chip_watch(int unmasked)
{
    if (!chip || !chip->sync || cart_irq || interrupts_held)
        return 0;
    if (unmasked && (!snes_interrupt_hook || (snes_irq_masked && snes_irq_masked())))
        return 0;
    return !chip->busy || chip->busy();
}

/* the chip run ahead `n` clocks, where nothing the S-CPU does can reach
   it, so a rise of its IRQ in them is seen at its clock: 1 when it rose */
static int chip_run_ahead(uint32_t n)
{
    if (!chip_watch(0))
        return 0;
    chip_to = snes_clock + n;
    chip_ahead = 1;
    chip->sync();
    chip_ahead = 0;
    return cart_irq;
}

/* nshvc_hold_interrupts: due interrupts wait while held */
void nshvc_hold_interrupts(int on)
{
    on = on != 0;
    /* the chip keeps what it holds of its interrupt (cart.h) */
    if (chip && chip->hold && on != interrupts_held)
        chip->hold(on);
    interrupts_held = on;
}

/* the interrupt due now, taken (the IRQ unless masked): 1 when one was */
static int check_interrupts(void)
{
    int masked = snes_irq_masked && snes_irq_masked();
    if (interrupts_held)
        return 0;
    if (nmi_pending && nmi_at <= snes_clock) {
        nmi_pending = 0;
        dbg_nmis++;
        if (snes_interrupt_hook)
            snes_interrupt_hook(1);
        return 1;
    }
    if (((timeup_flag && timeup_at <= snes_clock) || (cart_irq && cart_irq_at <= snes_clock)) && snes_interrupt_hook &&
        !masked) {
        dbg_irqs++;
        snes_interrupt_hook(0);
        return 1;
    }
    return 0;
}

/* a cycle of the CPU's begins, `cc` clocks long: where the DMA unit takes
   the bus */
static void cycle_begins(unsigned cc)
{
    if (dma_active || dma_pending || hdma_pending) {
        unit_cc = cc;
        unit_edge();
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
        /* the read clears the flag, but not in the 4 clocks it is held
           after it rises */
        v = (uint8_t)(timeup_flag << 7 | (snes_mdr & 0x7F));
        if (snes_clock >= timeup_at)
            timeup_flag = 0;
        return v;
    case 0x4212: {
        unsigned cv;
        uint64_t h;
        counters(&cv, &h);
        v = (uint8_t)(snes_mdr & 0x3E);
        if (cv >= snes_vdisp())
            v |= 0x80;
        if (h <= 2 || h >= HBLANK_AT)
            v |= 0x40;
        joy_run();
        if ((nmitimen & 1) && joy_counter < 33)
            v |= 0x01;
        return v;
    }
    case 0x4213: return wrio;
    case 0x4214: return (uint8_t)rddiv;
    case 0x4215: return (uint8_t)(rddiv >> 8);
    case 0x4216: return (uint8_t)rdmpy;
    case 0x4217: return (uint8_t)(rdmpy >> 8);
    case 0x4218: case 0x4219: case 0x421A: case 0x421B:
    case 0x421C: case 0x421D: case 0x421E: case 0x421F:
        joy_run();
        return (uint8_t)(joy_reg[(off - 0x4218) >> 1] >> ((off & 1) * 8));
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
        joy_run();
        nmitimen = v;
        /* the read's first step: the latch follows the enable; past its
           second, a disable ends it */
        if (joy_counter == 0) {
            joy_auto_latch = v & 1;
            ports_latch(joy_auto_latch | joy_cpu_latch);
        } else if (!(v & 1) && joy_counter >= 2 && joy_counter < 33) {
            joy_counter = 33;
            joy_next = NEVER;
        }
        /* the NMI enabled during the vertical blank, its flag still set */
        if (!(was & 0x80) && (v & 0x80) && rdnmi_flag) {
            nmi_pending = 1;
            nmi_at = snes_clock;
        }
        if (!(v & 0x30))
            timeup_flag = 0;
        irq_schedule(1);
        events_next();
        return;
    }
    case 0x4201:
        joy_run();
        if ((wrio & 0x80) && !(v & 0x80))
            latch_counters();
        wrio = v;
        return;
    case 0x4202: wrmpya = v; return;
    case 0x4203:
        rdmpy = 0;
        wrmpyb = v;
        rddiv = (uint16_t)(wrmpyb << 8 | wrmpya);
        mpy_left = 8;
        alu_shift = wrmpyb;
        alu_run();
        return;
    case 0x4204: wrdiv = (uint16_t)((wrdiv & 0xFF00) | v); return;
    case 0x4205: wrdiv = (uint16_t)((wrdiv & 0x00FF) | v << 8); return;
    case 0x4206:
        rdmpy = wrdiv;
        wrdivb = v;
        div_left = 16;
        alu_shift = (uint32_t)wrdivb << 16;
        alu_run();
        return;
    case 0x4207: htime = (uint16_t)((htime & 0x100) | v); irq_schedule(1); events_next(); return;
    case 0x4208: htime = (uint16_t)((htime & 0xFF) | (v & 1) << 8); irq_schedule(1); events_next(); return;
    case 0x4209: vtime = (uint16_t)((vtime & 0x100) | v); irq_schedule(1); events_next(); return;
    case 0x420A: vtime = (uint16_t)((vtime & 0xFF) | (v & 1) << 8); irq_schedule(1); events_next(); return;
    case 0x420B:
        /* the channels' transfers: the S-CPU stops for them */
        dma_mdmaen = v;
        if (v)
            dma_now();
        return;
    case 0x420C: dma_hdmaen = v; return;
    case 0x420D: memsel = v; return;
    }
    if (off >= 0x4300 && off < 0x4380)
        dma_write(off, v);
}

/* one edge of the automatic read (start: the frame's first) */
static void joy_edge(int start)
{
    int on = nmitimen & 1;
    if (start)
        joy_counter = 0;
    else if (joy_counter >= 33)
        return;
    else
        joy_counter++;
    if (joy_counter == 0) {
        joy_auto_latch = on;
        ports_latch(joy_auto_latch | joy_cpu_latch);
        if (on)
            joy_reg[0] = joy_reg[1] = joy_reg[2] = joy_reg[3] = 0;
    }
    if (joy_counter == 1) {
        joy_auto_latch = 0;
        ports_latch(joy_cpu_latch);
    }
    if (joy_counter != 1 && !on) {
        joy_counter = 33;
        return;
    }
    if (joy_counter >= 2) {
        if (!(joy_counter & 1)) {
            joy_bits[0] = ports_data(0);
            joy_bits[1] = ports_data(1);
        } else {
            joy_reg[0] = (uint16_t)(joy_reg[0] << 1 | (joy_bits[0] & 1));
            joy_reg[1] = (uint16_t)(joy_reg[1] << 1 | (joy_bits[1] & 1));
            joy_reg[2] = (uint16_t)(joy_reg[2] << 1 | (joy_bits[0] >> 1 & 1));
            joy_reg[3] = (uint16_t)(joy_reg[3] << 1 | (joy_bits[1] >> 1 & 1));
        }
    }
}

/* the automatic read's edges up to now (counter 34: the frame's first
   edge still to come) */
static void joy_run(void)
{
    while (joy_next <= snes_clock) {
        joy_edge(joy_counter == 34);
        joy_next = joy_counter < 33 ? joy_next + 128 : NEVER;
    }
}

static void joy_latch_write(uint8_t v)
{
    joy_run();
    joy_cpu_latch = v & 1;
    ports_latch(joy_auto_latch | joy_cpu_latch);
}

static uint8_t joy_serial_read(int port)
{
    joy_run();
    return (uint8_t)ports_data(port);
}

uint8_t snes_wrio(void) { return wrio; }

/* ---- the counters' latch (the PPU's $2137, $213C-$213D, $213F) ---- */
/* SLHV's read and WRIO's write latch the counters as they stand
   (measured: beam_walk, irq_fixed) */
static void latch_counters(void)
{
    unsigned v;
    uint64_t h;
    counters(&v, &h);
    ppu_latch_counters((uint16_t)dot_of(h), (uint16_t)v);
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
    /* nativeshvc's own registers, past the console's (nshvc.h): the VRAM
       pages, $21C0-$21C4 each layer's, $21C5 the data port's */
    if (reg >= 0xC0 && reg <= 0xC5) {
        ppu_ext_page_reg(reg, v);
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
    case NSHVC_CHIP:
        /* the board the chip is on decodes it (cart.h rom_at) */
        if (chip && chip->rom_at && chip->rom_at((uint32_t)bank << 16 | off, at)) {
            *at = snes_rom_mirror(*at);
            return CART_ROM;
        }
        return CART_NONE;
    }
    return CART_NONE;
}

static inline uint8_t *cart_ram(uint32_t at) { return &sram[at & sram_mask]; }

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
        if (off == 0x4016)
            return (uint8_t)((snes_mdr & 0xFC) | joy_serial_read(0));
        if (off == 0x4017)
            return (uint8_t)((snes_mdr & 0xE0) | 0x1C | joy_serial_read(1));
        if (off >= 0x4200 && off < 0x4400)
            return cpu_reg_read(off);
        {
            uint8_t v;
            if (chip && chip->read && chip->read(addr, &v))
                return v;
        }
        return snes_mdr;
    }
    {
        uint8_t v;
        if (chip && chip->read && chip->read(addr, &v))
            return v;
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
        if (off == 0x4016) {
            joy_latch_write(v);
            return;
        }
        if (off >= 0x4200 && off < 0x4400) {
            cpu_reg_write(off, v);
            return;
        }
        if (chip && chip->write)
            chip->write(addr, v);
        return;
    }
    if (chip && chip->write && chip->write(addr, v))
        return;
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
    {
        uint8_t v;
        if (chip && chip->peek && chip->peek(addr, &v))
            return v;
    }
    what = cart_decode(bank, off, &at);
    if (what == CART_ROM)
        return snes_rom_byte(at, NSHVC_ROM_PEEK);
    if (what == CART_RAM)
        return sram[at & sram_mask];
    return 0;
}

/* ---- the decomp's bus -------------------------------------------------------------- */
/* The S-CPU's C takes no time, so a loop of it that waits for the
   hardware (a status bit, a counter, the sound unit's reply) without
   nshvc_wait_until never ends; such a loop reads and does not write.
   20,000,000 reads with no write or wait between them are taken for one. */
static uint32_t spin_reads;

static uint8_t access_rd(uint32_t addr)
{
    uint8_t v;
    addr &= 0xFFFFFF;
    if (++spin_reads >= 20000000u)
        snes_fatal("the S-CPU made 20,000,000 reads and no write (the last of $%06X): a loop that waits for the "
                   "hardware with no wait (machine.h nshvc_wait_until)", (unsigned)addr);
    dbg_last_read = addr;
    v = bus_read(addr, 0);
    snes_mdr = v;
    return v;
}

/* ---- the C's operations, and the interrupts between them ----
   The CPU takes an interrupt between two of its operations, never inside
   one. The C's operations are what it states: an access (rd8, wr8), a
   word or long (rd16, wr16, rd24), a read-modify-write (rd8_rmw or
   rd16_rmw to its write). They take no time, so an interrupt comes due in
   a wait, and is taken there at its clock; or in a DMA a write started,
   and is taken at that write's end. */
static int rmw_open;                     /* a read-modify-write's read made, its write to come */
static uint32_t rmw_at;

/* when an interrupt raised and not masked comes due: NEVER for none */
static uint64_t interrupt_due_at(void)
{
    uint64_t t = NEVER;
    if (interrupts_held || !snes_interrupt_hook)
        return NEVER;
    if (nmi_pending)
        t = nmi_at;
    if (!(snes_irq_masked && snes_irq_masked())) {
        if (cart_irq && cart_irq_at < t)
            t = cart_irq_at;
        if (timeup_flag && timeup_at < t)
            t = timeup_at;
    }
    return t;
}

/* a point between two of the C's operations: the interrupt due by now is
   taken there (the chip brought up to now, so its IRQ is seen) */
static void between(void)
{
    if (rmw_open)
        return;
    if (chip_watch(1))
        chip_sync();
    check_interrupts();
}

static uint8_t c_rd(uint32_t addr)
{
    if (rmw_open)
        snes_fatal("rd8_rmw at $%06X: a read of $%06X before its write", (unsigned)rmw_at, (unsigned)(addr & 0xFFFFFF));
    return access_rd(addr);
}

uint8_t rd8(uint32_t addr)
{
    uint8_t v = c_rd(addr);
    between();
    return v;
}

uint16_t rd16(uint32_t addr)
{
    uint8_t lo = c_rd(addr);
    uint16_t v = (uint16_t)(lo | c_rd((addr + 1) & 0xFFFFFF) << 8);
    between();
    return v;
}

uint32_t rd24(uint32_t addr)
{
    uint32_t v = c_rd(addr);
    v |= (uint32_t)c_rd((addr + 1) & 0xFFFFFF) << 8;
    v |= (uint32_t)c_rd((addr + 2) & 0xFFFFFF) << 16;
    between();
    return v;
}

uint8_t rd8_rmw(uint32_t addr)
{
    uint8_t v = c_rd(addr);
    rmw_open = 1;
    rmw_at = addr & 0xFFFFFF;
    return v;
}

uint16_t rd16_rmw(uint32_t addr)
{
    uint8_t lo = c_rd(addr);
    uint16_t v = (uint16_t)(lo | c_rd((addr + 1) & 0xFFFFFF) << 8);
    rmw_open = 1;
    rmw_at = addr & 0xFFFFFF;
    return v;
}

static void access_wr(uint32_t addr, uint8_t value)
{
    addr &= 0xFFFFFF;
    spin_reads = 0;
    snes_mdr = value;
    if (snes_write_hook)
        snes_write_hook(addr, value);
    bus_write(addr, value, 0);
}

/* a write ends the read-modify-write whose read came before it */
void wr8(uint32_t addr, uint8_t value)
{
    access_wr(addr, value);
    rmw_open = 0;
    between();
}

void wr16(uint32_t addr, uint16_t value)
{
    access_wr(addr, (uint8_t)value);
    access_wr((addr + 1) & 0xFFFFFF, (uint8_t)(value >> 8));
    rmw_open = 0;
    between();
}

void wr16_rmw(uint32_t addr, uint16_t value)
{
    access_wr((addr + 1) & 0xFFFFFF, (uint8_t)(value >> 8));
    access_wr(addr, (uint8_t)value);
    rmw_open = 0;
    between();
}

void snes_interrupt_push(uint32_t addr, uint8_t value) { access_wr(addr, value); }
uint8_t snes_cpu_read_within(uint32_t addr) { return access_rd(addr); }

/* ---- the S-CPU's waits ----
   A wait is the original's poll loop: the clock moves on in steps of up to
   `most` clocks, each the start of a cycle of the loop's (where the DMA
   unit takes the bus), cut short where an event of the console's comes
   (a line's parts, an HDMA, the IRQ's time), where a bit of HVBJOY
   changes (the horizontal blank's start, an edge of the joypads' read),
   where an interrupt comes due, and where the cartridge's chip raises its
   IRQ (the chip run ahead through the step, cart.h): the loop sees each
   at its clock. At each step's end the interrupt due is taken, its
   handler run, before the wait looks again. 1 when one was. */
static void bound(uint32_t *n, uint64_t at)
{
    if (at > snes_clock && at - snes_clock < *n)
        *n = (uint32_t)(at - snes_clock);
}

static int wait_step(uint32_t most)
{
    uint32_t n = most;
    spin_reads = 0;
    /* one due already (one a handler held, its I flag now clear) */
    if (check_interrupts())
        return 1;
    bound(&n, next_event);
    bound(&n, interrupt_due_at());
    /* the IRQ, masked or not: WAI goes on at it */
    if (timeup_flag)
        bound(&n, timeup_at);
    /* HVBJOY's horizontal blank, 4 clocks before HBLANK_AT by the counters */
    bound(&n, line_start + HBLANK_AT - 4);
    bound(&n, joy_next);
    /* the chip's IRQ, masked or not: a chip's flag changes with it */
    if (cart_irq)
        bound(&n, cart_irq_at);
    /* the chip's IRQ, from its own clock: at once, when it rose in time
       already passed (a refresh) */
    if (chip_run_ahead(n) && cart_irq_at < snes_clock + n)
        n = cart_irq_at > snes_clock ? (uint32_t)(cart_irq_at - snes_clock) : 0;
    {
        /* the DMA unit's work due is taken up at a cycle's start and made
           at the next's: that step one cycle, so it comes as in the loop;
           made, it is the step */
        uint64_t at = snes_clock;
        int was = dma_active;
        cycle_begins(6);
        if (snes_clock == at) {
            if (dma_active && !was && n > 6)
                n = 6;
            snes_tick(n);
        }
    }
    if (chip_watch(1))
        chip_sync();
    return check_interrupts();
}

/* a wait while interrupts are held (code an app runs outside the console's
   time) could never end as the console's would */
static void wait_allowed(const char *what)
{
    if (interrupts_held)
        snes_fatal("%s with interrupts held: the console's time does not pass in code run held", what);
}

/* the step of a wait: a poll loop's turn is a few of the S-CPU's cycles */
#define WAIT_STEP 64

uint64_t nshvc_wait_until(int (*ready)(void *ctx), void *ctx)
{
    uint64_t from = snes_clock;
    wait_allowed("nshvc_wait_until");
    if (rmw_open)
        snes_fatal("nshvc_wait_until between rd8_rmw at $%06X and its write", (unsigned)rmw_at);
    while (!ready(ctx))
        wait_step(WAIT_STEP);
    spin_reads = 0;
    return snes_clock - from;
}

static int vblank_begun(void *ctx) { return vblanks != *(uint32_t *)ctx; }

void nshvc_wait_vblank(void)
{
    uint32_t v = vblanks;
    nshvc_wait_until(vblank_begun, &v);
}

/* WAI: until an interrupt is due (an NMI, or the IRQ line, masked or not),
   then on; it is taken there when it is not masked */
void snes_wai(void)
{
    wait_allowed("WAI");
    for (;;) {
        if (snes_interrupt_due()) {
            between();
            return;
        }
        if (wait_step(WAIT_STEP))
            return;
    }
}

/* a branch to itself: time passes, the interrupts run, for ever */
void snes_loop_forever(void)
{
    wait_allowed("a loop for ever");
    for (;;)
        wait_step(WAIT_STEP);
}

static int timeup_seen(void) { return timeup_flag && timeup_at <= snes_clock; }
int snes_interrupt_due(void)
{
    if (chip_watch(0))
        chip_sync();
    return (nmi_pending && nmi_at <= snes_clock) || timeup_seen() || (cart_irq && cart_irq_at <= snes_clock);
}

/* ---- power ---------------------------------------------------------------------- */
void snes_power(void)
{
    uint32_t n;
    memset(wram, 0, sizeof wram);
    wram_port = 0;
    free(sram);
    sram = NULL;
    sram_mask = 0;
    chip = snes_cart.chip;
    /* the cartridge's RAM, but where its chip owns it */
    if (!(chip && chip->ram) && snes_cart.sram_size) {
        for (n = 1; n < snes_cart.sram_size; n <<= 1)
            ;
        sram = (uint8_t *)malloc(n);
        if (!sram)
            snes_fatal("no memory for the cartridge's RAM");
        /* as a cartridge with no save yet powers on: all ones */
        memset(sram, 0xFF, n);
        sram_mask = n - 1;
    }
    /* the CPU starts 182 clocks into line 0 (measured against the
       reference: tests/rom/cases.py's power_on, beam_walk) */
    snes_clock = SNES_POWER_CLOCK;
    nmitimen = 0;
    wrio = 0xFF;
    wrmpya = 0xFF;
    wrmpyb = wrdivb = memsel = 0;
    wrdiv = 0xFFFF;
    rddiv = rdmpy = 0;
    mpy_left = div_left = 0;
    alu_shift = 0;
    htime = vtime = 0x1FF;
    rdnmi_flag = timeup_flag = 0;
    nmi_pending = cart_irq = 0;
    memset(joy_reg, 0, sizeof joy_reg);
    joy_counter = 33;
    joy_cpu_latch = joy_auto_latch = 0;
    joy_bits[0] = joy_bits[1] = 0;
    joy_next = NEVER;
    ports_reset(snes_cart.ports);
    lines = snes_cart.region == NSHVC_PAL ? SNES_LINES_PAL : SNES_LINES_NTSC;
    vblank_line = 225;
    vline = 0;
    line_start = 0;
    line_len = SNES_LINE_CLOCKS;
    frame_interlace = 0;
    frame_len = lines;
    nmi_valid = nmi_rose = 0;
    line_done_edge = line_done_nmi_flag = 0;
    frames = vblanks = 0;
    interrupts_held = 0;
    spin_reads = 0;
    line_done_hblank = line_done_refresh = 0;
    irq_at = NEVER;
    ppu_reset();
    dma_reset();
    if (chip && chip->power)
        chip->power(&snes_cart);
    apu_reset();
    events_next();
}

/* ---- the cartridge connector's side (cart.h) ---- */
uint64_t nshvc_cart_clock(void) { return chip_ahead ? chip_to : snes_clock; }
void nshvc_cart_irq_at(int level, uint64_t clock)
{
    snes_cart_irq(level, clock);
    /* run ahead: the chip stops where its IRQ rose */
    if (level && chip_ahead && clock < chip_to)
        chip_to = clock > snes_clock ? clock : snes_clock;
}
void nshvc_cart_irq(int level) { nshvc_cart_irq_at(level, nshvc_cart_clock()); }
uint8_t nshvc_cart_rom(uint32_t off, int who) { return snes_rom_byte(snes_rom_mirror(off), who); }
uint8_t nshvc_open_bus(void) { return snes_mdr; }
nshvc_coro *nshvc_coro_new(void (*fn)(void *), void *arg, size_t stack) { return (nshvc_coro *)coro_new(fn, arg, stack); }
nshvc_coro *nshvc_coro_current(void) { return (nshvc_coro *)coro_current(); }
void nshvc_coro_switch(nshvc_coro *from, nshvc_coro *to) { coro_switch((Coro *)from, (Coro *)to); }
