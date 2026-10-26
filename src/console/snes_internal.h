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
/* the IRQ line from the cartridge (the Super FX's STOP) */
void snes_cart_irq(int level);

/* ---- dma.c ---- */
void dma_reset(void);
uint8_t dma_read(uint16_t reg);
void dma_write(uint16_t reg, uint8_t v);
void dma_start(uint8_t channels);            /* MDMAEN */
void hdma_init(void);                        /* the frame's start */
void hdma_line(void);                        /* each line's horizontal blank */
extern uint8_t dma_hdmaen;

/* ---- superfx.c ---- */
void gsu_reset(void);
uint8_t gsu_reg_read(uint16_t addr);         /* $3000-$32FF */
void gsu_reg_write(uint16_t addr, uint8_t v);
void gsu_sync(void);                         /* the Super FX up to the S-CPU's time */
int gsu_running(void);
extern uint8_t *gsu_ram_mem;
extern uint32_t gsu_ram_size;
extern uint8_t gsu_scmr_ron, gsu_scmr_ran;   /* the Super FX holds ROM, its RAM */

/* ---- apu.c ---- */
void apu_reset(void);
uint8_t apu_port_read(uint8_t port);         /* the S-CPU reads $2140-$2143 */
void apu_port_write(uint8_t port, uint8_t v);
void apu_sync(void);                         /* the SPC700 up to the S-CPU's time */
uint8_t apu_debug_port(int n);               /* what the S-CPU last wrote to port n */
void apu_debug(char *out, int n);            /* the SPC700's clock and last access */

#endif
