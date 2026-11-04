/*
 * front_check.h: what the front end offers the checks in tests/ref: a hook
 * after each headless frame, and the frame's presentation by its steps.
 */
#ifndef FRONT_CHECK_H
#define FRONT_CHECK_H

extern void (*front_frame_hook)(void);
int front_show_mode7(int ow, int oh, const int d[4]);
void front_show_picture(const int d[4]);
void front_rr_need(void);
long front_frame_no(void);
void front_canvas_scale(int w, int h);

#endif
