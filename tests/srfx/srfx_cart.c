/*
 * srfx_cart.c: Stunt Race FX's decompilation as a cartridge for nativeshvc,
 * the check that a real decomp plugs in unchanged: the decomp's C (its
 * src/stuntracefx, as it is), its routine tables, the player's ROM, and
 * this description. Nothing else.
 *
 *   srfx --rom ROM [nshvc_main's options]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/superfx.h"
#include "routines.h"

/* --pacing FILE: at each race frame's start, where the frames since the
   last one ($0306) go to $06D4: the video frame, the count, and hashes of
   work RAM (the stack, $0100-$02FF, left out) and of the Super FX's RAM,
   as Stunt Race FX's own port writes them (its same.py compares them) */
static FILE *pacing_f;

static uint32_t fnv(const uint8_t *p, size_t n, uint32_t h)
{
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

static void pacing_write(uint32_t addr, uint8_t value)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    const uint8_t *w = nshvc_wram();
    (void)value;
    if ((addr & 0xFFFF) != 0x06D4 || !(bank == 0x7E || (bank & 0x7F) < 0x40))
        return;
    fprintf(pacing_f, "%u %06X %u %08x %08x\n", nshvc_frames(), 0u, w[0x306],
            fnv(w + 0x300, 0x20000 - 0x300, fnv(w, 0x100, 2166136261u)), fnv(nshvc_sram(), 0x10000, 2166136261u));
}

/* --hashes FILE: at each frame's end, the frame, and hashes of work RAM
   (the stack left out) and the Super FX's RAM, as tests/oracle's --hashes
   (tests/srfx/first.py finds where a play first differs from them) */
static FILE *hashes_f;

static void hashes_frame(void)
{
    const uint8_t *w = nshvc_wram();
    fprintf(hashes_f, "%u %08x %08x\n", nshvc_frames(), fnv(w + 0x300, 0x20000 - 0x300, fnv(w, 0x100, 2166136261u)),
            fnv(nshvc_sram(), 0x10000, 2166136261u));
}

#ifdef SRFX_ASSETS
void srfx_assets(nshvc_cart *cart);
/* the port's cartridge: its remaster options looked at each frame's end
   (with no RM_OPT_* in the environment, everything as the original) */
void rm_frame_end(void);
#endif

int main(int argc, char **argv)
{
    static nshvc_cart cart;
    const char *path = NULL;
    static uint8_t *rom;
    FILE *f;
    long n;
    int i;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rom") && i + 1 < argc)
            path = argv[i + 1];
        if (!strcmp(argv[i], "--pacing") && i + 1 < argc) {
            if (!(pacing_f = fopen(argv[i + 1], "w"))) {
                fprintf(stderr, "srfx: cannot write %s\n", argv[i + 1]);
                return 2;
            }
            setvbuf(pacing_f, NULL, _IOLBF, 0);
            nshvc_set_write_hook(pacing_write);
        }
        if (!strcmp(argv[i], "--hashes") && i + 1 < argc) {
            if (!(hashes_f = fopen(argv[i + 1], "w"))) {
                fprintf(stderr, "srfx: cannot write %s\n", argv[i + 1]);
                return 2;
            }
            nshvc_set_frame_hook(hashes_frame);
        }
    }
#ifndef SRFX_ASSETS
    if (!path) {
        fprintf(stderr, "srfx: --rom ROM (Stunt Race FX (USA) (Rev 1))\n");
        return 2;
    }
    if (!(f = fopen(path, "rb"))) {
        fprintf(stderr, "srfx: cannot read %s\n", path);
        return 2;
    }
    rom = (uint8_t *)malloc(0x100000);
    n = (long)fread(rom, 1, 0x100000, f);
    fclose(f);
    if (n != 0x100000) {
        fprintf(stderr, "srfx: %s is not a 1 MB ROM\n", path);
        return 2;
    }
#else
    (void)path; (void)f; (void)n;
#endif
    cart.title = "Stunt Race FX";
    cart.save_name = "stuntracefx";
    cart.map = NSHVC_SUPERFX;
    cart.chip = &nshvc_superfx;    /* the cartridge's Super FX */
    cart.region = NSHVC_NTSC;
#ifdef SRFX_ASSETS
    srfx_assets(&cart);
#else
    cart.rom = rom;
    cart.rom_size = 0x100000;
#endif
    cart.sram_size = 0x10000;     /* the Super FX's RAM, battery backed */
    cart.battery = 1;
    cart.save_from = 0xFC00;      /* the records; the rest is the Super FX's work */
    cart.save_size = 0x400;
    cart.routines = port_routines;
    cart.routine_count = port_routine_count;
    cart.spc_routines = port_spc_routines;
    cart.spc_routine_count = port_spc_routine_count;
#ifdef SRFX_ASSETS
    {
        static const nshvc_hooks hooks = { .frame = rm_frame_end };
        nshvc_set_hooks(&hooks);
    }
#endif
    return nshvc_main(&cart, argc, argv);
}
