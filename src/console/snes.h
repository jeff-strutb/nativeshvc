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
 * the game asks for are raised when the clock reaches them. The Super FX
 * and the SPC700 run their C on coroutines of their own, each with its own
 * clock, and are brought up to the S-CPU's time whenever the S-CPU could
 * see what they do (their registers, their RAM, the sound ports) and once
 * a line.
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
/* Each pixel the Super FX's PLOT writes, its colour after the plot rules. */
extern void (*snes_gsu_plot_hook)(uint8_t x, uint8_t y, uint8_t c);
/* Each byte a DMA or HDMA moves: A-bus address a, B-bus register $2100 + b. */
extern void (*snes_dma_hook)(uint32_t a, uint8_t b, uint8_t v);

/* The Super FX starts a program at bank:addr (the S-CPU wrote R15): its C
   runs, and returns at the program's STOP. */
extern void (*snes_gsu_run_hook)(uint8_t bank, uint16_t addr);
/* Whether the program at bank:addr starts with CACHE (its timing). */
extern int (*snes_gsu_cache_first_hook)(uint8_t bank, uint16_t addr);
/* The SPC700 runs the code at APU address addr (after the boot ROM's
   jump); never returns. */
extern void (*snes_spc_run_hook)(uint16_t addr);

/* ---- untimed (nshvc_set_untimed) ---- */
extern int snes_untimed_on;
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
const uint8_t *snes_wram(void);
uint8_t *snes_wram_rw(void);

/* ---- for the dispatcher ---- */
void snes_fatal(const char *fmt, ...);
uint8_t snes_peek(uint32_t addr);
/* the clocks one S-CPU access to addr takes on the bus */
unsigned snes_access_clocks(uint32_t addr);
/* the place between two instructions where an interrupt may be taken */
void snes_cpu_interrupts(void);
/* a byte the interrupt sequence pushes: a write where no interrupt is taken */
void snes_interrupt_push(uint32_t addr, uint8_t value);
/* whether an interrupt is due: the NMI, or the IRQ line (masked or not) */
int snes_interrupt_due(void);
/* clocks with no access (a delay loop), interrupts taken along the way */
void snes_cpu_wait(uint32_t clocks);
/* time the S-CPU spends between two of its accesses: the decomp's cyc() */
void snes_cpu_clocks(uint32_t clocks);

#ifdef __cplusplus
}
#endif
#endif
