/*
 * superfx.h: the Super FX (GSU-1), an expansion chip on the cartridge, as
 * a library a Super FX cartridge links (libnshvc_superfx): nshvc_cart.map
 * NSHVC_SUPERFX and .chip &nshvc_superfx. Its registers at $3000-$32FF and
 * its RAM at $70-$7D (32 KB to 128 KB mirrored through $70-$71, more
 * nshvc_cart.sram_size in the banks after) are its own; the C of its
 * programs uses gsu.h.
 */
#ifndef NSHVC_SUPERFX_H
#define NSHVC_SUPERFX_H

#include "cart.h"

extern const nshvc_chip nshvc_superfx;

/* The Super FX past its own: its screen width x height pixels (8 to 1024
   by 8 to 512, multiples of 8; 0, 0 the GSU's own, the default), plotted
   at 16-bit coordinates (gsu.h gsu_plot_at, gsu_rpix_at; gsu_plot and
   gsu_rpix the same at 8 bits), laid out as the GSU lays its own: 8x8
   characters, each its bit planes in pairs of bytes, in columns of
   height / 8 characters, down then across, from SCBR's 1 KB on, past bank
   $71 into the RAM after it. Its RAM: nshvc_cart.sram_size past 128 KB
   (up to 896 KB, in 64 KB banks) is at $70-$7D on the S-CPU's bus and the
   Super FX's. */
void nshvc_ext_gsu_screen(int width, int height);

#endif
