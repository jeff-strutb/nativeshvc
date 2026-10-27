/*
 * cart.h: the cartridge connector. A cartridge's own hardware (an expansion
 * chip: the Super FX, the SA-1, a DSP...) is the cartridge's, as on the
 * console: nativeshvc implements none of it. It gives a chip what the
 * connector gives one (the S-CPU's accesses to the addresses the chip
 * decodes, the master clock, the IRQ line, the ROM) and the chip gives the
 * console its answers. A cartridge with one puts it in nshvc_cart.chip: the
 * game brings the chip, its registers, its RAM, its board's decoding (with
 * nshvc_cart.map NSHVC_CHIP) and its programs, and owns them.
 */
#ifndef NSHVC_CART_H
#define NSHVC_CART_H

#include <stddef.h>
#include <stdint.h>

#include "nshvc.h"

/* An expansion chip, as the console's bus sees it. Every member is
   optional (NULL). */
struct nshvc_chip {
    const char *name;
    /* power on, before the S-CPU starts: the cartridge it is on */
    void (*power)(const nshvc_cart *cart);
    /* An S-CPU access to an address the chip decodes (bank:offset, 24
       bits): 1 when the chip answers it (*v the byte read), 0 when it is the
       console's or the cartridge's ROM or RAM as nshvc_cart.map lays them
       out. Asked before the map for every address outside the console's
       own (work RAM, the B bus, the CPU's registers). peek: the same with
       no effect (a debugger's, the front end's). */
    int (*read)(uint32_t addr, uint8_t *v);
    int (*write)(uint32_t addr, uint8_t v);
    int (*peek)(uint32_t addr, uint8_t *v);
    /* With nshvc_cart.map NSHVC_CHIP, the board's ROM decoding: 1 when the
       S-CPU's address (bank:offset, 24 bits) reaches the cartridge's ROM,
       *off its file offset; 0 otherwise. The console then reads the ROM
       there itself (nshvc_cart.rom, rom_pages or rom_read), for the S-CPU,
       DMA and a look alike, as it does a mapping's. */
    int (*rom_at)(uint32_t addr, uint32_t *off);
    /* the chip brought up to the console's clock (nshvc_cart_clock): made
       before the S-CPU's accesses to it and once a line */
    void (*sync)(void);
    /* interrupts held (nshvc_hold_interrupts) or released: what the chip
       keeps of its interrupt across a hold, so code an app runs held leaves
       the game's handlers nothing to take */
    void (*hold)(int on);
    /* the cartridge RAM the chip owns, when it owns it (its size in *size):
       saves, nshvc_sram, dumps */
    uint8_t *(*ram)(uint32_t *size);
    /* for NSHVC_DEBUG: whether it is running */
    int (*busy)(void);
};

/* ---- what the connector gives a chip ---- */
/* the master clock: clocks since power on (21.477 MHz NTSC) */
uint64_t nshvc_cart_clock(void);
/* the cartridge's IRQ line, driven by the chip (1 asserted) */
void nshvc_cart_irq(int level);
/* a byte of the cartridge's ROM at file offset `off` (mirrored as the
   console decodes a size that is not a power of two); `who`: NSHVC_ROM_* */
uint8_t nshvc_cart_rom(uint32_t off, int who);
/* the data bus's open value (what a read the chip leaves unanswered sees) */
uint8_t nshvc_open_bus(void);
/* the cartridge's routine with C at bank:address (nshvc_cart.routines), or
   NULL */
const nshvc_routine *nshvc_cart_routine(uint32_t addr);

/* Coroutines, for a chip whose C runs beside the S-CPU's (each on a stack
   of its own): the S-CPU's code calls sync, the chip's runs until it
   reaches the console's clock and switches back. */
typedef struct nshvc_coro nshvc_coro;
nshvc_coro *nshvc_coro_new(void (*fn)(void *), void *arg, size_t stack);
nshvc_coro *nshvc_coro_current(void);
void nshvc_coro_switch(nshvc_coro *from, nshvc_coro *to);

#endif
