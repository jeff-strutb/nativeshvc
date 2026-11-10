/* common.glsl: what every program of the engine's shares: the renderer's
 * bindings (rr.h) and the helpers for drawing texels sharply at any scale. */
#include "programs.h"

layout(std140, binding = 0) uniform D { vec4 f[8]; ivec4 i[8]; } u;
layout(std430, binding = 1) readonly buffer W { uint w[]; } data;
layout(binding = 2) uniform sampler2D tc0;     /* read at its texels */
layout(binding = 3) uniform sampler2D tc1;     /* filtered: linear, between levels, anisotropic, repeating */
layout(binding = 4) uniform sampler2D tc2;     /* the same */
layout(binding = 5) uniform usampler2D ti0;
layout(binding = 6) uniform usampler2D ti1;
layout(binding = 7) uniform usampler2D ti2;
layout(binding = 8) uniform sampler2DArray ta0;  /* arrays of layers, filtered as tc1 and tc2 */
layout(binding = 9) uniform sampler2DArray ta1;

/* u.f[1].xy: the units of the picture's space */
vec4 clip_at(vec2 p, float depth)
{
    vec2 n = p * (2.0 / u.f[1].xy) - 1.0;
    return vec4(n.x, n.y * u.f[0].w, depth, 1.0);
}

/* a target pixel's position as window pixels, u.f[0].xy the target's size */
vec4 clip_at_target(vec2 p)
{
    vec2 n = p * (2.0 / u.f[0].xy) - 1.0;
    return vec4(n.x, n.y * u.f[0].w, 0.0, 1.0);
}

/* the corner of a quad that vertex i of 6 is: two triangles */
vec2 corner(int i)
{
    return vec2(i == 1 || i == 2 || i == 4 ? 1.0 : 0.0, i == 2 || i == 4 || i == 5 ? 1.0 : 0.0);
}

/* nowhere: an instance with nothing to draw */
const vec4 NOWHERE = vec4(4.0, 4.0, 4.0, 1.0);

/* the rectangle p0-p1 kept to r (x0, y0, x1, y1): 0 when nothing is left */
bool keep_to(inout vec2 p0, inout vec2 p1, vec4 r)
{
    p0 = max(p0, r.xy);
    p1 = min(p1, r.zw);
    return p0.x < p1.x && p0.y < p1.y;
}

int imod(int a, int m) { int r = a % m; return r < 0 ? r + m : r; }
int fdiv(int a, int m) { return (a - imod(a, m)) / m; }

/* Mode 7's plane at texel p, ti0 the plane, ti1 its tiles (`cols` a row,
   `tpp` texels a pixel): 0x100 where it is see-through. outside: 0 repeat,
   1 see-through, 2 tile 0 */
uint m7_texel(ivec2 p, int outside, int cols, int tpp)
{
    if (((p.x | p.y) & ~1023) != 0) {
        if (outside == 1)
            return 0x100u;
        if (outside == 2)
            return texelFetch(ti1, ivec2((p.x & 7) * tpp + tpp / 2, (p.y & 7) * tpp + tpp / 2), 0).r;
        p &= 1023;
    }
    return texelFetch(ti0, p, 0).r;
}

#ifdef FRAG
/* Sharp texels: a pixel takes the texel it falls in, and the one pixel that
   an edge between texels crosses mixes the two by how much of it each covers.
   t is the pixel's centre in texels: k the nearest texel edge, w the part of
   the pixel past it (texel k's share; texel k - 1 has the rest). */
void sharp(vec2 t, out ivec2 k, out vec2 w)
{
    vec2 d = max(fwidth(t), vec2(1.0 / 256.0));
    vec2 b = floor(t + 0.5);
    w = clamp((t - b) / d + 0.5, 0.0, 1.0);
    k = ivec2(b);
}

/* the weight of corner (i, j) of the 2 x 2 texels around edge k */
float share(vec2 w, int i, int j)
{
    return (i == 0 ? 1.0 - w.x : w.x) * (j == 0 ? 1.0 - w.y : w.y);
}

#endif

/* the samples a part `cover` of a pixel covers, as a sample mask; u.f[0].z the samples */
int cover_mask(float cover)
{
    int n = int(cover * u.f[0].z + 0.5);
    return n >= 31 ? -1 : int((1u << uint(n)) - 1u);
}

/* a 15-bit BGR colour as 8-bit channels */
vec3 bgr15(uint c)
{
    uvec3 v = uvec3(c & 31u, (c >> 5) & 31u, (c >> 10) & 31u);
    return vec3((v << 3) | (v >> 2)) / 255.0;
}

/* palette entry e of version (row) v */
vec3 palette(uint e, int v)
{
    return texelFetch(tc0, ivec2(int(e), v), 0).rgb;
}

/* the colour of a direct-colour texel c with palette bits p */
vec3 direct(uint c, uint p)
{
    return bgr15(((c & 7u) << 2 | (p & 1u) << 1) | ((c & 0x38u) >> 1 | (p & 2u)) << 5 | ((c & 0xC0u) >> 3 | (p & 4u)) << 10);
}

/* texel (x, y) of tile t in an atlas of `cols` tiles a row, `s` texels a side, in ti0 */
uint tile_texel(uint t, int x, int y, int s, int cols)
{
    return texelFetch(ti0, ivec2(int(t) % cols * s + x, int(t) / cols * s + y), 0).r;
}

uint word(int k) { return data.w[k]; }
float wordf(int k) { return uintBitsToFloat(data.w[k]); }
