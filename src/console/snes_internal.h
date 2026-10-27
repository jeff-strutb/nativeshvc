/*
 * snes_internal.h: what the parts of the console share beyond snes.h.
 */
#ifndef NSHVC_SNES_INTERNAL_H
#define NSHVC_SNES_INTERNAL_H

#include "snes.h"
#include "ppu.h"
#include "mixer.h"
#include "mixer_regs.h"

/* ---- bus.c ---- */
extern uint8_t snes_mdr;                    /* the data bus's last value (open bus) */
/* one byte on the A bus for DMA (no CPU overhead, no interrupt) */
uint8_t bus_read_a(uint32_t addr);
void bus_write_a(uint32_t addr, uint8_t v);
/* one byte on the B bus ($21xx) */
uint8_t bus_read_b(uint8_t reg);
void bus_write_b(uint8_t reg, uint8_t v);
/* whether an A-bus address is a register the B bus or the CPU answers
   (DMA reaches none of them) */
int bus_a_is_register(uint32_t addr);
/* the clock moves on (the CPU or DMA busy) */
void snes_tick(unsigned clocks);
/* the IRQ line from the cartridge (its chip: cart.h nshvc_cart_irq) */
void snes_cart_irq(int level);

uint8_t snes_wrio(void);                     /* WRIO ($4201): the ports' IOBit pins */

/* ---- ports.c: the controller ports' devices ---- */
void ports_reset(const int *ports);          /* power on: what the cartridge has plugged in */
void ports_latch(int level);                 /* the latch line both ports share */
unsigned ports_data(int port);               /* a clock on the port: data line 1, line 2 in bit 1 */
void ports_frame(void);                      /* line vdisp begins: port 2's light gun latches */

/* ---- dma.c ---- */
void dma_reset(void);
uint8_t dma_read(uint16_t reg);
void dma_write(uint16_t reg, uint8_t v);
void dma_run(void);                          /* MDMAEN's transfers (dma_mdmaen's channels) */
extern uint8_t dma_mdmaen;                   /* the channels whose general DMA is waiting or under way */
void dma_edge(void);                         /* inside a DMA: an edge of the unit (bus.c), where HDMA comes in */
void hdma_init(void);                        /* the frame's start */
void hdma_line(void);                        /* each line's horizontal blank */
int hdma_enabled(void);                      /* a channel set in HDMAEN */
int hdma_active(void);                       /* one whose table has not ended */
extern unsigned dma_clocks;                  /* the unit's clocks since it took the bus */
extern uint8_t dma_hdmaen;


/* ---- apu.c ---- */
void apu_reset(void);
uint8_t apu_port_read(uint8_t port);         /* the S-CPU reads $2140-$2143 */
void apu_port_write(uint8_t port, uint8_t v);
void apu_sync(void);                         /* the SPC700 up to the S-CPU's time */
uint8_t apu_debug_port(int n);               /* what the S-CPU last wrote to port n */
void apu_debug(char *out, int n);            /* the SPC700's clock and last access */
uint8_t *apu_aram_rw(void);                  /* APU RAM, written with no access (a unit call's stub) */

#endif
