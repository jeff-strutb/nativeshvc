/*
 * script.h: a headless run's script (script.c): steps of buttons held for
 * frames, shots and dumps, from power on.
 *
 *   wait N               N frames, no buttons
 *   hold B[+B...] [N]    hold buttons for N frames (default 1)
 *   press B[+B...] [N]   hold for N frames (default 2), then 2 released
 *   shot NAME            the finished frame as DIR/NAME.ppm (the console's
 *                        picture; at NSHVC_SCALE the presented one:
 *                        console.c front_shot_picture)
 *   dump NAME            work RAM as DIR/NAME.wram, the cartridge's RAM as
 *                        DIR/NAME.sram, the PPU's memories as DIR/NAME.vram,
 *                        .cgram and .oam
 *   repeat N ... end     the lines between, N times (nests)
 *   save NAME            the machine as it is, for a load
 *   load PATH            the machine as state NAME (PATH's file name less
 *                        .state) left it: saved by this run, else replayed
 *                        from power-on, by the script's own steps to its
 *                        save NAME or by the play in NSHVC_STATES that has one
 *   plug PORT DEVICE     plug joypad, none, mouse, multitap, superscope,
 *                        justifier or justifiers into port 0 or 1
 *   pad N BITS           player N's joypad holds BITS (hex, the joypad's
 *                        own: B 8000) from now; players 0 and 1 are hold's
 *   mouse PORT DX DY B   the mouse's motion and buttons the next frame
 *   gun G X Y B          light gun G points at dot X, line Y, with buttons
 *                        B (nshvc.h NSHVC_GUN_*), from now
 *   dump NAME also writes the cartridge chip's own RAM as DIR/NAME.chipram.
 * save and load fork, or, without fork, go on in a new copy of the program
 * that replays to the machine wanted (script.c).
 *
 * Buttons: B Y SELECT START UP DOWN LEFT RIGHT A X L R, player 1; 2. before
 * one for player 2 (2.START). '#' starts a comment.
 */
#ifndef NSHVC_SCRIPT_H
#define NSHVC_SCRIPT_H

#include <stdint.h>

void script_load(const char *path, const char *out_dir);
/* the program's arguments, for a run that goes on in a new copy of the
   program (save, load and calls without keep, without fork) */
void script_args(int argc, char **argv);
/* replaying steps to rebuild a machine (nothing they make is written) */
int script_replaying(void);
/* the steps loaded (0: no script) */
int script_steps(void);
/* the shots and dumps due now, at the end of a frame */
void script_takes(void);
/* the next frame's buttons from the script; 0 when it has ended */
int script_frame(uint16_t *b0, uint16_t *b1);

/* the picture a shot holds (console.c): nshvc_picture's, or with
   NSHVC_SCALE the frame presented at that scale (*presented 1) */
const uint32_t *front_shot_picture(int *w, int *h, int *presented);

#endif
