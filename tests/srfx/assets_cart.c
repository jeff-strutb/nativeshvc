/*
 * assets_cart.c: Stunt Race FX's cartridge from its named assets (its
 * ports/stuntracefx/tools/assets.py: gen/assets.c, port_assets[]), for a
 * build with no ROM image: the ROM's data blocks at their offsets, and the
 * code the C stands for nowhere. A byte no asset holds reads 0 where the
 * Super FX's ROM buffer fills from it (it may never be used: nshvc.h
 * NSHVC_ROM_FETCH); anywhere else it stops the run.
 */
#include <stdlib.h>

#include "nshvc/nshvc.h"
#include "cart.h"

static const PortAsset *find(uint32_t off)
{
    int lo = 0, hi = port_asset_count - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        const PortAsset *a = &port_assets[m];
        if (off < a->rom)
            hi = m - 1;
        else if (off >= a->rom + a->size)
            lo = m + 1;
        else
            return a;
    }
    return NULL;
}

static uint8_t asset_read(uint32_t off, int who)
{
    const PortAsset *a = find(off);
    if (a)
        return a->data[off - a->rom];
    if (who & NSHVC_ROM_FETCH) {
        nshvc_rom_unheld = 1;
        return 0;
    }
    if (who & NSHVC_ROM_PEEK)
        return 0;
    nshvc_fatal("ROM $%06X read (by %s): no asset holds it (code the C stands for)", (unsigned)off,
                who & NSHVC_ROM_DMA ? "DMA" : who & NSHVC_ROM_GSU ? "the Super FX" : "the S-CPU");
    return 0;
}

/* each page one asset holds whole, for reads with no lookup */
void srfx_assets(nshvc_cart *cart)
{
    static const uint8_t **pages;
    uint32_t n = port_rom_size >> 8, p;
    pages = (const uint8_t **)calloc(n, sizeof *pages);
    if (!pages)
        nshvc_fatal("no memory for the cartridge's pages");
    for (p = 0; p < n; p++) {
        const PortAsset *a = find(p << 8);
        if (a && a->rom + a->size >= (p << 8) + 256)
            pages[p] = a->data + ((p << 8) - a->rom);
    }
    cart->rom = NULL;
    cart->rom_pages = pages;
    cart->rom_read = asset_read;
    cart->rom_size = port_rom_size;
}

/* the game's touch-driving hook: nothing to do on a headless check */
void port_controls_read(int player) { (void)player; }
