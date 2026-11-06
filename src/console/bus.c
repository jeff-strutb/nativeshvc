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

uint8_t rd8(uint32_t addr);
void wr8(uint32_t addr, uint8_t value);

nshvc_cart snes_cart;
uint64_t snes_clock;
nshvc_bus_state nshvc_bus;

void (*snes_interrupt_hook)(int nmi);
int (*snes_irq_masked)(void);
void (*snes_frame_hook)(void);

static void bus_settle(void);
static void bus_enter(void);
static void bus_leave(void);
void (*snes_write_hook)(uint32_t addr, uint8_t value);
void (*snes_read_hook)(uint32_t addr, uint8_t value);
void (*snes_register_write_hook)(uint32_t addr, uint8_t value);

static uint8_t wram[0x20000];
static uint32_t wram_port;
static uint8_t *sram;
static uint32_t sram_mask;
/* A wait's condition as it runs (nshvc_wait_until): on, every access
   a call (bus_leave), each read noted (turn_note) with when it could next
   answer otherwise; the earliest (until), and whether one cannot tell
   (step) or HDMA's work could change one (dma: its registers, mem:
   memory). The clock looked at directly (snes_vcounter and the rest, a
   chip's nshvc_cart_clock) counts as a read. */
typedef struct {
    int on, step, port, dma, mem;
    uint64_t until;
} Turn;
static Turn turn;
static int read_by_chip;              /* the last read answered by the cartridge's chip */

/* the CPU's registers */
static uint8_t nmitimen, wrio = 0xFF, wrmpya = 0xFF, wrmpyb, wrdivb, memsel;
static uint16_t wrdiv = 0xFFFF, rddiv, rdmpy;
/* The multiplier and divider: 8 steps for a multiply, 16 for a divide,
   RDDIV and RDMPY changing as they go. The S-CPU's C takes no time, so its
   next read comes after the last step: the results are written at once,
   the product in RDMPY and the multiplier in RDDIV (the last of its
   shifts), the quotient and remainder, or for a divide by 0 $FFFF and the
   dividend. */

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

#define NEVER UINT64_MAX
#define NO_LINE 0xFFFFu
#define HBLANK_AT 1096                    /* dot 274 */
/* the work RAM's refresh: the S-CPU stopped for 40 clocks once a line,
   near dot 133 (as the DMA unit's clock stands as the line begins) */
#define REFRESH_AT 534
#define REFRESH_CLOCKS 40
/* HDMA's line transfers (lines 0-224) and, in line 0, the frame's setup:
   due here, made at the start of the CPU's next cycle */
#define HDMA_AT 1100
#define HDMA_SETUP_AT 8

/* ---- the beam, worked out from the clock ----
   Nothing steps through a frame's lines. A frame starts at frame_start;
   its lines are 1,364 clocks each but one, odd_line: NTSC's line 240 in
   field 1 when not interlaced (1,360, its dots all 4 clocks) and PAL's
   line 311 in field 1 when interlaced (1,368, a dot 340 after 339). It has
   frame_len lines: 262 (PAL 312), one more in an interlaced frame's field
   0. Its field and interlace (as SETINI stood when it began) are known as
   it begins, so a line's start is a formula of its number (line_at) and
   the line a clock falls in one of the clock (line_of). What the hardware
   does at a point of the frame happens there only when it does something
   the S-CPU can see or the game asked for: the console's deadlines
   (events_next) are the IRQ, the NMI's flag, the vertical blank's start
   and the frame's, and HDMA while it runs. Everything else (the counters,
   HVBJOY, the refresh, the lines' records for the picture) is worked out
   from the clock where it is looked at. */
static unsigned lines = SNES_LINES_NTSC, vblank_line = 225;
static uint64_t frame_start;
static unsigned frame_len = SNES_LINES_NTSC, odd_line = NO_LINE;
static int odd_delta, frame_interlace;
static uint32_t frames, vblanks;

static uint64_t line_at(unsigned l)
{
    return frame_start + (uint64_t)SNES_LINE_CLOCKS * l + (l > odd_line ? (uint64_t)(int64_t)odd_delta : 0);
}

static unsigned line_len_of(unsigned l) { return (unsigned)(SNES_LINE_CLOCKS + (l == odd_line ? odd_delta : 0)); }

/* the line clock c falls in (past the frame's last, the next frame's as
   this frame's lines go on: the next frame's first lines are all 1,364).
   (c is never more than a frame and a little past frame_start: 32 bits) */
static unsigned line_of(uint64_t c)
{
    uint32_t o = (uint32_t)(c - frame_start);
    if (odd_line != NO_LINE) {
        uint32_t b = (uint32_t)(SNES_LINE_CLOCKS * (odd_line + 1) + odd_delta);
        if (o >= b)
            return odd_line + 1 + (o - b) / SNES_LINE_CLOCKS;
    }
    return o / SNES_LINE_CLOCKS;
}

static uint64_t frame_end(void) { return line_at(frame_len); }

/* the frame's shape, from its field and interlace */
static void frame_shape(void)
{
    int f = ppu_field();
    odd_line = NO_LINE;
    odd_delta = 0;
    if (!snes_pal() && !frame_interlace && f) {
        odd_line = 240;
        odd_delta = -4;
    } else if (snes_pal() && frame_interlace && f) {
        odd_line = 311;
        odd_delta = 4;
    }
    frame_len = lines + (frame_interlace && !f);
}

/* the NMI's condition (the counters' line at or past the vertical blank's)
   as last tested at a line's edge, 2 clocks before the line starts:
   nmi_from the first line whose edge is still to come, nmi_edge the next
   at which the condition changes (NO_LINE: none this frame; frame_len: the
   frame's end, the line 0 of the next); its rise, for the line rose_line's
   start */
static int nmi_valid, nmi_rose;
static unsigned nmi_from = 1, nmi_edge = NO_LINE, rose_line = NO_LINE;
/* the first line whose start, HDMA point and horizontal blank (its record
   for the picture) are still to come; the frame's last edge (its field
   flipped, 4 clocks before its end) and line 0's HDMA setup, done */
static unsigned begin_from = 1, hdma_from, rendered = 1;
static int edge_done, setup_done;
static uint64_t irq_at, next_event;
static uint64_t next_real;           /* next_event but for HDMA's lines (hdma_bulk) */
/* the next refresh not yet made */
static uint64_t refresh_next;
/* counted up where the console changes what a wait's reads could see
   (wait_until looks again): the interrupts, the NMI's flag, the vertical
   blank's and the frame's start; HDMA's lines (what the DMA registers
   show) and its writes to the A bus (memory) on their own */
static unsigned look_again, look_dma, look_mem;

/* The DMA unit's clock: the master clock in steps of 8, as the unit sees
   it (its phase against snes_clock measured: irq_entry, frame_clock) */
#define DMA_PHASE 4
static unsigned dma_counter(uint64_t clock) { return (unsigned)((clock + DMA_PHASE) & 7); }
/* a line's refresh and the frame's HDMA setup, by the unit's clock as the
   line begins (4 clocks before its start by the counters) */
static uint64_t refresh_of(unsigned l)
{
    uint64_t ls = line_at(l);
    return ls + REFRESH_AT - dma_counter(ls - 4);
}
static uint64_t setup_time(void) { return frame_start + HDMA_SETUP_AT + dma_counter(frame_start - 4); }

/* The DMA unit's work waiting (MDMAEN's transfers; HDMA's setup, mode 0,
   or a line, mode 1) and whether it is taking the bus: work that arrives
   is taken up at the start of the CPU's next cycle and made at the start
   of the one after (unit_edge); HDMA's, if the CPU makes no cycle first,
   at the end of the line it came due in (hdma_line_of) */
static int dma_pending, hdma_pending, hdma_mode, dma_active;
static unsigned hdma_line_of = NO_LINE;
static unsigned unit_cc;        /* the CPU's cycle the unit's work comes in, its clocks */

static unsigned dbg_irqs, dbg_nmis;          /* for NSHVC_DEBUG */
static unsigned interrupts_taken;            /* every one taken: a wait's look again */
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
unsigned snes_vcounter(void)
{
    unsigned l;
    bus_settle();
    l = line_of(snes_clock);
    if (turn.on && line_at(l + 1) < turn.until)
        turn.until = line_at(l + 1);
    return l;
}
/* A line's 340 dots are 4 master clocks each but dots 323 and 327, which
   are 6 (1,364 clocks a line); a short line's are all 4. */
static unsigned dot_of(uint64_t c, unsigned len)
{
    if (c < 323 * 4 || len < SNES_LINE_CLOCKS)
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
   clock (into the next line in this line's last 4); *len the length of
   the line they are in */
static void counters(unsigned *v, uint64_t *h, unsigned *len)
{
    unsigned l = line_of(snes_clock);
    *v = l;
    *len = line_len_of(l);
    *h = snes_clock - line_at(l) + 4;
    if (*h >= *len) {
        *h -= *len;
        *v = l + 1 >= frame_len ? 0 : l + 1;
        *len = l + 1 >= frame_len ? SNES_LINE_CLOCKS : line_len_of(l + 1);
    }
}

unsigned snes_hcounter(void)
{
    unsigned l;
    bus_settle();
    turn.step |= turn.on;
    l = line_of(snes_clock);
    return dot_of(snes_clock - line_at(l), line_len_of(l));
}

uint32_t snes_line_clock(void)
{
    bus_settle();
    turn.step |= turn.on;
    return (uint32_t)(snes_clock - line_at(line_of(snes_clock)));
}
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

static void direct_ask(uint32_t addr);
static int chip_direct;                  /* the chip gives direct answers (cart.h) */

/* the clock the chip was last brought up to (cart.h: a sync to a clock it
   has reached does nothing, so none is made) */
static uint64_t chip_at = UINT64_MAX;

static void chip_sync(void)
{
    if (chip && chip->sync && (chip_at == UINT64_MAX || snes_clock > chip_at)) {
        chip->sync();
        chip_at = snes_clock;
    }
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

/* Line l's own IRQ time by the registers as they stand (NEVER: none). */
static uint64_t irq_own(unsigned l)
{
    unsigned mode = (nmitimen >> 4) & 3;
    uint64_t ls;
    if (l >= frame_len)
        return NEVER;
    /* none at dot 339 of the frame's last line: its compare falls on the
       counters' first dot of the next field */
    if (htime == 339 && (mode == 1 || mode == 3) && l + 1 == frame_len)
        return NEVER;
    ls = line_at(l);
    if (mode == 1 && htime < 340)
        return ls + 4u * htime + IRQ_DELAY;
    if (mode == 2 && l == vtime)
        return ls + 10;
    if (mode == 3 && l == vtime && htime < 340)
        return ls + 4u * htime + IRQ_DELAY;
    return NEVER;
}

/* The IRQ's next time from line l's start on. As each line starts, its
   own time is due, or, earlier, `carried`: a time the line before left
   due past its end (H 339, or 338 on a short line, and the IRQ's own
   delay), which takes the line's place. A line's time comes in the line
   when it is due by the line's end and the 4 clocks the line rises ahead
   of the CPU (timeup); a later one is carried. Within this frame (the
   frame's last line carries none); NEVER: none in it. */
static uint64_t irq_find(unsigned l, uint64_t carried)
{
    unsigned mode = (nmitimen >> 4) & 3;
    for (;;) {
        uint64_t own, t;
        if (l >= frame_len)
            return NEVER;
        if (carried == NEVER && (mode == 2 || mode == 3) && l < vtime) {
            if (vtime >= frame_len)
                return NEVER;
            l = vtime;
        }
        own = irq_own(l);
        t = carried < own ? carried : own;
        if (t == NEVER)
            return NEVER;
        if (t <= line_at(l) + line_len_of(l) + 4)
            return t;
        carried = t;
        l++;
    }
}

/* After a write to the IRQ's registers: this line's time counts only if
   it is still to come; the lines after as they start. */
static void irq_rewrite(void)
{
    unsigned l = line_of(snes_clock);
    uint64_t own = irq_own(l);
    if (own != NEVER && own < snes_clock)
        own = NEVER;
    if (own != NEVER && own <= line_at(l) + line_len_of(l) + 4)
        irq_at = own;
    else
        irq_at = irq_find(l + 1, own);
}

/* the next edge at which the NMI's condition changes: its fall at a line
   before the vertical blank's (SETINI's overscan set in the blank's first
   lines) or the frame's end, its rise at the blank's line */
static unsigned nmi_next(void)
{
    unsigned n = nmi_from, v = snes_vdisp();
    if (n > frame_len)
        return NO_LINE;
    if (nmi_valid)
        return n < v ? n : frame_len;
    if (n < v)
        n = v;
    return n < frame_len ? n : NO_LINE;
}

/* the next line start with work in it: the vertical blank's line (the
   joypads' read), the NMI's rise (the blank's start), the frame's end */
static uint64_t begin_next(void)
{
    uint64_t t = frame_end();
    unsigned v = snes_vdisp();
    if (v >= begin_from && v < frame_len && line_at(v) < t)
        t = line_at(v);
    if (nmi_rose && line_at(rose_line) < t)
        t = line_at(rose_line);
    return t;
}

int hdma_active(void);
int hdma_enabled(void);
int hdma_writes_a(void);
int hdma_writes_ports(void);

/* the next line's HDMA point while HDMA runs (lines before the vertical
   blank's) */
static uint64_t hdma_next(void)
{
    if (hdma_from < snes_vdisp() && hdma_from < frame_len && hdma_active())
        return line_at(hdma_from) + HDMA_AT;
    return NEVER;
}

static void events_next(void)
{
    uint64_t e = begin_next(), t;
    if (irq_at != NEVER && irq_at - 4 < e)
        e = irq_at - 4;
    if (!edge_done && frame_end() - 4 < e)
        e = frame_end() - 4;
    if (nmi_edge != NO_LINE && line_at(nmi_edge) - 2 < e)
        e = line_at(nmi_edge) - 2;
    if (!setup_done && setup_time() < e)
        e = setup_time();
    if (hdma_pending && hdma_line_of != NO_LINE && line_at(hdma_line_of + 1) < e)
        e = line_at(hdma_line_of + 1);
    next_real = e;
    if ((t = hdma_next()) < e)
        e = t;
    next_event = e;
}

/* After a write that moves the deadlines (SETINI's overscan, HDMAEN): the
   edges and starts already passed are passed. */
static void events_moved(void)
{
    unsigned l = line_of(snes_clock);
    nmi_from = line_of(snes_clock + 2) + 1;
    nmi_edge = nmi_next();
    if (begin_from < l + 1)
        begin_from = l + 1;
    hdma_from = line_at(l) + HDMA_AT > snes_clock ? l : l + 1;
    events_next();
}

/* The lines whose horizontal blank has passed, recorded for the picture
   (ppu_render_lines: their state as the blank came), before anything that
   could change what the PPU shows or reads back, and as the vertical
   blank starts: the record each line made as it was drawn. */
static void ppu_catch_up(void)
{
    unsigned l, end;
    if (rendered >= vblank_line || line_at(rendered) + HBLANK_AT > snes_clock)
        return;
    l = line_of(snes_clock);
    end = line_at(l) + HBLANK_AT <= snes_clock ? l + 1 : l;
    if (end > vblank_line)
        end = vblank_line;
    ppu_render_lines(rendered, end);
    rendered = end;
}

void snes_ppu_catch_up_now(void) { ppu_catch_up(); }
void snes_ppu_catch_up(void)
{
    bus_enter();
    ppu_catch_up();
    bus_leave();
}

/* A new frame: its picture's lines (overscan, SETINI bit 2), its
   interlace, its shape and its deadlines. */
static void frame_begin(void)
{
    frame_start = frame_end();
    frames++;
    vblank_line = ppu_overscan() ? 240 : 225;
    frame_interlace = ppu_interlace();
    ppu_frame_start();
    frame_shape();
    edge_done = setup_done = 0;
    nmi_from = 1;
    begin_from = 1;
    hdma_from = 0;
    rendered = 1;
    nmi_edge = nmi_next();
    irq_at = irq_find(0, NEVER);
    look_again++;
}

static void line_begin(unsigned l)
{
    uint64_t ls = line_at(l);
    begin_from = l + 1;
    if (l < frame_len && l == snes_vdisp()) {
        unsigned h0;
        joy_run();
        /* the light guns see the finished picture; the joypads' read
           starts on its step of 256 in dots 130-384 */
        ports_frame();
        h0 = 130 + ((256 - (unsigned)((ls + 130) & 255)) & 255);
        joy_next = ls - 4 + h0;
        joy_counter = 34;
        look_again++;
    }
    if (nmi_rose && rose_line == l) {
        nmi_rose = 0;
        vblanks++;
        look_again++;
        if (nmitimen & 0x80) {
            nmi_pending = 1;
            nmi_at = ls + NMI_DELAY;
        }
        ppu_catch_up();
        ppu_vblank_start();
        /* the sound unit and the chip brought up to the frame's end: the
           frame's sound made, the chip's work in it done */
        apu_sync();
        chip_sync();
        if (getenv("NSHVC_DEBUG")) {
            char a[128];
            apu_debug(a, sizeof a);
            fprintf(stderr, "frame %u: %s; irqs %u nmis %u nmitimen %02X vtime %u htime %u; last read %06X, "
                    "chip %s, APU in %02X %02X %02X %02X out %02X %02X %02X %02X\n",
                    (unsigned)frames, a, dbg_irqs, dbg_nmis, nmitimen, vtime, htime, (unsigned)dbg_last_read,
                    chip && chip->busy ? (chip->busy() ? "running" : "idle") : "none",
                    apu_debug_port(0), apu_debug_port(1), apu_debug_port(2), apu_debug_port(3),
                    apu_port_read(0), apu_port_read(1), apu_port_read(2), apu_port_read(3));
            dbg_irqs = dbg_nmis = 0;
        }
        /* the frame is done: the front end shows it and sets what the
           players hold, which the joypads' read now takes */
        if (snes_frame_hook) {
            bus_leave();
            snes_frame_hook();
            bus_enter();
        }
    } else if (l == frame_len) {
        frame_begin();
    }
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
                look_dma++;
                events_next();
                if (!dma_mdmaen) {
                    unit_back();
                    dma_active = 0;
                }
            }
        }
        if (dma_pending) {
            dma_pending = 0;
            if (dma_mdmaen && dma_direct_only()) {
                /* only writes to VRAM directly (nshvc_ext_vram_direct): no time */
                dma_run();
                dma_active = 0;
            } else if (dma_mdmaen) {
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
void dma_wrote_a(void) { look_mem++; }

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

/* The refreshes the clock has reached, each stopping the S-CPU for 40
   clocks from the end of the step that reached it: a step of a DMA's, an
   access's, or the S-CPU's own work settled (bus_settle), each short of
   the next deadline, so at most a refresh or two a step but for the
   work settled, whose refreshes are its own lines' (Original mode's
   charged time; none in Enhanced mode, where the C takes none). */
/* The refreshes the clock has passed, each adding its 40 clocks, which can pass
   the next. A line is far longer than 40, so the k-th (from 0) is passed exactly
   when the clock plus k refreshes reaches it, and the count comes at once from
   the clock: from the shortest a refresh can come after the last (the shortest
   line, less the unit's clock's 7, less a refresh), put right by the lines'
   actual lengths in a pass or two. */
static void refresh_take(void)
{
    unsigned l, n;
    if (snes_clock < refresh_next)
        return;
    l = line_of(refresh_next);
    n = (unsigned)((snes_clock - refresh_next) / (SNES_LINE_CLOCKS - 4 - 7 - REFRESH_CLOCKS)) + 1;
    while (n > 1 && snes_clock + (uint64_t)REFRESH_CLOCKS * (n - 1) < refresh_of(l + n - 1))
        n--;
    while (snes_clock + (uint64_t)REFRESH_CLOCKS * n >= refresh_of(l + n))
        n++;
    snes_clock += (uint64_t)REFRESH_CLOCKS * n;
    refresh_next = refresh_of(l + n);
}

/* the refreshes from refresh_next on before clock e (e at most the
   frame's end) */
static uint64_t refreshes_before(uint64_t e)
{
    unsigned a, b;
    if (refresh_next >= e)
        return 0;
    a = line_of(refresh_next);
    b = line_of(e - 1);
    return (uint64_t)(b - a) + (refresh_of(b) < e ? 1 : 0);
}

static void run_events(void)
{
    for (;;) {
        uint64_t c = snes_clock, t;
        /* the IRQ line rises (TIMEUP reads it) 4 clocks before the CPU
           sees it; the next is found from the line after the one it came
           in */
        if (irq_at != NEVER && c + 4 >= irq_at) {
            unsigned l = line_of(irq_at - 5);
            timeup_flag = 1;
            timeup_at = irq_at;
            irq_at = irq_find(l + 1, NEVER);
            look_again++;
            continue;
        }
        /* the counters reach the next frame 4 clocks before its start
           here: its field */
        if (!edge_done && c + 4 >= frame_end()) {
            edge_done = 1;
            ppu_field_flip();
            continue;
        }
        /* the NMI's condition is tested 2 clocks before a line's start:
           its rise sets the flag (and the NMI), its fall clears the flag */
        if (nmi_edge != NO_LINE && c + 2 >= line_at(nmi_edge)) {
            unsigned n = nmi_edge;
            int valid = n < frame_len && n >= snes_vdisp();
            if (valid != nmi_valid) {
                rdnmi_flag = (uint8_t)valid;
                nmi_rose = valid;
                nmi_valid = valid;
                if (valid)
                    rose_line = n;
            }
            nmi_from = n + 1;
            nmi_edge = nmi_next();
            look_again++;
            continue;
        }
        if (!setup_done && c >= setup_time()) {
            setup_done = 1;
            if (hdma_enabled()) {
                hdma_pending = 1;
                hdma_mode = 0;
                hdma_line_of = 0;
            }
            continue;
        }
        if ((t = hdma_next()) != NEVER && c >= t) {
            hdma_pending = 1;
            hdma_mode = 1;
            hdma_line_of = hdma_from;
            hdma_from++;
            continue;
        }
        /* HDMA the CPU made no cycle for (a stop) is made at the line's
           end */
        if (hdma_pending && hdma_line_of != NO_LINE && c >= line_at(hdma_line_of + 1)) {
            unsigned keep = unit_cc;
            hdma_line_of = NO_LINE;
            dma_active = 1;
            unit_cc = 0;
            unit_edge();
            unit_cc = keep;
            continue;
        }
        if (c >= (t = begin_next())) {
            line_begin(line_of(t));
            continue;
        }
        break;
    }
    events_next();
}

uint32_t snes_vblanks(void) { return vblanks; }

static int interrupts_held;

/* the clock up to which a run of the DMA unit's bytes may go with nothing
   of the console's in it: a deadline, a refresh, a line's record */
uint64_t snes_run_limit(void)
{
    uint64_t t = next_event < refresh_next ? next_event : refresh_next;
    if (rendered < vblank_line && line_at(rendered) + HBLANK_AT < t)
        t = line_at(rendered) + HBLANK_AT;
    return t;
}

void snes_tick(unsigned clocks)
{
    snes_clock += clocks;
    if (snes_clock >= refresh_next)
        refresh_take();
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
    /* (where it stopped: the step's end, or its IRQ's clock) */
    chip_at = chip_to;
    return cart_irq;
}

/* nshvc_hold_interrupts: due interrupts wait while held */
void nshvc_hold_interrupts(int on)
{
    bus_enter();
    on = on != 0;
    /* the chip keeps what it holds of its interrupt (cart.h) */
    if (chip && chip->hold && on != interrupts_held)
        chip->hold(on);
    interrupts_held = on;
    bus_leave();
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
        interrupts_taken++;
        if (snes_interrupt_hook) {
            /* (the handler's C: its own inline accesses) */
            bus_leave();
            snes_interrupt_hook(1);
            bus_enter();
        }
        return 1;
    }
    if (((timeup_flag && timeup_at <= snes_clock) || (cart_irq && cart_irq_at <= snes_clock)) && snes_interrupt_hook &&
        !masked) {
        dbg_irqs++;
        interrupts_taken++;
        bus_leave();
        snes_interrupt_hook(0);
        bus_enter();
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
        unsigned cv, len;
        uint64_t h;
        counters(&cv, &h, &len);
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
        irq_rewrite();
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
        wrmpyb = v;
        rdmpy = (uint16_t)(wrmpya * wrmpyb);
        rddiv = wrmpyb;
        return;
    case 0x4204: wrdiv = (uint16_t)((wrdiv & 0xFF00) | v); return;
    case 0x4205: wrdiv = (uint16_t)((wrdiv & 0x00FF) | v << 8); return;
    case 0x4206:
        wrdivb = v;
        rddiv = wrdivb ? (uint16_t)(wrdiv / wrdivb) : 0xFFFF;
        rdmpy = wrdivb ? (uint16_t)(wrdiv % wrdivb) : wrdiv;
        return;
    case 0x4207: htime = (uint16_t)((htime & 0x100) | v); irq_rewrite(); events_next(); return;
    case 0x4208: htime = (uint16_t)((htime & 0xFF) | (v & 1) << 8); irq_rewrite(); events_next(); return;
    case 0x4209: vtime = (uint16_t)((vtime & 0x100) | v); irq_rewrite(); events_next(); return;
    case 0x420A: vtime = (uint16_t)((vtime & 0xFF) | (v & 1) << 8); irq_rewrite(); events_next(); return;
    case 0x420B:
        /* the channels' transfers: the S-CPU stops for them */
        dma_mdmaen = v;
        if (v)
            dma_now();
        return;
    case 0x420C: dma_hdmaen = v; events_moved(); return;
    case 0x420D: memsel = v; return;   /* (bus_leave: the regions' costs) */
    }
    if (off >= 0x4300 && off < 0x4380)
        dma_write(off, v);
}

/* The automatic read's edges up to now, from the clock with no walk over
   them. Edge 0 (counter 34: the frame's first, still to come) raises the
   latch, edge 1 lowers it. Edges 2 to 33 take turns: an even one clocks the
   ports, the odd one after it shifts the bits into JOY1-4. Nothing they use
   changes between two calls: a write to $4016, $4200 or $4201 comes here
   first. */
static void joy_run(void)
{
    unsigned c, e, reads, shifts, carried, left;
    unsigned a1 = 0, a2 = 0, b1 = 0, b2 = 0;
    uint64_t k;
    if (joy_next > snes_clock)
        return;
    if (joy_counter == 34) {
        int on = nmitimen & 1;
        joy_counter = 0;
        joy_auto_latch = on;
        ports_latch(joy_auto_latch | joy_cpu_latch);
        if (!on) {
            joy_counter = 33;
            joy_next = NEVER;
            return;
        }
        joy_reg[0] = joy_reg[1] = joy_reg[2] = joy_reg[3] = 0;
        joy_next += 128;
        if (joy_next > snes_clock)
            return;
    }
    if (joy_counter == 0) {
        joy_counter = 1;
        joy_auto_latch = 0;
        ports_latch(joy_cpu_latch);
        joy_next += 128;
        if (joy_next > snes_clock)
            return;
    }
    /* turned off before edge 2: it ends there (later, the write ends it) */
    if (!(nmitimen & 1)) {
        joy_counter = 33;
        joy_next = NEVER;
        return;
    }
    c = joy_counter;
    k = (snes_clock - joy_next) / 128 + 1;
    e = k < 33 - c ? c + (unsigned)k : 33;
    reads = e / 2 - c / 2;
    shifts = (e + 1) / 2 - (c + 1) / 2;
    carried = !(c & 1);          /* a read that waits for its shift */
    if (reads) {
        ports_bits(0, reads, &a1, &a2);
        ports_bits(1, reads, &b1, &b2);
    }
    if (carried) {
        a1 |= (joy_bits[0] & 1) << reads;
        a2 |= (joy_bits[0] >> 1 & 1) << reads;
        b1 |= (joy_bits[1] & 1) << reads;
        b2 |= (joy_bits[1] >> 1 & 1) << reads;
    }
    left = reads + carried - shifts;
    joy_reg[0] = (uint16_t)((uint32_t)joy_reg[0] << shifts | a1 >> left);
    joy_reg[1] = (uint16_t)((uint32_t)joy_reg[1] << shifts | b1 >> left);
    joy_reg[2] = (uint16_t)((uint32_t)joy_reg[2] << shifts | a2 >> left);
    joy_reg[3] = (uint16_t)((uint32_t)joy_reg[3] << shifts | b2 >> left);
    if (reads) {
        joy_bits[0] = (a1 & 1) | (a2 & 1) << 1;
        joy_bits[1] = (b1 & 1) | (b2 & 1) << 1;
    }
    joy_counter = e;
    joy_next = e < 33 ? joy_next + (e - c) * (uint64_t)128 : NEVER;
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
    unsigned v, len;
    uint64_t h;
    counters(&v, &h, &len);
    ppu_latch_counters((uint16_t)dot_of(h, len), (uint16_t)v);
}

/* ---- the B bus --------------------------------------------------------------------- */
uint8_t bus_read_b(uint8_t reg)
{
    if (reg < 0x40)
        ppu_catch_up();
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
        if (reg == 0x3E) {
            unsigned v, len;
            uint64_t h;
            counters(&v, &h, &len);
            ppu_stat_catch_up(v, (unsigned)h, vblank_line);
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
        ppu_catch_up();
        ppu_write(reg, v);
        /* SETINI: the vertical blank's line moves the NMI's and HDMA's */
        if (reg == 0x33)
            events_moved();
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
        ppu_catch_up();
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
            if (chip && chip->read && chip->read(addr, &v)) {
                read_by_chip = 1;
                return v;
            }
        }
        return snes_mdr;
    }
    {
        uint8_t v;
        if (chip && chip->read && chip->read(addr, &v)) {
            read_by_chip = 1;
            return v;
        }
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

int bus_read_plain(uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    uint32_t at;
    int what;
    if (bank == 0x7E || bank == 0x7F || (!(bank & 0x40) && off < 0x2000))
        return 1;
    if ((!(bank & 0x40) && off < 0x6000) || chip)
        return 0;
    what = cart_decode(bank, off, &at);
    if (what == CART_RAM)
        return 1;
    return what == CART_ROM && (snes_cart.rom || (snes_cart.rom_pages && snes_cart.rom_pages[at >> 8]));
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
/* What an access costs the S-CPU (contract 2.1): the bus's time for it, as
   the console's memory speeds give it (8 clocks for work RAM, the
   cartridge's RAM and ROM, 6 for ROM in banks $80-$FF when MEMSEL selects
   FastROM, 6 for most registers, 12 for $4000-$41FF), and before it the
   allowance: the CPU's own work around an access of that kind (its
   instruction fetches and internal cycles, which the C does not make),
   the same for every game. Neither is the game's to state.

   The allowance by kind (nshvc.h NSHVC_KIND_*: a read or a write of work
   RAM, ROM, cartridge RAM, I/O), in tenths of a master clock: the
   cartridge's cost profile (nshvc_cart.cost_profile, docs/cost-profile.md)
   or, with none, 0: the bus's time alone. The tenths carry from one access
   to the next. nshvc_set_cpu_costs(0) takes the S-CPU's accesses out of
   the console's time as the game runs (its Enhanced mode: no bus time, no
   weight; its waits and a DMA still take theirs), (1) puts them back.
   NSHVC_CPU_ALLOWANCE overrides the weights, for
   measuring only: one value in clocks for every kind, or eight,
   comma-separated, the reads' four then the writes'. */
static uint32_t allowance[2][NSHVC_KINDS];
static uint32_t allowance_carry;          /* tenths not yet spent */
static int allowance_on;                  /* nshvc_set_cpu_costs: 0, accesses take no time */

int snes_costs_on(void) { return allowance_on; }

void nshvc_set_cpu_costs(int on)
{
    bus_enter();
    allowance_on = on != 0;
    allowance_carry = 0;
    bus_leave();
}

int nshvc_access_kind(uint32_t addr)
{
    uint8_t bank = (uint8_t)(addr >> 16), b = bank & 0x7F;
    uint16_t off = (uint16_t)addr;
    if ((b >= 0x70 && b <= 0x7D && off < 0x8000) || (bank >= 0xF0 && off < 0x8000) ||
        (b >= 0x20 && b <= 0x3F && off >= 0x6000 && off < 0x8000))
        return NSHVC_KIND_CART_RAM;
    if (bank == 0x7E || bank == 0x7F || (!(bank & 0x40) && off < 0x2000))
        return NSHVC_KIND_WRAM;
    if (!(bank & 0x40) && off < 0x6000)
        return NSHVC_KIND_IO;
    return NSHVC_KIND_ROM;
}

/* the cartridge's profile, NSHVC_CPU_ALLOWANCE, or the console's own */
static void allowance_set(void)
{
    const char *e = getenv("NSHVC_CPU_ALLOWANCE");
    const nshvc_cost_profile *p = snes_cart.cost_profile;
    double v[2 * NSHVC_KINDS];
    int n = 0, w, k;
    memset(allowance, 0, sizeof allowance);
    allowance_carry = 0;
    allowance_on = 1;
    if (p) {
        if (p->version != NSHVC_COST_PROFILE_VERSION)
            snes_fatal("the cartridge's cost profile is version %d; this console reads version %d", p->version,
                       NSHVC_COST_PROFILE_VERSION);
        memcpy(allowance, p->cpu_tenths, sizeof allowance);
    }
    if (!e)
        return;
    while (n < 2 * NSHVC_KINDS && sscanf(e, "%lf", &v[n]) == 1) {
        n++;
        e = strchr(e, ',');
        if (!e)
            break;
        e++;
    }
    if (n != 1 && n != 2 * NSHVC_KINDS)
        snes_fatal("NSHVC_CPU_ALLOWANCE: one value in clocks, or eight (reads of work RAM, ROM, cartridge RAM, "
                   "I/O, then writes)");
    for (w = 0; w < 2; w++)
        for (k = 0; k < NSHVC_KINDS; k++)
            allowance[w][k] = (uint32_t)(v[n == 1 ? 0 : NSHVC_KINDS * w + k] * 10 + 0.5);
}

/* the bus's time for an access */
static unsigned access_clocks(uint32_t addr)
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

/* An access's bus time and kind (access_clocks', nshvc_access_kind's) by
   its bank and 8 KB of the bank, for MEMSEL's FastROM bit clear and set:
   the clocks in bits 0-3, the kind in 4-7. The clocks are 0 for
   $4000-$5FFF in banks $00-$3F and $80-$BF, which hold both 12 (below
   $4200) and 6. */
static uint8_t acc_info[2][256][8];

static void acc_info_build(void)
{
    uint8_t was = memsel;
    unsigned f, bank, r;
    for (f = 0; f < 2; f++) {
        memsel = (uint8_t)f;
        for (bank = 0; bank < 256; bank++)
            for (r = 0; r < 8; r++) {
                uint32_t a = bank << 16 | r << 13;
                unsigned cc = r == 2 && !(bank & 0x40) ? 0 : access_clocks(a);
                acc_info[f][bank][r] = (uint8_t)(cc | (unsigned)nshvc_access_kind(a) << 4);
            }
    }
    memsel = was;
}

/* the bus time of an access to `addr` (24 bits), its kind in *kind */
static inline unsigned acc_lookup(uint32_t addr, unsigned *kind)
{
    unsigned i = acc_info[memsel & 1][addr >> 16][addr >> 13 & 7], cc = i & 15;
    *kind = i >> 4;
    if (!cc)
        cc = (uint16_t)addr < 0x4200 ? 12 : 6;
    return cc;
}

/* work RAM's byte for an access of kind NSHVC_KIND_WRAM */
static inline uint8_t *wram_at(uint32_t addr)
{
    return &wram[(addr & 0xFE0000) == 0x7E0000 ? addr & 0x1FFFF : addr & 0x1FFF];
}

/* Code run with interrupts held (an app's, outside the console's time), or
   with the S-CPU's costs off (nshvc_set_cpu_costs(0)), costs nothing, so a
   loop of it that waits for the hardware never ends: the front end's watchdog
   ends the run, naming the last access made through the console. */
static volatile uint32_t last_access;
uint32_t snes_last_access(void) { return last_access; }

/* the time before an access: the allowance (the C's own, by the access's
   kind, `write`; none for the console's interrupt sequence, `game` 0),
   then the cycle's start, where the DMA unit may take the bus */
/* the weight of an access of the C's (its kind's, from the profile),
   spent: none with interrupts held or the costs off (a word's weights are
   spent here, ahead of its first byte, before access_begins looks) */
static void allowance_spend(uint32_t addr, int write)
{
    if (interrupts_held || !allowance_on)
        return;
    allowance_carry += allowance[write][acc_info[memsel & 1][addr >> 16 & 0xFF][addr >> 13 & 7] >> 4];
    if (allowance_carry >= 10) {
        snes_tick(allowance_carry / 10);
        allowance_carry %= 10;
    }
}

static int access_begins(uint32_t addr, unsigned cc, int game, int write)
{
    if (interrupts_held || !allowance_on)
        return 0;
    if (game)
        allowance_spend(addr, write);
    cycle_begins(cc);
    return 1;
}

/* A word, a long, a read-modify-write's two reads or two writes: one
   instruction's, its bytes in consecutive bus cycles as the 65816 makes
   them. Each byte's weight is spent before the first, so none comes
   between them (an S-CPU that writes a port's index and its byte as a
   word lands them a cycle apart, as on the console). */
static void allowance_ahead(uint32_t addr, int write, int bytes)
{
    while (bytes--)
        allowance_spend(addr++, write);
}

static void turn_note(uint32_t addr, uint8_t v);

/* a read: its value 4 clocks before its cycle ends */
/* An access with no DMA due at its cycle's start and no event of the
   console's before its end (next_event) takes its allowance and bus time
   at once (fast_time): the same clock, nothing in it to run. Else they are
   taken step by step (access_begins). */
static inline int fast_time(unsigned kind, unsigned cc, int game, int write, unsigned *carry, unsigned *pre)
{
    unsigned c = allowance_carry, n = 0;
    if (interrupts_held || !allowance_on || (dma_active | dma_pending | hdma_pending))
        return 0;
    if (game) {
        c += allowance[write][kind];
        if (c >= 10) {
            n = c / 10;
            c -= n * 10;
        }
    }
    if (snes_clock + n + cc >= next_event || snes_clock + n + cc >= refresh_next)
        return 0;
    *carry = c;
    *pre = n;
    return 1;
}

static uint8_t access_rd(uint32_t addr, int game)
{
    uint8_t v;
    unsigned cc, kind, carry, pre;
    addr &= 0xFFFFFF;
    cc = acc_lookup(addr, &kind);
    dbg_last_read = addr;
    last_access = addr;
    if (fast_time(kind, cc, game, 0, &carry, &pre)) {
        allowance_carry = carry;
        snes_clock += pre + cc - 4;
        if (kind == NSHVC_KIND_WRAM) {
            v = *wram_at(addr);
            if (turn.on)
                turn_note(addr, v);
            snes_clock += 4;
        } else {
            /* (a register's read may bring an event nearer) */
            read_by_chip = 0;
            v = bus_read(addr, 0);
            if (turn.on)
                turn_note(addr, v);
            snes_tick(4);
        }
    } else if (access_begins(addr, cc, game, 0)) {
        snes_tick(cc - 4);
        read_by_chip = 0;
        v = bus_read(addr, 0);
        if (turn.on)
            turn_note(addr, v);
        snes_tick(4);
    } else {
        read_by_chip = 0;
        v = bus_read(addr, 0);
        if (turn.on)
            turn_note(addr, v);
    }
    snes_mdr = v;
    if (snes_read_hook)
        snes_read_hook(addr, v);
    if (chip_direct && kind != NSHVC_KIND_WRAM)
        direct_ask(addr);
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
    /* (the chip brought up to now already: nothing to look at) */
    if ((chip_at == UINT64_MAX || snes_clock > chip_at) && chip_watch(1))
        chip_sync();
    /* (none raised: none to take) */
    if (nmi_pending | timeup_flag | cart_irq)
        check_interrupts();
}

/* ---- the inline accesses (machine.h NSHVC_BUS_INLINE) ----
   An access the C makes inline spends its region's cost (nshvc_bus:
   tenths of a master clock, its bus time and its kind's weight) from
   `left`, the tenths that can pass before the console's next event; it is
   made only when some are left after it, so no event, and nothing else
   the console does, falls in it or in any before it since the limit was
   set. Its time is the clock's when settled (bus_settle): the tenths
   spent, with the allowance's carry, are the same whole clocks the calls
   would have ticked one by one. The limit is 0 (every access the call)
   while anything could come between two operations: an interrupt raised,
   a DMA or HDMA due, the chip able to raise its IRQ, a read-modify-write
   open, or a hook watching the accesses. With interrupts held or the
   costs off, accesses take no time, so nothing is spent: where nothing else
   is waiting on them they are a load or a store alone (nshvc_bus.plain).
   Nativeshvc's own code runs with the limit 0 (bus_enter), and sets it
   again before the C runs (bus_leave): as an access returns, and round a
   handler, a wait's condition or the frame's hook. */
static int32_t bus_left0;            /* nshvc_bus.left as last set */
static int bus_key = -1;             /* the regions' costs as built: MEMSEL's bit 0 | bus_free << 1 */

static void bus_settle_now(void)
{
    int32_t left = nshvc_bus.left, used = bus_left0 - left;
    if (used) {
        uint32_t t = allowance_carry + (uint32_t)used;
        snes_clock += t / 10;
        allowance_carry = t % 10;
        /* the refreshes in the work's own time (the limit leaves room for
           them before the next deadline) */
        if (snes_clock >= refresh_next)
            refresh_take();
    }
    bus_left0 = left;
}

/* (nothing to settle when no inline access was made since: a call's own
   enter and leave, and the most of them) */
static inline __attribute__((always_inline)) void bus_settle_inline(void)
{
    if (nshvc_bus.left != bus_left0)
        bus_settle_now();
}
static void bus_settle(void) { bus_settle_inline(); }

static void bus_enter(void)
{
    bus_settle_inline();
    nshvc_bus.plain = 0;
    nshvc_bus.left = bus_left0 = 0;
}

/* where each region's accesses may be made directly: work RAM, and ROM a
   chip does not answer (a whole 8 KB of it, in order: the ROM's size a
   multiple of 8 KB, its mirrors whole blocks of it) */
static void regions_place(void)
{
    unsigned bank, r;
    for (bank = 0; bank < 256; bank++)
        for (r = 0; r < 8; r++) {
            nshvc_bus_region *g = &nshvc_bus.region[bank << 3 | r];
            uint16_t off = (uint16_t)(r << 13);
            uint32_t at0, at1;
            g->rd = NULL;
            g->wr = NULL;
            if (bank == 0x7E || bank == 0x7F) {
                g->wr = &wram[(bank & 1) << 16 | off];
                g->rd = g->wr;
            } else if (!(bank & 0x40) && r == 0) {
                g->wr = wram;
                g->rd = wram;
            } else if (snes_cart.rom && !(chip && chip->read) && !(!(bank & 0x40) && off < 0x6000) &&
                       snes_cart.rom_size % 0x2000 == 0 && cart_decode((uint8_t)bank, off, &at0) == CART_ROM &&
                       cart_decode((uint8_t)bank, (uint16_t)(off + 0x1FFF), &at1) == CART_ROM && at1 == at0 + 0x1FFF) {
                g->rd = snes_cart.rom + at0;
            }
        }
    bus_key = -1;
}

/* each region's costs: its bus time and its kind's weight in tenths */
static void regions_build(int key)
{
    unsigned i;
    for (i = 0; i < 2048; i++) {
        nshvc_bus_region *g = &nshvc_bus.region[i];
        unsigned info = acc_info[memsel & 1][i >> 3][i & 7], cc = info & 15, kind = info >> 4;
        g->cost_rd = (int32_t)(10 * cc + allowance[0][kind]);
        g->cost_wr = (int32_t)(10 * cc + allowance[1][kind]);
    }
    bus_key = key;
}

static void bus_leave(void)
{
    int32_t left = 0;
    int free = interrupts_held || !allowance_on, plain = 0;
    bus_settle_inline();
    if (((memsel & 1) | free << 1) != bus_key)
        regions_build((memsel & 1) | free << 1);
    if (!snes_read_hook && !snes_write_hook && !rmw_open && !turn.on) {
        if (free) {
            /* No time passes in the C's accesses: nothing comes due between
               two of its operations, from the console or the chip (whose
               clock does not move), but an interrupt raised already. */
            plain = interrupts_held || !(nmi_pending | timeup_flag | cart_irq);
        } else if (!(nmi_pending | timeup_flag | cart_irq | dma_active | dma_pending | hdma_pending) &&
                   next_event > snes_clock && !chip_watch(0)) {
            /* up to the next deadline, less 40 clocks for each refresh
               before it (so the work's own time, its refreshes added,
               ends short of it) */
            static uint64_t for_event = NEVER, for_refresh = NEVER, r;
            uint64_t d = next_event - snes_clock;
            /* (worked out again only when the deadline or the next refresh
               moved) */
            if (for_event != next_event || for_refresh != refresh_next) {
                for_event = next_event;
                for_refresh = refresh_next;
                r = REFRESH_CLOCKS * refreshes_before(next_event);
            }
            if (d > r) {
                d -= r;
                left = d >= 100000000u ? 1000000000 : (int32_t)(10 * d - allowance_carry);
            }
        }
    }
    nshvc_bus.left = bus_left0 = left;
    nshvc_bus.plain = plain;
}

uint64_t snes_now(void)
{
    bus_settle();
    turn.step |= turn.on;
    return snes_clock;
}

void snes_bus_enter(void) { bus_enter(); }
void snes_bus_leave(void) { bus_leave(); }

/* a read of the C's; `game` 0: its weight spent already (allowance_ahead) */
static uint8_t c_rd(uint32_t addr, int game)
{
    if (rmw_open)
        snes_fatal("rd8_rmw at $%06X: a read of $%06X before its write", (unsigned)rmw_at, (unsigned)(addr & 0xFFFFFF));
    return access_rd(addr, game);
}

static uint8_t op_rd8(uint32_t addr)
{
    uint8_t v = c_rd(addr, 1);
    between();
    return v;
}

static uint16_t op_rd16(uint32_t addr)
{
    uint8_t lo;
    uint16_t v;
    if (rmw_open)
        c_rd(addr, 0);
    allowance_ahead(addr, 0, 2);
    lo = c_rd(addr, 0);
    v = (uint16_t)(lo | c_rd((addr + 1) & 0xFFFFFF, 0) << 8);
    between();
    return v;
}

static uint32_t op_rd24(uint32_t addr)
{
    uint32_t v;
    if (rmw_open)
        c_rd(addr, 0);
    allowance_ahead(addr, 0, 3);
    v = c_rd(addr, 0);
    v |= (uint32_t)c_rd((addr + 1) & 0xFFFFFF, 0) << 8;
    v |= (uint32_t)c_rd((addr + 2) & 0xFFFFFF, 0) << 16;
    between();
    return v;
}

static uint8_t op_rd8_rmw(uint32_t addr)
{
    uint8_t v = c_rd(addr, 1);
    rmw_open = 1;
    rmw_at = addr & 0xFFFFFF;
    return v;
}

static uint16_t op_rd16_rmw(uint32_t addr)
{
    uint8_t lo;
    uint16_t v;
    if (rmw_open)
        c_rd(addr, 0);
    allowance_ahead(addr, 0, 2);
    lo = c_rd(addr, 0);
    v = (uint16_t)(lo | c_rd((addr + 1) & 0xFFFFFF, 0) << 8);
    rmw_open = 1;
    rmw_at = addr & 0xFFFFFF;
    return v;
}

/* a write: it lands at its cycle's end */
static void access_wr(uint32_t addr, uint8_t value, int game)
{
    unsigned cc, kind, carry, pre;
    addr &= 0xFFFFFF;
    cc = acc_lookup(addr, &kind);
    last_access = addr | 0x1000000u;
    if (fast_time(kind, cc, game, 1, &carry, &pre)) {
        allowance_carry = carry;
        snes_clock += pre + cc;
    } else if (access_begins(addr, cc, game, 1)) {
        snes_tick(cc);
    }
    snes_mdr = value;
    if (snes_write_hook)
        snes_write_hook(addr, value);
    /* (the console's registers: never inline, so this hook keeps the rest
       inline) */
    if (snes_register_write_hook && !(addr & 0x400000) && (uint16_t)addr >= 0x2100 &&
        ((uint16_t)addr < 0x2200 || ((uint16_t)addr >= 0x4000 && (uint16_t)addr < 0x4400)))
        snes_register_write_hook(addr, value);
    if (kind == NSHVC_KIND_WRAM)
        *wram_at(addr) = value;
    else {
        bus_write(addr, value, 0);
        if (chip_direct)
            direct_ask(addr);
    }
}

/* a write ends the read-modify-write whose read came before it */
static void op_wr8(uint32_t addr, uint8_t value)
{
    access_wr(addr, value, 1);
    rmw_open = 0;
    between();
}

static void op_wr16(uint32_t addr, uint16_t value)
{
    allowance_ahead(addr, 1, 2);
    access_wr(addr, (uint8_t)value, 0);
    access_wr((addr + 1) & 0xFFFFFF, (uint8_t)(value >> 8), 0);
    rmw_open = 0;
    between();
}

static void op_wr16_rmw(uint32_t addr, uint16_t value)
{
    allowance_ahead(addr, 1, 2);
    access_wr((addr + 1) & 0xFFFFFF, (uint8_t)(value >> 8), 0);
    access_wr(addr, (uint8_t)value, 0);
    rmw_open = 0;
    between();
}

/* the interrupt sequence's pushes and RTI's pulls: the console's own
   accesses, their bus time and no allowance */
void snes_interrupt_push(uint32_t addr, uint8_t value)
{
    bus_enter();
    access_wr(addr, value, 0);
    bus_leave();
}

uint8_t snes_cpu_read_within(uint32_t addr)
{
    uint8_t v;
    bus_enter();
    v = access_rd(addr, 0);
    bus_leave();
    return v;
}

/* the calls: the inline accesses' way when theirs is not open, and the
   C's other operations */
uint8_t nshvc_bus_rd8(uint32_t addr)
{
    uint8_t v;
    bus_enter();
    v = op_rd8(addr);
    bus_leave();
    return v;
}

uint16_t nshvc_bus_rd16(uint32_t addr)
{
    uint16_t v;
    bus_enter();
    v = op_rd16(addr);
    bus_leave();
    return v;
}

void nshvc_bus_wr8(uint32_t addr, uint8_t value)
{
    bus_enter();
    op_wr8(addr, value);
    bus_leave();
}

void nshvc_bus_wr16(uint32_t addr, uint16_t value)
{
    bus_enter();
    op_wr16(addr, value);
    bus_leave();
}

uint8_t rd8(uint32_t addr) { return nshvc_bus_rd8_inline(addr); }
uint16_t rd16(uint32_t addr) { return nshvc_bus_rd16_inline(addr); }
void wr8(uint32_t addr, uint8_t value) { nshvc_bus_wr8_inline(addr, value); }
void wr16(uint32_t addr, uint16_t value) { nshvc_bus_wr16_inline(addr, value); }

uint32_t rd24(uint32_t addr)
{
    uint32_t v;
    bus_enter();
    v = op_rd24(addr);
    bus_leave();
    return v;
}

uint8_t rd8_rmw(uint32_t addr)
{
    uint8_t v;
    bus_enter();
    v = op_rd8_rmw(addr);
    bus_leave();
    return v;
}

uint16_t rd16_rmw(uint32_t addr)
{
    uint16_t v;
    bus_enter();
    v = op_rd16_rmw(addr);
    bus_leave();
    return v;
}

void wr16_rmw(uint32_t addr, uint16_t value)
{
    bus_enter();
    op_wr16_rmw(addr, value);
    bus_leave();
}

/* ---- the S-CPU's waits ----
   A wait is the original's poll loop, with nothing of the loop's own
   instructions in it: time passes, and the condition looks again where
   what it read can change. Its reads are noted as it runs (the inline
   accesses made as calls meanwhile), and each tells when it could next
   answer otherwise (read_changes): a status bit or a counter at the clock
   the beam brings it to, an interrupt's flag as it rises, memory never on
   its own. The clock goes there, the console's deadlines on the way (an
   interrupt taken at its clock, its handler run, is a look again; so are
   the NMI's flag, the vertical blank's and the frame's start, which change
   what reads see, and HDMA's work where a read could see it). The sound
   unit's ports: the SPC700 runs ahead to its next change of one, while nothing
   can write them (wait_port_ahead). The chip's registers: the chip says when
   (nshvc_chip.next_change). A read the console cannot tell the next change of
   (a chip's with no answer, a read with an effect of its own such as a data
   port's) has the condition look again WAIT_STEP clocks on, as the loop
   would within a few of its turns. A wait with nothing of its own to read
   (WAI, a loop for ever, the vertical blank's wait) goes from one
   deadline to the next. Each step's start is a cycle of the S-CPU's, where
   the DMA unit takes the bus, and the cartridge's chip is run ahead
   through it so a rise of its IRQ ends it (cart.h). */
#define WAIT_STEP 64

static void bound(uint32_t *n, uint64_t at)
{
    if (at > snes_clock && at - snes_clock < *n)
        *n = (uint32_t)(at - snes_clock);
}

/* when HVBJOY's bits next change: the vertical blank's (the counters'
   line reaching the blank's or the frame's end), the horizontal blank's
   (the counters' dot 274, 1,092 clocks into the line, to 2 clocks into the
   next), the joypads' read's (its first edge, its last) */
static uint64_t hvbjoy_changes(void)
{
    unsigned l = line_of(snes_clock), v = snes_vdisp();
    uint64_t ls = line_at(l), t = NEVER, cl = line_of(snes_clock + 4);
    /* (cl: the counters' line; past the frame's, the next frame's first
       4 clocks, whose blank's start the frame's start looks again at) */
    if (cl < v)
        t = line_at(v) - 4;
    else if (cl < frame_len)
        t = frame_end() - 4;
    if (snes_clock < ls + HBLANK_AT - 4) {
        if (ls + HBLANK_AT - 4 < t)
            t = ls + HBLANK_AT - 4;
    } else if (snes_clock < ls + line_len_of(l) - 1) {
        if (ls + line_len_of(l) - 1 < t)
            t = ls + line_len_of(l) - 1;
    } else if (line_at(l + 1) + HBLANK_AT - 4 < t) {
        t = line_at(l + 1) + HBLANK_AT - 4;
    }
    if (nmitimen & 1) {
        joy_run();
        if (joy_counter == 34 && joy_next < t)
            t = joy_next;
        else if (joy_counter < 33 && joy_next + (uint64_t)(32 - joy_counter) * 128 < t)
            t = joy_next + (uint64_t)(32 - joy_counter) * 128;
    }
    return t;
}

/* The clock from which a read of `addr` that saw `seen` could next
   answer otherwise (made now, at the clock it would be: snes_clock for at
   once; NEVER: only at a look again), *step set where the console cannot
   tell, *dma and *mem where HDMA's work could change it. */
static uint64_t read_changes(uint32_t addr, uint8_t seen, int by_chip, int *step, int *port, int *dma, int *mem)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    if (by_chip) {
        uint64_t t = chip && chip->next_change ? chip->next_change(addr, seen) : 0;
        if (!t) {
            *step = 1;
            return NEVER;
        }
        if (t == NSHVC_CART_NO_CHANGE)
            return NEVER;
        return t > snes_clock ? t : snes_clock;
    }
    if (bank == 0x7E || bank == 0x7F || (!(bank & 0x40) && off < 0x2000)) {
        *mem = 1;
        return NEVER;
    }
    if (!(bank & 0x40) && off < 0x6000) {
        if (off >= 0x2100 && off < 0x2200) {
            unsigned r = off & 0xFF;
            /* the sound unit's ports */
            if (r >= 0x40 && r < 0x80)
                *port = 1;
            /* reads with an effect of their own: the counters' latch and its
               bytes, the data ports, STAT78 */
            else if ((r >= 0x37 && r <= 0x3D) || r == 0x3F || r == 0x80)
                *step = 1;
            return NEVER;
        }
        if (off == 0x4016 || off == 0x4017) {
            *step = 1;
            return NEVER;
        }
        if (off == 0x4210) {
            /* the flag up since (or seen and cleared by) the read: at once;
               its rise is a look again */
            return rdnmi_flag || (seen & 0x80) ? snes_clock : NEVER;
        }
        if (off == 0x4211) {
            if (timeup_flag)
                return timeup_at > snes_clock ? timeup_at : snes_clock;
            return seen & 0x80 ? snes_clock : NEVER;
        }
        if (off == 0x4212)
            return hvbjoy_changes();
        if (off >= 0x4218 && off < 0x4220) {
            joy_run();
            return joy_counter < 33 || joy_counter == 34 ? joy_next : NEVER;
        }
        if (off >= 0x4300 && off < 0x4380) {
            *dma = 1;
            return NEVER;
        }
        return NEVER;
    }
    *mem = 1;
    return NEVER;
}

/* a read of the condition's, made: when it could next answer otherwise,
   worked out at the clock it took its value (access_rd) */
static void turn_note(uint32_t addr, uint8_t v)
{
    uint64_t c = read_changes(addr, v, read_by_chip, &turn.step, &turn.port, &turn.dma, &turn.mem);
    if (c < turn.until)
        turn.until = c;
}

/* the clock moved on to `to` in a wait: a stop in a refresh's 40 clocks
   ends at its end (the S-CPU stopped there); one before, passed */
static void wait_clock(uint64_t to)
{
    if (to >= refresh_next) {
        unsigned l = line_of(to);
        uint64_t r = refresh_of(l);
        if (to >= r) {
            if (to < r + REFRESH_CLOCKS)
                to = r + REFRESH_CLOCKS;
            refresh_next = refresh_of(l + 1);
        } else {
            refresh_next = r;
        }
    }
    snes_clock = to;
}

static void wait_to(uint64_t to)
{
    wait_clock(to);
    if (snes_clock >= next_event)
        run_events();
}

/* The most clocks from a line's HDMA point to its transfer's end: the step
   in, 8 channels of 4 bytes and 3 table bytes at 8 clocks each, the transfer's
   start, aligning and giving back. */
#define HDMA_SPAN_MAX 600

/* A wait's HDMA lines, no deadline each. In a wait nothing else moves a line's
   transfer: its work at the same clocks as wait_step's three steps to it (to
   its point, the step of a cycle it is taken up in, the transfer aligned and
   given back as unit_edge's), and none of the wait's looks or event passes
   between them, so long as no interrupt or deadline, and nothing the wait stops
   for, falls before the transfer ends. Those a line
   away, a wait reading the DMA registers, or memory a channel writes, or a hook
   on the accesses: wait_step, a line at a time. A running chip is run ahead to
   the start of each transfer and no further, so a rise of its IRQ before that
   stops the lines at its clock. The S-CPU cannot take a rise within a transfer,
   which holds the bus, and the transfer's own reads and writes of the chip bring
   it up to their clocks: it is never ahead of them. */

/* Of q lines that move nothing from hdma_from, those whose transfers end before
   `m` and the vertical blank's */
static unsigned hdma_quiet_lines(unsigned q, uint64_t m)
{
    unsigned last = snes_vdisp() - 1;
    if (last >= frame_len)
        last = frame_len - 1;
    if (line_at(last) + HDMA_AT + HDMA_SPAN_MAX >= m) {
        last = line_of(m);
        if (line_at(last) + HDMA_AT + HDMA_SPAN_MAX >= m)
            last--;
    }
    return last - hdma_from < q - 1 ? last - hdma_from + 1 : q;
}

static void hdma_bulk(uint64_t until)
{
    uint64_t limit = until, t, at;
    int any = 0;
    /* (no line of HDMA is the next thing to come: none of this) */
    if (next_event == next_real)
        return;
    if (dma_active || dma_pending || hdma_pending || dma_mdmaen || snes_read_hook || snes_write_hook || snes_dma_hook ||
        turn.dma || (turn.mem && hdma_writes_a()))
        return;
    if ((t = interrupt_due_at()) > snes_clock && t < limit)
        limit = t;
    if (timeup_flag && timeup_at > snes_clock && timeup_at < limit)
        limit = timeup_at;
    if (cart_irq && cart_irq_at > snes_clock && cart_irq_at < limit)
        limit = cart_irq_at;
    /* (next_real anew each line: a transfer can write SETINI's overscan, whose
       events_next sets it) */
    while ((at = hdma_next()) != NEVER && at > snes_clock && at + HDMA_SPAN_MAX < limit && at + HDMA_SPAN_MAX < next_real) {
        unsigned q = hdma_quiet();
        /* lines that move nothing, as many as this stretch holds, all but the
           last passed at once: the clock at a line's point is the same */
        if (q > 1)
            q = hdma_quiet_lines(q, limit < next_real ? limit : next_real);
        /* the chip to this transfer's start (none, or none running: nothing);
           its IRQ risen first: no line of these, the wait goes on to it */
        if (chip_run_ahead((uint32_t)((q > 1 ? line_at(hdma_from + q - 1) + HDMA_AT : at) + 6 - snes_clock)))
            break;
        if (q > 1) {
            hdma_skip(q - 1);
            hdma_from += q - 1;
            at = hdma_next();
        }
        /* (nothing due in the transfer: the next line's point is past it) */
        next_event = next_real;
        any = 1;
        wait_clock(at);
        hdma_line_of = hdma_from;
        hdma_from++;
        wait_clock(snes_clock + 6);
        unit_cc = 6;
        dma_active = 1;
        unit_align();
        hdma_line();
        look_dma++;
        unit_back();
        dma_active = 0;
        /* (the chip's IRQ, risen in the transfer: no line past it) */
        if (cart_irq && cart_irq_at < limit)
            limit = cart_irq_at;
    }
    if (any)
        events_next();
}

/* a step of a wait, to `until` at most (NEVER: as far as nothing stops
   it): 1 when an interrupt was taken; *look set when it stopped where an
   interrupt or the cartridge's IRQ line changes (the condition looks
   again there) */
static int wait_step(uint64_t until, int *look)
{
    uint32_t n = 0xFFFFFFFFu;
    uint64_t lp = NEVER, t, s0 = snes_clock;
    /* the deadlines the clock has passed with no access's time to reach them:
       power on's, when the costs go off before any access */
    if (snes_clock >= next_event)
        run_events();
    /* one due already (one a handler held, its I flag now clear) */
    if (check_interrupts())
        return 1;
    hdma_bulk(until);
    bound(&n, until);
    if (until <= snes_clock)
        n = 0;
    bound(&n, next_event);
    /* where an interrupt comes due, the IRQ rises (masked or not) or the
       cartridge's line does: the condition looks again there */
    if ((t = interrupt_due_at()) > snes_clock && t < lp)
        lp = t;
    if (timeup_flag && timeup_at > snes_clock && timeup_at < lp)
        lp = timeup_at;
    if (cart_irq && cart_irq_at > snes_clock && cart_irq_at < lp)
        lp = cart_irq_at;
    bound(&n, interrupt_due_at());
    /* the IRQ, masked or not: WAI goes on at it */
    if (timeup_flag)
        bound(&n, timeup_at);
    /* the chip's IRQ, masked or not: a chip's flag changes with it */
    if (cart_irq)
        bound(&n, cart_irq_at);
    /* the chip's IRQ, from its own clock: at once, when it rose in time
       already passed (a refresh) */
    if (n == 0xFFFFFFFFu)
        snes_fatal("a wait with no deadline before it (the console's next event at %llu, the clock %llu)",
                   (unsigned long long)next_event, (unsigned long long)snes_clock);
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
            wait_to(snes_clock + n);
        }
    }
    if (chip_watch(1))
        chip_sync();
    /* (the chip's line risen in the step, run ahead) */
    *look = snes_clock >= lp || (cart_irq && cart_irq_at > s0 && cart_irq_at <= snes_clock);
    return check_interrupts();
}

/* A wait reading the sound unit's ports. Up to the wait's next stop nothing can
   write them: no handler runs before the next interrupt can come due, no DMA is
   under way, an HDMA channel's next line if one writes them. So the SPC700 runs ahead to there,
   back at its first change of a port, and the wait goes to the clock a read
   first sees it, where the condition looks again. A running chip's IRQ is not
   known ahead, and with one of these the wait looks again WAIT_STEP on. */
static void wait_port_ahead(void)
{
    /* (a condition that also reads what HDMA's lines change can end at one) */
    uint64_t next = turn.dma || turn.mem ? next_event : next_real;
    uint64_t limit, t, at;
    if (dma_active || dma_pending || hdma_pending || dma_mdmaen || chip_watch(0)) {
        turn.step = 1;
        return;
    }
    /* (an HDMA channel writing the ports: no further than its next line) */
    if (hdma_writes_ports())
        next = next_event;
    limit = turn.until < next ? turn.until : next;
    if ((t = interrupt_due_at()) > snes_clock && t < limit)
        limit = t;
    if (timeup_flag && timeup_at > snes_clock && timeup_at < limit)
        limit = timeup_at;
    if (cart_irq && cart_irq_at > snes_clock && cart_irq_at < limit)
        limit = cart_irq_at;
    if (limit <= snes_clock || !apu_port_change(limit, &at)) {
        turn.step = 1;
        return;
    }
    if (at < limit)
        limit = at > snes_clock ? at : snes_clock;
    turn.until = limit;
}

/* a wait while interrupts are held (code an app runs outside the console's
   time) could never end as the console's would */
static void wait_allowed(const char *what)
{
    if (interrupts_held)
        snes_fatal("%s with interrupts held: the console's time does not pass in code run held", what);
}

uint64_t nshvc_wait_until(int (*ready)(void *ctx), void *ctx)
{
    uint64_t from;
    int r;
    Turn keep = turn;
    bus_enter();
    from = snes_clock;
    wait_allowed("nshvc_wait_until");
    if (rmw_open)
        snes_fatal("nshvc_wait_until between rd8_rmw at $%06X and its write", (unsigned)rmw_at);
    for (;;) {
        unsigned taken = interrupts_taken, looks = look_again, dmas = look_dma, mems = look_mem;
        memset(&turn, 0, sizeof turn);
        turn.until = NEVER;
        turn.on = 1;
        bus_leave();
        r = ready(ctx);
        bus_enter();
        turn.on = 0;
        if (r)
            break;
        /* an interrupt taken in the condition's reads: the loop's next
           turn looks again at once, at what its handler did */
        if (interrupts_taken != taken)
            continue;
        if (turn.port && !turn.step)
            wait_port_ahead();
        if (turn.step && snes_clock + WAIT_STEP < turn.until)
            turn.until = snes_clock + WAIT_STEP;
        while (snes_clock < turn.until && look_again == looks && (!turn.dma || look_dma == dmas) &&
               (!turn.mem || look_mem == mems)) {
            int look = 0;
            if (wait_step(turn.until, &look) || look)
                break;
        }
    }
    turn = keep;
    from = snes_clock - from;
    bus_leave();
    return from;
}

static int vblank_begun(void *ctx) { return vblanks != *(uint32_t *)ctx; }

void nshvc_wait_vblank(void)
{
    uint32_t v = vblanks;
    nshvc_wait_until(vblank_begun, &v);
}

/* WAI: until an interrupt is due (an NMI, or the IRQ line, masked or not),
   then on; it is taken there when it is not masked */
static int interrupt_due(void);

void snes_wai(void)
{
    bus_enter();
    wait_allowed("WAI");
    for (;;) {
        if (interrupt_due()) {
            between();
            break;
        }
        int look;
        if (wait_step(NEVER, &look))
            break;
    }
    bus_leave();
}

/* a branch to itself: time passes, the interrupts run, for ever */
void snes_loop_forever(void)
{
    bus_enter();
    wait_allowed("a loop for ever");
    for (;;) {
        int look;
        wait_step(NEVER, &look);
    }
}

static int timeup_seen(void) { return timeup_flag && timeup_at <= snes_clock; }
static int interrupt_due(void)
{
    if (chip_watch(0))
        chip_sync();
    return (nmi_pending && nmi_at <= snes_clock) || timeup_seen() || (cart_irq && cart_irq_at <= snes_clock);
}

int snes_interrupt_due(void)
{
    int due;
    bus_enter();
    due = interrupt_due();
    bus_leave();
    return due;
}

/* ---- power ---------------------------------------------------------------------- */
void snes_power(void)
{
    uint32_t n;
    /* the inline accesses: the call until the C first runs */
    nshvc_bus.left = bus_left0 = 0;
    nshvc_bus.plain = 0;
    chip_at = UINT64_MAX;
    memset(wram, 0, sizeof wram);
    wram_port = 0;
    free(sram);
    sram = NULL;
    sram_mask = 0;
    chip = snes_cart.chip;
    chip_direct = chip && (chip->direct_read || chip->direct_write);
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
    acc_info_build();
    wrdiv = 0xFFFF;
    rddiv = rdmpy = 0;
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
    frame_start = 0;
    frame_interlace = 0;
    frame_len = lines;
    odd_line = NO_LINE;
    odd_delta = 0;
    nmi_valid = nmi_rose = 0;
    nmi_from = begin_from = rendered = 1;
    hdma_from = 0;
    rose_line = hdma_line_of = NO_LINE;
    edge_done = setup_done = 0;
    dma_pending = hdma_pending = dma_active = 0;
    look_again = look_dma = look_mem = 0;
    memset(&turn, 0, sizeof turn);
    frames = vblanks = 0;
    interrupts_held = 0;
    allowance_set();
    irq_at = NEVER;
    ppu_reset();
    dma_reset();
    if (chip && chip->power)
        chip->power(&snes_cart);
    apu_reset();
    /* the first frame's shape (its field as the PPU starts), its
       deadlines, the first refresh */
    frame_shape();
    nmi_edge = nmi_next();
    refresh_next = refresh_of(0);
    events_next();
    regions_place();
    nshvc_cart_direct_changed();
}

/* ---- the cartridge connector's side (cart.h) ---- */
uint64_t nshvc_cart_clock(void)
{
    bus_settle();
    turn.step |= turn.on;
    return chip_ahead ? chip_to : snes_clock;
}
void nshvc_cart_irq_at(int level, uint64_t clock)
{
    /* (the inline accesses' limit 0: the next access looks) */
    bus_enter();
    snes_cart_irq(level, clock);
    /* run ahead: the chip stops where its IRQ rose */
    if (level && chip_ahead && clock < chip_to)
        chip_to = clock > snes_clock ? clock : snes_clock;
}
void nshvc_cart_irq(int level) { nshvc_cart_irq_at(level, nshvc_cart_clock()); }
uint8_t nshvc_cart_rom(uint32_t off, int who) { return snes_rom_byte(snes_rom_mirror(off), who); }

/* the chip's direct answers given (their regions), and the regions asked */
static uint16_t direct_list[2048];
static unsigned direct_n;
static uint8_t direct_asked[2048];

/* after an access the call made to the region of `addr`: the chip asked
   once, until it says its answers changed, whether the S-CPU's accesses
   there may be made directly (cart.h direct_read, direct_write) */
static void direct_ask(uint32_t addr)
{
    unsigned r = addr >> 13 & 0x7FF;
    nshvc_bus_region *g = &nshvc_bus.region[r];
    if (direct_asked[r] || g->rd || g->wr)
        return;
    /* (never over the console's own registers: $2000-$5FFF of banks $00-$3F
       and $80-$BF) */
    if (!(addr & 0x400000) && ((r & 7) == 1 || (r & 7) == 2))
        return;
    direct_asked[r] = 1;
    direct_list[direct_n++] = (uint16_t)r;
    if (chip->direct_read)
        g->rd = chip->direct_read(addr & 0xFFE000);
    if (chip->direct_write)
        g->wr = chip->direct_write(addr & 0xFFE000);
}

/* (the regions' pointers are what each inline access looks at: cleared,
   the next is the call) */
void nshvc_cart_direct_changed(void)
{
    while (direct_n) {
        unsigned r = direct_list[--direct_n];
        nshvc_bus.region[r].rd = NULL;
        nshvc_bus.region[r].wr = NULL;
        direct_asked[r] = 0;
    }
}
uint8_t nshvc_open_bus(void) { return snes_mdr; }
nshvc_coro *nshvc_coro_new(void (*fn)(void *), void *arg, size_t stack) { return (nshvc_coro *)coro_new(fn, arg, stack); }
nshvc_coro *nshvc_coro_current(void) { return (nshvc_coro *)coro_current(); }
void nshvc_coro_switch(nshvc_coro *from, nshvc_coro *to) { coro_switch((Coro *)from, (Coro *)to); }
