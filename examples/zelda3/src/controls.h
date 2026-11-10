// controls.h: the game's controls, from every device nativeshvc reports.
#ifndef ZELDA3_CONTROLS_H_
#define ZELDA3_CONTROLS_H_
#include "nshvc/nshvc.h"

// The joypad word the game reads: B in bit 0, then Y, Select, Start, Up,
// Down, Left, Right, A, X, L and R.
int Controls_Read(const nshvc_input *in);
// The on-screen controls, once a finger has touched the screen.
void Controls_Draw(void);

#endif  // ZELDA3_CONTROLS_H_
