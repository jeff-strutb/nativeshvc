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
/* The multiplier and divider work a step at each edge of the CPU's cycles
   (alu_edge: a read's after its access, a write's before it, an internal
   cycle's at its end; five in a refresh, none in DMA): 8 steps for a
   multiply, 16 for a divide, RDDIV and RDMPY changing as they go */
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
static uint16_t htime = 0x1FF, vtime = 0x1FF;
static uint8_t rdnmi_flag, timeup_flag;
/* When each interrupt became due, and when the CPU's latest cycle began
   with the I flag as it stood then: in an instruction the console runs
   (WAI, a branch to itself, a unit call's stub), the CPU decides on an
   interrupt at the start of its last cycle, so one due by then, unmasked
   then, is taken at its end (measured against the reference:
   tests/rom/cases.py irq_fixed, at every dot) */
static uint64_t nmi_at, timeup_at, poll_clock;
static int poll_masked;
static int nmi_pending, cart_irq;

static void latch_counters(void);
static void joy_run(void);
static void cpu_clocks(uint32_t clocks);
static unsigned stretch_first(uint32_t clocks);
static void alu_stretch(uint32_t clocks);
static void *audit_at;   /* where the last audited access came from (NSHVC_AUDIT) */

static int audit_on(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("NSHVC_AUDIT") != NULL;
    return on;
}

/* NSHVC_CYCLOG=FILE: the S-CPU's time and accesses as the C makes them, a
   line each, streamed (NSHVC_CYCLOG_FRAMES=A-B: frames A to B only, the
   frame counted from 1 as it runs; a script counts its own frames, as the
   reference's harness does, and writes nothing while a load replays):
     # base 0xADDR PATH      the program's load address (sites are absolute)
     F n                     frame n begins
     C 0xSITE n              cyc(n) from the C at SITE (cyc_then, cyc_loop too)
     S 0xSITE 0xLENGTHS      cycs(LENGTHS) from SITE
     K n                     time the console states for the C: an opcode
                             fetch it makes, cpu_implied, WAI, an interrupt's
     D n                     cpu_idle's wait, n clocks
     M n                     nshvc_mark(n): a place the C names, in stream order
     R aaaaaa vv / W aaaaaa vv / P aaaaaa vv   a read, a write, an interrupt's push */
static FILE *cyclog;
static unsigned cyclog_from, cyclog_to = ~0u;
int snes_cyclog_quiet;
uint32_t (*snes_cyclog_frame)(void);

static int cyclog_on(void)
{
    static int init;
    if (!init) {
        const char *p = getenv("NSHVC_CYCLOG"), *r = getenv("NSHVC_CYCLOG_FRAMES");
        init = 1;
        if (p && !(cyclog = fopen(p, "w")))
            snes_fatal("NSHVC_CYCLOG: cannot write %s", p);
        if (r && sscanf(r, "%u-%u", &cyclog_from, &cyclog_to) < 1)
            snes_fatal("NSHVC_CYCLOG_FRAMES: A-B");
        if (cyclog) {
            setvbuf(cyclog, NULL, _IOFBF, 1 << 20);
#ifndef _WIN32
            {
                Dl_info di;
                if (dladdr((void *)cyclog_on, &di))
                    fprintf(cyclog, "# base %p %s\n", di.dli_fbase, di.dli_fname);
            }
#endif
        }
    }
    if (!cyclog || snes_cyclog_quiet)
        return 0;
    {
        static unsigned last;
        unsigned f = snes_cyclog_frame ? snes_cyclog_frame() : ppu_frames() + 1;
        if (f < cyclog_from || f > cyclog_to)
            return 0;
        if (f != last) {
            last = f;
            fprintf(cyclog, "F %u\n", f);
        }
        return 1;
    }
}

void snes_cyclog_mark(uint32_t id)
{
    if (cyclog_on())
        fprintf(cyclog, "M %u\n", (unsigned)id);
}

void snes_cyclog_cycs(void *site, uint64_t lengths)
{
    if (cyclog_on())
        fprintf(cyclog, "S %p 0x%llx\n", site, (unsigned long long)lengths);
}

/* the C's place audit_at names, once each: 0 when it was named before */
static int audit_where(char *buf, size_t n)
{
    enum { SEEN = 1 << 16 };
    static void *seen[SEEN];        /* an open-addressed set of the places named */
    size_t h = ((uintptr_t)audit_at >> 2) * 2654435761u % SEEN, k;
    for (k = 0; k < SEEN && seen[h]; k++, h = (h + 1) % SEEN)
        if (seen[h] == audit_at)
            return 0;
    if (k == SEEN)
        return 0;
    seen[h] = audit_at;
#ifndef _WIN32
    {
        Dl_info di;
        if (dladdr(audit_at, &di) && di.dli_sname) {
            snprintf(buf, n, "by %s+%ld", di.dli_sname, (long)((char *)audit_at - (char *)di.dli_saddr));
            return 1;
        }
    }
#endif
    snprintf(buf, n, "at %p", audit_at);
    return 1;
}

static void audit_site(uint32_t addr, int w)
{
    char where[256];
    if (audit_where(where, sizeof where))
        fprintf(stderr, "audit: $%06X %s %s\n", (unsigned)addr, w ? "written" : "read", where);
}

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
int snes_untimed_on, snes_in_interrupt;
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
uint8_t *snes_sram(void) { return chip && chip->ram ? chip->ram(NULL) : sram; }
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
            {
                int k;
                for (k = 0; k < 5; k++)
                    alu_edge();
            }
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

/* untimed as it stands: set, and outside an interrupt handler or with
   interrupts held (an app's code run from the frame hook, which may come
   while a handler is under way) */
int snes_untimed(void)
{
    return snes_untimed_on && (!snes_in_interrupt || interrupts_held);
}

void snes_tick(unsigned clocks)
{
    if (snes_untimed())
        return;
    snes_clock += clocks;
    if (snes_clock >= next_event)
        run_events();
}

void snes_cart_irq(int level) { cart_irq = level; }

/* nshvc_hold_interrupts: due interrupts wait while held */
void nshvc_hold_interrupts(int on)
{
    on = on != 0;
    /* the chip keeps what it holds of its interrupt (cart.h) */
    if (chip && chip->hold && on != interrupts_held)
        chip->hold(on);
    interrupts_held = on;
}

/* the interrupts due by clock `by` (masked as `masked` says) */
static void check_interrupts_at(uint64_t by, int masked)
{
    if (interrupts_held)
        return;
    if (nmi_pending && nmi_at <= by) {
        nmi_pending = 0;
        dbg_nmis++;
        if (snes_interrupt_hook)
            snes_interrupt_hook(1);
        return;
    }
    if (((timeup_flag && timeup_at <= by) || cart_irq) && snes_interrupt_hook && !masked) {
        dbg_irqs++;
        snes_interrupt_hook(0);
    }
}

/* a cycle of the CPU's begins, `cc` clocks long: where the CPU decides on
   an interrupt (snes_cpu_boundary) and where the DMA unit takes the bus */
static void cycle_begins(unsigned cc)
{
    poll_clock = snes_clock;
    poll_masked = snes_irq_masked && snes_irq_masked();
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
        /* a start while one is under way is lost (but the result's clear) */
        rdmpy = 0;
        if (mpy_left || div_left)
            return;
        wrmpyb = v;
        rddiv = (uint16_t)(wrmpyb << 8 | wrmpya);
        mpy_left = 8;
        alu_shift = wrmpyb;
        return;
    case 0x4204: wrdiv = (uint16_t)((wrdiv & 0xFF00) | v); return;
    case 0x4205: wrdiv = (uint16_t)((wrdiv & 0x00FF) | v << 8); return;
    case 0x4206:
        rdmpy = wrdiv;
        if (mpy_left || div_left)
            return;
        wrdivb = v;
        div_left = 16;
        alu_shift = (uint32_t)wrdivb << 16;
        return;
    case 0x4207: htime = (uint16_t)((htime & 0x100) | v); irq_schedule(1); events_next(); return;
    case 0x4208: htime = (uint16_t)((htime & 0xFF) | (v & 1) << 8); irq_schedule(1); events_next(); return;
    case 0x4209: vtime = (uint16_t)((vtime & 0x100) | v); irq_schedule(1); events_next(); return;
    case 0x420A: vtime = (uint16_t)((vtime & 0xFF) | (v & 1) << 8); irq_schedule(1); events_next(); return;
    case 0x420B:
        /* the channels' transfers, made at the start of the CPU's next cycle
           but one */
        dma_mdmaen = v;
        if (v)
            dma_pending = 1;
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
    case NSHVC_SUPERFX:
        if (b < 0x40) {
            if (off >= 0x8000) {
                *at = snes_rom_mirror((uint32_t)(b & 0x3F) << 15 | (off & 0x7FFF));
                return CART_ROM;
            }
            return CART_NONE;
        }
        if (b < 0x60) {
            *at = snes_rom_mirror((uint32_t)(b & 0x1F) << 16 | off);
            return CART_ROM;
        }
        /* its registers and RAM: the chip's (nshvc_cart.chip) */
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
    addr &= 0xFFFFFF;
    if (snes_untimed())
        untimed_read(addr);
    dbg_last_read = addr;
    /* the read sees the machine 4 clocks before its cycle ends */
    if (rom_data(addr)) {
        v = bus_read(addr, 0);
    } else {
        unsigned cc = snes_access_clocks(addr);
        cycle_begins(cc);
        snes_tick(cc - 4);
        v = bus_read(addr, 0);
        snes_tick(4);
    }
    alu_edge();
    snes_mdr = v;
    if (cyclog_on())
        fprintf(cyclog, "R %06X %02X\n", (unsigned)addr, v);
    return v;
}

/* ---- the C's operations, and the interrupts between them ----
   The CPU takes an interrupt between two of its operations, never inside
   one. The C's operations are what it states: an access (rd8, wr8), a
   word or long (rd16, wr16, rd24), a read-modify-write (rd8_rmw or
   rd16_rmw to its write), a stretch of time (cyc, cycs, cpu_idle). An
   interrupt is taken at the first point between them at or after the
   time it comes due, the IRQ unless the I flag masks it: at an
   operation's end, or inside a stretch of time at the clock it comes due.
   No instruction's end is known or inferred: the original takes the
   interrupt at its instruction's end, later than here by the rest of that
   instruction (README.md, Time). */
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
        if (cart_irq)
            return 0;
        if (timeup_flag && timeup_at < t)
            t = timeup_at;
    }
    return t;
}

/* a point between two of the C's operations: the interrupt due by now is
   taken there (and a unit call the script waits on starts) */
static void between(void)
{
    if (rmw_open)
        return;
    if (snes_unit_pending)
        snes_unit_start();
    check_interrupts_at(snes_clock, snes_irq_masked && snes_irq_masked());
}

void snes_c_between(void) { between(); }

/* `clocks` of the C's time with no access in it, as the clock moves on:
   each interrupt that comes due in it is taken at its time (the multiplier
   brought up to that time first), the rest of the time after it */
static void c_time(uint32_t clocks, uint32_t *alu_pending)
{
    while (clocks) {
        uint32_t n = clocks;
        uint64_t due = interrupt_due_at();
        if (!rmw_open && !snes_untimed()) {
            if (due > snes_clock && due - snes_clock < n)
                n = (uint32_t)(due - snes_clock);
            /* an event in it may raise one */
            if (next_event > snes_clock && next_event - snes_clock < n)
                n = (uint32_t)(next_event - snes_clock);
        }
        snes_tick(n);
        clocks -= n;
        *alu_pending += n;
        if (!rmw_open && interrupt_due_at() <= snes_clock) {
            alu_stretch(*alu_pending);
            *alu_pending = 0;
            between();
        }
    }
}

static uint8_t c_rd(uint32_t addr, void *site)
{
    if (rmw_open)
        snes_fatal("rd8_rmw at $%06X: a read of $%06X before its write", (unsigned)rmw_at, (unsigned)(addr & 0xFFFFFF));
    if (audit_on() && bus_a_is_register(addr & 0xFFFFFF)) {
        audit_at = site;
        audit_site(addr & 0xFFFFFF, 0);
    }
    return access_rd(addr);
}

uint8_t rd8(uint32_t addr)
{
    uint8_t v = c_rd(addr, __builtin_return_address(0));
    between();
    return v;
}

uint16_t rd16(uint32_t addr)
{
    void *site = __builtin_return_address(0);
    uint8_t lo = c_rd(addr, site);
    uint16_t v = (uint16_t)(lo | c_rd((addr + 1) & 0xFFFFFF, site) << 8);
    between();
    return v;
}

uint32_t rd24(uint32_t addr)
{
    void *site = __builtin_return_address(0);
    uint32_t v = c_rd(addr, site);
    v |= (uint32_t)c_rd((addr + 1) & 0xFFFFFF, site) << 8;
    v |= (uint32_t)c_rd((addr + 2) & 0xFFFFFF, site) << 16;
    between();
    return v;
}

uint8_t rd8_rmw(uint32_t addr)
{
    uint8_t v = c_rd(addr, __builtin_return_address(0));
    rmw_open = 1;
    rmw_at = addr & 0xFFFFFF;
    return v;
}

uint16_t rd16_rmw(uint32_t addr)
{
    void *site = __builtin_return_address(0);
    uint8_t lo = c_rd(addr, site);
    uint16_t v = (uint16_t)(lo | c_rd((addr + 1) & 0xFFFFFF, site) << 8);
    rmw_open = 1;
    rmw_at = addr & 0xFFFFFF;
    return v;
}

/* a write: `push`, one of the interrupt sequence's own (NSHVC_CYCLOG's P
   line) */
static void access_wr(uint32_t addr, uint8_t value, int push)
{
    addr &= 0xFFFFFF;
    untimed_reads = 0;
    alu_edge();
    cycle_begins(snes_access_clocks(addr));
    snes_tick(snes_access_clocks(addr));
    snes_mdr = value;
    if (cyclog_on())
        fprintf(cyclog, "%c %06X %02X\n", push ? 'P' : 'W', (unsigned)addr, value);
    if (snes_write_hook)
        snes_write_hook(addr, value);
    bus_write(addr, value, 0);
}

static void c_wr(uint32_t addr, uint8_t value, void *site)
{
    if (audit_on() && bus_a_is_register(addr & 0xFFFFFF)) {
        audit_at = site;
        audit_site(addr & 0xFFFFFF, 1);
    }
    access_wr(addr, value, 0);
}

/* a write ends the read-modify-write whose read came before it */
void wr8(uint32_t addr, uint8_t value)
{
    c_wr(addr, value, __builtin_return_address(0));
    rmw_open = 0;
    between();
}

void wr16(uint32_t addr, uint16_t value)
{
    void *site = __builtin_return_address(0);
    c_wr(addr, (uint8_t)value, site);
    c_wr((addr + 1) & 0xFFFFFF, (uint8_t)(value >> 8), site);
    rmw_open = 0;
    between();
}

void wr16_rmw(uint32_t addr, uint16_t value)
{
    void *site = __builtin_return_address(0);
    c_wr((addr + 1) & 0xFFFFFF, (uint8_t)(value >> 8), site);
    c_wr(addr, (uint8_t)value, site);
    rmw_open = 0;
    between();
}

void snes_interrupt_push(uint32_t addr, uint8_t value) { access_wr(addr, value, 1); }
uint8_t snes_cpu_read_within(uint32_t addr) { return access_rd(addr); }
void snes_cpu_write_within(uint32_t addr, uint8_t value) { access_wr(addr, value, 0); }

/* A stretch with no access (a delay loop): the clock moves on in short
   steps, each a cycle's start where the DMA unit may take the bus. The
   console's own wait (nshvc_wait_vblank) takes no interrupt in it; the C's
   (cpu_idle, `c`) takes each as it comes due */
static void cpu_wait(uint32_t clocks, int c)
{
    if (cyclog_on())
        fprintf(cyclog, "D %u\n", (unsigned)clocks);
    while (clocks) {
        unsigned n = clocks > 36 ? 36 : (unsigned)clocks;
        uint32_t alu = 0;
        cycle_begins(6);
        if (c)
            c_time(n, &alu);
        else
            snes_tick(n), alu = n;
        alu_stretch(alu);
        clocks -= n;
    }
    if (c)
        between();
}

void snes_cpu_wait(uint32_t clocks) { cpu_wait(clocks, 0); }
void snes_cpu_idle(uint32_t clocks) { cpu_wait(clocks, 1); }
/* Time the C states (machine.h cyc) from `site`. A DMA the last write
   started begins at the second cycle after it, an HDMA at the first cycle
   after its time, which a stretch of more than one cycle hides:
   NSHVC_AUDIT names each place one does (machine.h cycs states it). */
void snes_cpu_stated(uint32_t clocks, void *site)
{
    if (audit_on() && clocks != 6 && clocks != 8 && clocks != 12) {
        const char *what = NULL;
        uint64_t end = snes_clock + clocks;
        if (!dma_active && (dma_pending || hdma_pending))
            what = dma_pending ? "a DMA starts" : "an HDMA comes";
        else if (!line_done_hdma && vline < snes_vdisp() && hdma_active() && line_start + HDMA_AT > snes_clock &&
                 line_start + HDMA_AT < end)
            what = "an HDMA comes";
        else if (!line_done_setup && hdma_enabled() && setup_at() > snes_clock && setup_at() < end)
            what = "the HDMA setup comes";
        if (what) {
            char where[256];
            audit_at = site;
            if (audit_where(where, sizeof where))
                fprintf(stderr, "audit: %s inside cyc(%u) %s: state its cycles (cycs)\n", what, (unsigned)clocks, where);
        }
    }
    if (cyclog_on())
        fprintf(cyclog, "C %p %u\n", site, (unsigned)clocks);
    {
        uint32_t alu = 0;
        if (clocks)
            cycle_begins(stretch_first(clocks));
        c_time(clocks, &alu);
        alu_stretch(alu);
    }
    between();
}

/* one cycle of the S-CPU's, `cc` clocks (machine.h cycs) */
void snes_cpu_cycle(unsigned cc)
{
    cycle_begins(cc);
    snes_tick(cc);
    alu_edge();
}

/* one of the C's cycles (cycs): an interrupt that comes due in it is taken
   at its end */
void snes_cpu_cycle_c(unsigned cc)
{
    snes_cpu_cycle(cc);
    between();
}

void snes_cpu_clocks(uint32_t clocks)
{
    if (cyclog_on())
        fprintf(cyclog, "K %u\n", (unsigned)clocks);
    cpu_clocks(clocks);
}

/* a cycle the console states for the C (machine.h's extensions, an
   interrupt's sequence): NSHVC_CYCLOG's K line */
void snes_cpu_cycle_k(unsigned cc)
{
    if (cyclog_on())
        fprintf(cyclog, "K %u\n", cc);
    snes_cpu_cycle(cc);
}

/* the multiplier's steps in a stretch whose cycles are not stated (cyc):
   one for each 7 clocks, the mean of a fetch's 8 and an internal cycle's
   6 (cycs states them exactly) */
static void alu_stretch(uint32_t clocks)
{
    unsigned k;
    if (!mpy_left && !div_left)
        return;
    for (k = (clocks + 3) / 7; k && (mpy_left || div_left); k--)
        alu_edge();
}

/* a stretch's first cycle: an internal cycle when it is internal cycles
   only, else a SlowROM fetch */
static unsigned stretch_first(uint32_t clocks) { return clocks % 6 == 0 && clocks % 8 != 0 ? 6 : 8; }

static void cpu_clocks(uint32_t clocks)
{
    if (clocks)
        cycle_begins(stretch_first(clocks));
    snes_tick(clocks);
    alu_stretch(clocks);
}
static int timeup_seen(void) { return timeup_flag && timeup_at <= snes_clock; }
int snes_interrupt_due(void) { return (nmi_pending && nmi_at <= snes_clock) || timeup_seen() || cart_irq; }
int snes_interrupt_taken_now(void)
{
    return !interrupts_held
        && ((nmi_pending && nmi_at <= snes_clock) || ((timeup_seen() || cart_irq) && !(snes_irq_masked && snes_irq_masked())));
}
/* the end of an instruction the console runs: the interrupt the CPU
   decided on as its last cycle began, with the I flag as it was then */
void snes_cpu_boundary(void)
{
    if (snes_unit_pending)
        snes_unit_start();
    check_interrupts_at(poll_clock, poll_masked);
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
    snes_untimed_on = snes_in_interrupt = 0;
    interrupts_held = 0;
    line_done_hblank = line_done_refresh = 0;
    irq_at = NEVER;
    ppu_reset();
    dma_reset();
    if (chip && chip->power)
        chip->power(&snes_cart);
    apu_reset();
    events_next();
}

/* ---- untimed (nshvc.h) ---------------------------------------------------------- */
void nshvc_set_untimed(int on)
{
    snes_untimed_on = on != 0;
    untimed_reads = 0;
}
int nshvc_untimed(void) { return snes_untimed(); }

void nshvc_wait_vblank(void)
{
    int was = snes_untimed_on;
    uint32_t v = vblanks;
    snes_untimed_on = 0;
    /* the console's own wait, an instruction's end after each step */
    while (vblanks == v) {
        snes_cpu_wait(64);
        snes_cpu_boundary();
    }
    snes_untimed_on = was;
}

/* ---- the cartridge connector's side (cart.h) ---- */
uint64_t nshvc_cart_clock(void) { return snes_clock; }
void nshvc_cart_irq(int level) { snes_cart_irq(level); }
uint8_t nshvc_cart_rom(uint32_t off, int who) { return snes_rom_byte(snes_rom_mirror(off), who); }
uint8_t nshvc_open_bus(void) { return snes_mdr; }
nshvc_coro *nshvc_coro_new(void (*fn)(void *), void *arg, size_t stack) { return (nshvc_coro *)coro_new(fn, arg, stack); }
nshvc_coro *nshvc_coro_current(void) { return (nshvc_coro *)coro_current(); }
void nshvc_coro_switch(nshvc_coro *from, nshvc_coro *to) { coro_switch((Coro *)from, (Coro *)to); }
