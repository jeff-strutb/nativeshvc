/*
 * snes.h: what the console's parts share (src/console, src/ppu, src/dsp,
 * src/front): the machine's time, its hooks and the calls between parts.
 * Not for games: a game sees nshvc.h and the machine model (machine.h,
 * spc.h).
 *
 * Time. The S-CPU's C takes none: the master clock (21.477 MHz) moves on
 * where the S-CPU waits for the hardware (machine.h nshvc_wait_until and
 * the waits beside it) and while a DMA stops it. The picture is drawn a
 * line at a time as that clock passes each line's horizontal blank, HDMA
 * runs there, and the interrupts the game asks for are raised when the
 * clock reaches them. The SPC700 (and a cartridge's chip, cart.h) runs its
 * C on a coroutine of its own with its own clock, and is brought up to the
 * S-CPU's time whenever the S-CPU could see what it does (the sound ports;
 * a chip's registers and RAM) and once a line; its C takes no time either,
 * and its clock moves on in its own waits (spc.h spc_wait_until).
 *
 * Interrupts are taken in a wait at the clock they come due, or after the
 * access whose DMA made them due (never between the two bytes of one word,
 * nor between a read and its write back).
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
/* Each S-CPU read, as it is made, with its value. */
extern void (*snes_read_hook)(uint32_t addr, uint8_t value);
/* Each byte a DMA or HDMA moves: A-bus address a, B-bus register $2100 + b. */
extern void (*snes_dma_hook)(uint32_t a, uint8_t b, uint8_t v);

/* The SPC700 runs the code at APU address addr (after the boot ROM's
   jump); never returns. */
extern void (*snes_spc_run_hook)(uint16_t addr);

/* vertical blanks begun since power on */
uint32_t snes_vblanks(void);

/* ---- input ---- */

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
/* a byte the interrupt sequence pushes */
void snes_interrupt_push(uint32_t addr, uint8_t value);
/* a read of the interrupt's return (RTI's pulls) */
uint8_t snes_cpu_read_within(uint32_t addr);
/* whether an interrupt is due: the NMI, or the IRQ line (masked or not) */
int snes_interrupt_due(void);
/* WAI: the clock on until an interrupt is due, then that one taken unless
   masked */
void snes_wai(void);
/* a branch to itself: the clock on, the interrupts taken, for ever */
void snes_loop_forever(void);

#ifdef __cplusplus
}
#endif
#endif
