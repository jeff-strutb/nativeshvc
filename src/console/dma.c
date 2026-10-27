/*
 * dma.c: the CPU's eight DMA channels, as general DMA (MDMAEN) and HDMA.
 *
 * General DMA moves bytes between the A bus (any address but the
 * registers) and one B-bus register ($21xx) in the channel's pattern, the
 * CPU stopped meanwhile (the clock and the lines it passes go on). HDMA
 * runs in each line's horizontal blank, lines 0-224: each channel's table
 * gives a line count (bit 7: transfer every line of the run, else only its
 * first) and the bytes, inline or (indirect) through an address in the
 * table. The unit's time: 8 clocks to start, 8 for each channel a DMA
 * runs, 8 a byte moved or read from a table (its access 4 clocks in); the
 * bus (bus.c) puts it between two of the CPU's cycles and aligns it.
 */
#include <string.h>

#include "snes_internal.h"

typedef struct {
    uint8_t dmap, bbad;
    uint16_t a1t;          /* A-bus address */
    uint8_t a1b;           /* its bank */
    uint16_t das;          /* byte count; HDMA's indirect address */
    uint8_t dasb;          /* HDMA's indirect bank */
    uint16_t a2a;          /* HDMA's table address */
    uint8_t ntrl;          /* HDMA's line counter */
    uint8_t unused;
    int done, transfer;    /* HDMA: the table has ended; this line transfers */
} Channel;

static Channel ch[8];
uint8_t dma_hdmaen;
uint8_t dma_mdmaen;
unsigned dma_clocks;       /* the unit's clocks since it took the bus */

static void dma_tick(unsigned clocks)
{
    dma_clocks += clocks;
    snes_tick(clocks);
}

/* a byte of an HDMA table: its read, 4 clocks into its 8 */
static uint8_t table_read(uint32_t a)
{
    uint8_t v;
    dma_tick(4);
    v = bus_read_a(a);
    dma_tick(4);
    return v;
}

/* the B-bus offsets of each transfer mode's bytes */
static const uint8_t pattern[8][4] = {
    { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 1, 1 },
    { 0, 1, 2, 3 }, { 0, 1, 0, 1 }, { 0, 0, 0, 0 }, { 0, 0, 1, 1 },
};
static const uint8_t pattern_len[8] = { 1, 2, 2, 4, 4, 4, 2, 4 };

void dma_reset(void)
{
    int i;
    memset(ch, 0xFF, sizeof ch);
    for (i = 0; i < 8; i++) {
        ch[i].done = 1;
        ch[i].transfer = 0;
    }
    dma_hdmaen = 0;
}

uint8_t dma_read(uint16_t reg)
{
    Channel *c = &ch[(reg >> 4) & 7];
    switch (reg & 0x0F) {
    case 0x0: return c->dmap;
    case 0x1: return c->bbad;
    case 0x2: return (uint8_t)c->a1t;
    case 0x3: return (uint8_t)(c->a1t >> 8);
    case 0x4: return c->a1b;
    case 0x5: return (uint8_t)c->das;
    case 0x6: return (uint8_t)(c->das >> 8);
    case 0x7: return c->dasb;
    case 0x8: return (uint8_t)c->a2a;
    case 0x9: return (uint8_t)(c->a2a >> 8);
    case 0xA: return c->ntrl;
    case 0xB: case 0xF: return c->unused;
    }
    return snes_mdr;
}

void dma_write(uint16_t reg, uint8_t v)
{
    Channel *c = &ch[(reg >> 4) & 7];
    switch (reg & 0x0F) {
    case 0x0: c->dmap = v; return;
    case 0x1: c->bbad = v; return;
    case 0x2: c->a1t = (uint16_t)((c->a1t & 0xFF00) | v); return;
    case 0x3: c->a1t = (uint16_t)((c->a1t & 0x00FF) | v << 8); return;
    case 0x4: c->a1b = v; return;
    case 0x5: c->das = (uint16_t)((c->das & 0xFF00) | v); return;
    case 0x6: c->das = (uint16_t)((c->das & 0x00FF) | v << 8); return;
    case 0x7: c->dasb = v; return;
    case 0x8: c->a2a = (uint16_t)((c->a2a & 0xFF00) | v); return;
    case 0x9: c->a2a = (uint16_t)((c->a2a & 0x00FF) | v << 8); return;
    case 0xA: c->ntrl = v; return;
    case 0xB: case 0xF: c->unused = v; return;
    }
}

/* each byte a DMA or HDMA moves (snes.h) */
void (*snes_dma_hook)(uint32_t a, uint8_t b, uint8_t v);

/* one byte between A-bus address `a` and B-bus register `b`, the way the
   channel's direction says (DMAP bit 7: from the B bus to the A bus): 8
   clocks, the read 4 in and the write at the end */
static void move(const Channel *c, uint32_t a, uint8_t b)
{
    uint8_t v;
    dma_tick(4);
    if (c->dmap & 0x80) {
        v = bus_read_b(b);
        snes_mdr = v;
        dma_tick(4);
        if (snes_dma_hook)
            snes_dma_hook(a, b, v);
        bus_write_a(a, v);
        return;
    }
    v = bus_read_a(a);
    dma_tick(4);
    if (snes_dma_hook)
        snes_dma_hook(a, b, v);
    bus_write_b(b, v);
}

/* MDMAEN's channels in turn; the unit's edges inside (dma_edge), after its
   start and each byte, where HDMA comes in, which stops a DMA on its own
   channel */
void dma_run(void)
{
    int i;
    dma_tick(8);
    dma_edge();
    for (i = 0; i < 8; i++) {
        Channel *c = &ch[i];
        uint8_t bit = (uint8_t)(1u << i);
        unsigned k = 0, mode;
        if (!(dma_mdmaen & bit))
            continue;
        mode = c->dmap & 7;
        dma_tick(8);
        dma_edge();
        do {
            move(c, (uint32_t)c->a1b << 16 | c->a1t, (uint8_t)(c->bbad + pattern[mode][k & 3]));
            k++;
            /* bits 3-4: fixed (1, 3), decrement (2) or increment (0) */
            if (!(c->dmap & 0x08))
                c->a1t = (uint16_t)(c->a1t + ((c->dmap & 0x10) ? -1 : 1));
            dma_edge();
        } while ((dma_mdmaen & bit) && --c->das);
        dma_mdmaen &= (uint8_t)~bit;
    }
}

int hdma_enabled(void) { return dma_hdmaen != 0; }

int hdma_active(void)
{
    int i;
    for (i = 0; i < 8; i++)
        if (!ch[i].done && (dma_hdmaen & (1u << i)))
            return 1;
    return 0;
}

/* the channels after channel i have all ended */
static int later_done(int i)
{
    for (i++; i < 8; i++)
        if (!ch[i].done && (dma_hdmaen & (1u << i)))
            return 0;
    return 1;
}

/* channel c's next run: its line count and (indirect) address, read from
   its table; none read when the table has ended and no channel after
   this one is left (the unit stops) */
static void reload(Channel *c, int i)
{
    c->ntrl = table_read((uint32_t)c->a1b << 16 | c->a2a);
    c->a2a++;
    c->done = c->ntrl == 0;
    if (c->dmap & 0x40) {
        uint8_t lo, hi;
        lo = table_read((uint32_t)c->a1b << 16 | c->a2a);
        c->a2a++;
        if (c->done && later_done(i)) {
            c->das = (uint16_t)(lo << 8);
            return;
        }
        hi = table_read((uint32_t)c->a1b << 16 | c->a2a);
        c->a2a++;
        c->das = (uint16_t)(lo | hi << 8);
    }
}

void hdma_init(void)
{
    int i;
    dma_tick(8);
    for (i = 0; i < 8; i++) {
        Channel *c = &ch[i];
        c->done = 1;
        c->transfer = 0;
        if (!(dma_hdmaen & (1u << i)))
            continue;
        dma_mdmaen &= (uint8_t)~(1u << i);      /* HDMA stops a DMA on its channel */
        c->a2a = c->a1t;
        c->done = 0;
        reload(c, i);
        c->transfer = !c->done;
    }
}

void hdma_line(void)
{
    int i;
    dma_tick(8);
    for (i = 0; i < 8; i++) {
        Channel *c = &ch[i];
        unsigned k, mode;
        if (c->done || !(dma_hdmaen & (1u << i)))
            continue;
        dma_mdmaen &= (uint8_t)~(1u << i);      /* HDMA stops a DMA on its channel */
        if (!c->transfer)
            continue;
        mode = c->dmap & 7;
        for (k = 0; k < pattern_len[mode]; k++) {
            uint8_t b = (uint8_t)(c->bbad + pattern[mode][k]);
            if (c->dmap & 0x40) {
                move(c, (uint32_t)c->dasb << 16 | c->das, b);
                c->das++;
            } else {
                move(c, (uint32_t)c->a1b << 16 | c->a2a, b);
                c->a2a++;
            }
        }
    }
    for (i = 0; i < 8; i++) {
        Channel *c = &ch[i];
        if (c->done || !(dma_hdmaen & (1u << i)))
            continue;
        c->ntrl--;
        c->transfer = (c->ntrl & 0x80) != 0;
        if (!(c->ntrl & 0x7F)) {
            reload(c, i);
            c->transfer = !c->done;
        } else {
            /* the unit reads the table's next byte every line, the line
               count's own when it is due and in vain otherwise */
            table_read((uint32_t)c->a1b << 16 | c->a2a);
        }
    }
}
