/* rr_window.h: where a frame's units sit in a target, for the backends and
 * the engine's input alike. */
#ifndef RR_WINDOW_H
#define RR_WINDOW_H

#include <math.h>

/* where units shown at `aspect` (width over height; 0 or less: fill) sit in
   a target of w x h: centred, as large as fits, keeping their shape */
static inline void rr_view(float aspect, int w, int h, int *vx, int *vy, int *vw, int *vh)
{
    int a = w, b = h;
    if (aspect > 0.0f) {
        b = (int)((float)w / aspect + 0.5f);
        if (b > h) {
            b = h;
            a = (int)((float)h * aspect + 0.5f);
        }
    }
    *vw = a < 1 ? 1 : a;
    *vh = b < 1 ? 1 : b;
    *vx = (w - *vw) / 2;
    *vy = (h - *vh) / 2;
}

/* a rectangle in units of a uw x uh space shown in view (vx, vy, vw, vh), as
   whole target pixels: each edge at the pixel whose centre it passes */
static inline void rr_scissor(const float r[4], int uw, int uh, int vx, int vy, int vw, int vh, int *x0, int *y0, int *x1, int *y1)
{
    *x0 = vx + (int)floorf(r[0] * (float)vw / (float)uw + 0.5f);
    *y0 = vy + (int)floorf(r[1] * (float)vh / (float)uh + 0.5f);
    *x1 = vx + (int)floorf(r[2] * (float)vw / (float)uw + 0.5f);
    *y1 = vy + (int)floorf(r[3] * (float)vh / (float)uh + 0.5f);
    if (*x0 < vx) *x0 = vx;
    if (*y0 < vy) *y0 = vy;
    if (*x1 > vx + vw) *x1 = vx + vw;
    if (*y1 > vy + vh) *y1 = vy + vh;
    if (*x1 < *x0) *x1 = *x0;
    if (*y1 < *y0) *y1 = *y0;
}

#endif
