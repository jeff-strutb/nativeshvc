/*
 * input.h: the native controls as the console's controllers (input.c).
 */
#ifndef NSHVC_INPUT_H
#define NSHVC_INPUT_H

#include <stdint.h>

#include "nshvc/nshvc.h"
#include "host.h"

void input_key(int vk, int down);
void input_focus_lost(void);
void input_mouse(float x, float y, float dx, float dy, int buttons);
void input_touch(int id, int phase, float x, float y);
/* each frame: the players' devices to the controllers (game_buttons: what
   the game holds for each player, nshvc_game_buttons) */
void input_frame(const uint16_t *game_buttons);
/* the same with the game controllers' states given (on[k]: controller k
   connected) */
void input_frame_pads(const host_pad *now, const int *on, const uint16_t *game_buttons);
/* (x, y) in window points to the console's dot and line (input.c) */
int input_point(const int *at, float scale, int width, int screen_height, int left, int top, float x, float y, int *dot,
                int *line);
/* a headless run: the script drives every player */
void input_script(void);
/* the players whose device changed since the last, each to hook */
void input_report(void (*hook)(int player, const nshvc_input_source *src));

/* console.c: the console's dot and line shown at (x, y) in the window's
   points; 0 off the picture */
int console_picture_at(float x, float y, int *dot, int *line);

#endif
