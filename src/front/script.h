/*
 * script.h: a headless run's script (script.c): steps of buttons held for
 * frames, shots and dumps, from power on.
 *
 *   wait N               N frames, no buttons
 *   hold B[+B...] [N]    hold buttons for N frames (default 1)
 *   press B[+B...] [N]   hold for N frames (default 2), then 2 released
 *   shot NAME            the finished frame as DIR/NAME.ppm
 *   dump NAME            work RAM as DIR/NAME.wram, the cartridge's RAM as
 *                        DIR/NAME.sram, the PPU's memories as DIR/NAME.vram,
 *                        .cgram and .oam
 *   repeat N ... end     the lines between, N times (nests)
 *
 * Buttons: B Y SELECT START UP DOWN LEFT RIGHT A X L R, player 1; 2. before
 * one for player 2 (2.START). '#' starts a comment.
 */
#ifndef NSHVC_SCRIPT_H
#define NSHVC_SCRIPT_H

#include <stdint.h>

void script_load(const char *path, const char *out_dir);
/* the steps loaded (0: no script) */
int script_steps(void);
/* the shots and dumps due now, at the end of a frame */
void script_takes(void);
/* the next frame's buttons from the script; 0 when it has ended */
int script_frame(uint16_t *b0, uint16_t *b1);

#endif
