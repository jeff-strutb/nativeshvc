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
 * And the steps of Stunt Race FX's harness (sfxbox), as it runs them:
 *   poke ADDR BYTE[,BYTE...]   bytes into work RAM ($7E-$7F, the low 8 KB in
 *                        banks $00-$3F) or the chip's RAM ($70-$71), now
 *   set ADDR BYTE[,BYTE...]    the same, written when the next call starts
 *   call ADDR [long] [keep] [noreturn] [a= x= y= p= db= d=] [frames=N]
 *        [press=B@F] [frame=PPDD] [php] [phb] [rti=PP] [vblank]
 *                        the routine run once from a stub in work RAM
 *                        (snes.h SnesUnit), over up to N frames (600); then
 *                        the machine as it was before (keep: the game goes
 *                        on from where it was, the call's changes kept;
 *                        noreturn: one that does not return in its frames
 *                        is left running)
 *   fx BANK:ADDR stop=ADDR [rN=V...] [frames=N] [keep]
 *                        a Super FX program started as the game starts one
 *                        (a stub at $7F:F000, run as a call)
 *   save NAME            the machine as it is, for a load
 *   load PATH            the machine as state NAME (PATH's file name less
 *                        .state) left it: saved by this run, else replayed
 *                        from power-on, by the script's own steps to its
 *                        save NAME or by the play in NSHVC_STATES that has one
 *   spcset APU BYTE[,BYTE...]  APU RAM bytes for the next spccall
 *   spccall APU [a= x= y= psw= at=APU]   the SPC700 routine at APU called
 *                        when the driver's C next marks `at` ($0897 if not
 *                        named) with spc_at
 *   dump NAME also writes the chip's own RAM as DIR/NAME.gsuram.
 * save, load and a call without keep fork (script.c).
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
