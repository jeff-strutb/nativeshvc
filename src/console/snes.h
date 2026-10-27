/*
 * snes.h: what the console's parts share (src/console, src/ppu, src/dsp,
 * src/front): the machine's time, its hooks and the calls between parts.
 * Not for games: a game sees nshvc.h and the machine model (machine.h,
 * gsu.h, spc.h).
 *
 * Time. Each access the S-CPU's C makes takes the time that access takes on
 * the bus, and between two accesses the clock moves on by the time the C
 * states (cyc: the original's fetches and internal cycles), in master
 * clocks (21.477 MHz). The picture is drawn a line at a time as that clock
 * passes each line's horizontal blank, HDMA runs there, and the interrupts
 * the game asks for are raised when the clock reaches them. The SPC700 (and
 * a cartridge's chip, cart.h) runs its C on a coroutine of its own with its
 * own clock, and is brought up to the S-CPU's time whenever the S-CPU could
 * see what it does (the sound ports; a chip's registers and RAM) and once a
 * line.
 *
 * Interrupts are taken where the S-CPU's C reads or writes the bus (never
 * between the two bytes of one word, nor between a read and its write
 * back), as an instruction boundary is the only place the hardware takes
 * them.
 */
#ifndef NSHVC_SNES_H
#define NSHVC_SNES_H

#include <stdint.h>
#include <stddef.h>

#include "nshvc/nshvc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNES_MASTER_HZ_NTSC 21477272.727
#define SNES_MASTER_HZ_PAL  21281370.0
#define SNES_LINE_CLOCKS    1364
#define SNES_LINES_NTSC     262
#define SNES_LINES_PAL      312
#define SNES_POWER_CLOCK    182     /* the clock in line 0 the CPU starts at */

/* ---- the cartridge ---- */
extern nshvc_cart snes_cart;

static inline uint8_t snes_rom_byte(uint32_t off, int who)
{
    if (snes_cart.rom)
        return snes_cart.rom[off];
    {
        const uint8_t *p = snes_cart.rom_pages ? snes_cart.rom_pages[off >> 8] : NULL;
        return p ? p[off & 0xFF] : snes_cart.rom_read(off, who);
    }
}
/* a ROM offset as the cartridge's decoding mirrors one past its size */
uint32_t snes_rom_mirror(uint32_t off);

/* ---- time ---- */
extern uint64_t snes_clock;            /* master clocks since power on */
uint32_t snes_frame(void);             /* frames begun since power on */
unsigned snes_vcounter(void);
unsigned snes_hcounter(void);          /* dots, 0-339 */
uint32_t snes_line_clock(void);        /* master clocks since the line began */
unsigned snes_lines(void);             /* lines a frame: 262 or 312 */
int snes_pal(void);
/* the line the vertical blank starts at: 225, or 240 with overscan */
unsigned snes_vblank_line(void);
unsigned snes_vdisp(void);

/* ---- hooks the front end and the dispatcher set ---- */
/* An interrupt is due (nmi 1: the NMI; else the IRQ line is asserted and the
   CPU's I flag clear): take it. Called from inside an S-CPU access, before
   the access. */
extern void (*snes_interrupt_hook)(int nmi);
/* Whether the CPU masks the IRQ (its I flag): read where an IRQ is due. */
extern int (*snes_irq_masked)(void);
/* A frame's picture is finished (the start of the vertical blank). Called
   from inside an access. */
extern void (*snes_frame_hook)(void);
/* Each write by the S-CPU's C, before it lands. */
extern void (*snes_write_hook)(uint32_t addr, uint8_t value);
/* Each byte a DMA or HDMA moves: A-bus address a, B-bus register $2100 + b. */
extern void (*snes_dma_hook)(uint32_t a, uint8_t b, uint8_t v);

/* The SPC700 runs the code at APU address addr (after the boot ROM's
   jump); never returns. */
extern void (*snes_spc_run_hook)(uint16_t addr);

/* ---- untimed (nshvc_set_untimed) ---- */
extern int snes_untimed_on;
int snes_untimed(void);
extern int snes_in_interrupt;              /* an interrupt handler runs (timed even untimed) */
/* vertical blanks begun since power on */
uint32_t snes_vblanks(void);

/* ---- input ---- */
void snes_set_joypad(int port, uint16_t buttons);

/* ---- sound ---- */
int snes_audio_take(int16_t *lr, int max);
int snes_audio_queued(void);
const uint8_t *snes_aram(void);

/* ---- power ---- */
void snes_power(void);
uint8_t *snes_sram(void);
uint32_t snes_sram_size(void);           /* its size (the chip's when the chip owns it) */
const uint8_t *snes_wram(void);
uint8_t *snes_wram_rw(void);

/* ---- for the dispatcher ---- */
void snes_fatal(const char *fmt, ...);
uint8_t snes_peek(uint32_t addr);
/* the clocks one S-CPU access to addr takes on the bus */
unsigned snes_access_clocks(uint32_t addr);
/* an instruction's end (machine.h's cpu_boundary, or one of an
   instruction the console runs): the only place an interrupt is taken, the
   one the CPU decided on as the instruction's last cycle began */
void snes_cpu_boundary(void);
/* a byte the interrupt sequence pushes (NSHVC_CYCLOG's P line) */
void snes_interrupt_push(uint32_t addr, uint8_t value);
/* a read or write of an instruction the console runs (a pull, a store):
   not audited */
uint8_t snes_cpu_read_within(uint32_t addr);
void snes_cpu_write_within(uint32_t addr, uint8_t value);
/* whether an interrupt is due: the NMI, or the IRQ line (masked or not) */
int snes_interrupt_due(void);
/* an interrupt would be taken now (due, and the IRQ not masked) */
int snes_interrupt_taken_now(void);
/* clocks with no access and no instruction end (cpu_idle) */
void snes_cpu_wait(uint32_t clocks);
/* time the S-CPU spends between two of its accesses: the decomp's cyc() */
void snes_cpu_clocks(uint32_t clocks);
void snes_cpu_stated(uint32_t clocks, void *site);   /* cyc from the C's `site` (NSHVC_AUDIT) */
void snes_cpu_cycle(unsigned cc);                     /* one S-CPU cycle of cc clocks (cycs) */
void snes_cyclog_cycs(void *site, uint64_t lengths);  /* NSHVC_CYCLOG's cycs line */
void snes_cyclog_mark(uint32_t id);                   /* NSHVC_CYCLOG's M line (nshvc_mark) */
extern int snes_cyclog_quiet;                         /* NSHVC_CYCLOG writes nothing while set */
extern uint32_t (*snes_cyclog_frame)(void);           /* its frame number (default: frames run + 1) */
void snes_cpu_cycle_k(unsigned cc);                   /* a cycle the console states for the C */

/* ---- unit calls (the script's call and fx, src/front/script.c) ---- */
/* A routine run once as a unit test from the state the game has reached,
   as the reference's harness runs one: at the first place between two of
   the S-CPU's instructions where it is not in an interrupt handler,
   untimed, or in the vertical blank (unless `vblank`), and the cartridge's
   chip is stopped, the CPU runs a stub in work RAM ($00:1FE0; $7F:FFE0 for
   a JSR into bank $7F; written when the call is made): a JSR (a JSL with
   `lng`) to the routine, then a BRA to itself, with the registers as they
   were when the call was made but those it names, S as it is, native
   mode. The routine's C runs from there; the stub's own instructions the
   console runs, cycle by cycle. */
typedef struct {
    uint32_t addr;
    int lng, keep, vblank, php, phb;
    long frame, rti;                 /* frame=PPDD, rti=PP; -1 none */
    int has_a, has_x, has_y, has_p, has_db, has_d;
    uint16_t a, x, y, d;
    uint8_t p, db;
    void (*apply)(void);             /* the staged bytes, written as it starts */
} SnesUnit;
/* a call to start at the next place allowed (the stub checked now: a JSR
   cannot reach every bank) */
void snes_unit_call(const SnesUnit *u);
/* 0 none, 1 waiting to start, 2 under way, 3 returned (the machine parked in
   the stub's loop, or with keep the game gone on) */
int snes_unit_state(void);
/* a call that has not started is dropped */
void snes_unit_cancel(void);
extern int snes_unit_pending;
void snes_unit_start(void);          /* at a boundary while one is pending */

/* An SPC700 unit call (the script's spcset and spccall): when the driver's
   C next marks the instruction at `at` (spc.h spc_at), the routine at
   `target` is called from a stub at $01F0 (CALL, then BRA to itself) with
   the registers given and the staged bytes in APU RAM; when it returns, the
   registers and the stub's bytes are put back and the driver goes on at
   `at`. */
void snes_spc_unit_set(uint16_t addr, uint8_t v);
void snes_spc_unit_call(uint16_t target, uint16_t at, const int *has, const uint8_t *reg);
int snes_spc_unit_busy(void);         /* 0 none, 1 waiting for `at`, 2 under way */

#ifdef __cplusplus
}
#endif
#endif
