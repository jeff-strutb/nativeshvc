/* picture_defs.h: the description's layout (render/picture_body.h) for C. */
#ifndef ENGINE_PICTURE_DEFS_H
#define ENGINE_PICTURE_DEFS_H

#include <stdint.h>

#define PB_FN static inline __attribute__((unused))
#define PB_VAR static
#define U32 uint32_t
#define I32 int32_t
#define F32 float
#define U_(x) ((uint32_t)(x))
#define I_(x) ((int32_t)(x))
#define F_(x) ((float)(x))
static inline uint32_t pb_no_read(uint32_t i) { (void)i; return 0; }
#define D_(i) pb_no_read(U_(i))
#define T_(i) pb_no_read(U_(i))
#define P_(x, y) pb_no_read(U_(x) + U_(y))
#define L_(n, x, y) pb_no_read(U_(n) + U_(x) + U_(y))
#define LM_(n, x, y, s) pb_no_read(U_(n) + U_(x) + U_(y) + U_(s))
static inline float pb_f_b(uint32_t b) { union { uint32_t u; float f; } c; c.u = b; return c.f; }
#define F_B(x) pb_f_b(x)
#include <math.h>
#include "picture_body.h"
#undef PB_FN
#undef PB_VAR
#undef U32
#undef I32
#undef F32
#undef U_
#undef I_
#undef F_
#undef D_
#undef T_
#undef P_
#undef L_
#undef LM_
#undef F_B
#undef S_

#endif
