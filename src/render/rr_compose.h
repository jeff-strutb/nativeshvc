/*
 * rr_compose.h: compose_body.h in C, the reference each GPU backend's
 * compositor is held to (and itself held to ppu.c's ppu_draw_frame:
 * NSHVC_COMPOSECHECK=1). Set rr_cp_fd to a frame's description
 * (ppu_compose), then rr_cp_pixel(x, y) is the picture's pixel.
 */
#ifndef RR_COMPOSE_H
#define RR_COMPOSE_H

#include <stdint.h>

#define CP_FN static inline
#define CP_VAR static
#define U32 uint32_t
#define I32 int32_t
#define U_(x) ((uint32_t)(x))
#define I_(x) ((int32_t)(x))
static const uint32_t *rr_cp_fd;
#define FD_(i) rr_cp_fd[cp_fdb + (i)]

#include "compose_body.h"

static inline uint32_t rr_cp_pixel(uint32_t x, uint32_t y) { return cp_pixel(x, y); }

#undef CP_FN
#undef CP_VAR
#undef U32
#undef I32
#undef U_
#undef I_
#undef FD_

#endif
