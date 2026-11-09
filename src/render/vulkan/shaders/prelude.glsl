#version 450
#define PB_FN
#define PB_VAR
#define U32 uint
#define I32 int
#define F32 float
#define U_(x) uint(x)
#define I_(x) int(x)
#define F_(x) float(x)
#define F_B(x) uintBitsToFloat(x)
#define D_(i) pb_db[i]
#define T_(i) pb_tb[i]
#define P_(x, y) texelFetch(pb_img, ivec2(x, y), 0).r
layout(std430, binding = 0) readonly buffer DB { uint pb_db[]; };
layout(std430, binding = 1) readonly buffer TB { uint pb_tb[]; };
layout(binding = 2) uniform usampler2D pb_img;
layout(binding = 3) uniform usampler2D pb_l0;
layout(binding = 4) uniform usampler2D pb_l1;
layout(binding = 5) uniform usampler2D pb_l2;
layout(binding = 6) uniform usampler2D pb_l3;
uint pb_list_read(uint n, uint x, uint y) {
    ivec2 p = ivec2(x, y);
    if (n == 0u) return texelFetch(pb_l0, p, 0).r;
    if (n == 1u) return texelFetch(pb_l1, p, 0).r;
    if (n == 2u) return texelFetch(pb_l2, p, 0).r;
    return texelFetch(pb_l3, p, 0).r;
}
#define L_(n, x, y) pb_list_read(n, x, y)
