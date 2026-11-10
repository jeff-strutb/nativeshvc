/* common.glsl: what every program of the engine's shares: the renderer's
 * bindings (rr.h) and the helpers for drawing texels sharply at any scale. */
#include "programs.h"

layout(std140, binding = 0) uniform D { vec4 f[8]; ivec4 i[8]; } u;
layout(std430, binding = 1) readonly buffer W { uint w[]; } data;
layout(binding = 2) uniform sampler2D tc0;     /* read at its texels */
layout(binding = 3) uniform sampler2D tc1;     /* filtered: linear, between levels, anisotropic, repeating */
layout(binding = 4) uniform sampler2D tc2;     /* filtered the same, transparent outside the texture */
layout(binding = 5) uniform usampler2D ti0;
layout(binding = 6) uniform usampler2D ti1;
layout(binding = 7) uniform usampler2D ti2;
layout(binding = 8) uniform sampler2DArray ta0;  /* arrays of layers, filtered as tc1 (repeating) */
layout(binding = 9) uniform sampler2DArray ta1;  /* and as tc2 (transparent outside) */
layout(binding = 10) uniform sampler2DArray ta2; /* the same */
layout(binding = 11) uniform sampler2D tsub;   /* read at its texels: the sub screen, for the colour maths (maths.glsl) */
layout(binding = 12) uniform sampler2D taux;   /* and the main screen alone, for a hires row's even half-columns */

/* a normalised point (-1 to 1, y down) as the clip space of the target it is
   drawn into. u.f[0].w is the backend's code, a small whole number: bit 0, the
   API's clip y is down (else up); bits 1-2, quarter turns clockwise; bit 3,
   mirrored left to right first: a window-sized target the display is turned
   from is drawn turned (the backend turns its viewport and scissors to match) */
vec4 oriented(vec2 n, float depth)
{
    int code = int(u.f[0].w);
    if ((code & 8) != 0)
        n.x = -n.x;
    switch ((code >> 1) & 3) {
    case 1: n = vec2(-n.y, n.x); break;
    case 2: n = -n; break;
    case 3: n = vec2(n.y, -n.x); break;
    }
    return vec4(n.x, (code & 1) != 0 ? n.y : -n.y, depth, 1.0);
}

/* u.f[1].xy: the units of the picture's space */
vec4 clip_at(vec2 p, float depth)
{
    return oriented(p * (2.0 / u.f[1].xy) - 1.0, depth);
}

/* a target pixel's position as window pixels, u.f[0].xy the target's size (unturned) */
vec4 clip_at_target(vec2 p)
{
    return oriented(p * (2.0 / u.f[0].xy) - 1.0, 0.0);
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
void sharp_at(vec2 t, vec2 span, out ivec2 k, out vec2 w)
{
    vec2 d = max(span, vec2(1.0 / 256.0));
    vec2 b = floor(t + 0.5);
    w = clamp((t - b) / d + 0.5, 0.0, 1.0);
    k = ivec2(b);
}

/* the same, the pixel's span in texels from t itself */
void sharp(vec2 t, out ivec2 k, out vec2 w)
{
    sharp_at(t, fwidth(t), k, w);
}

/* the weight of corner (i, j) of the 2 x 2 texels around edge k */
float share(vec2 w, int i, int j)
{
    return (i == 0 ? 1.0 - w.x : w.x) * (j == 0 ? 1.0 - w.y : w.y);
}

#endif

#ifdef FRAG
#define MATHS_U u
#define MATHS_DATA data
#define MATHS_SUB tsub
#define MATHS_AUX taux
#include "maths.glsl"
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
    /* a tile past the set's reads 0: one in the atlas's last row is never
       written (a new texture reads 0), one past it is not read */
    if (int(t) / cols * s >= textureSize(ti0, 0).y)
        return 0u;
    return texelFetch(ti0, ivec2(int(t) % cols * s + x, int(t) / cols * s + y), 0).r;
}

/* Mode 7's per-line rounding at picture row `row`, in texels: the console
   keeps b x line and d x line to 64ths before adding them (the linear matrix
   a quad is drawn with has them exact from its first row, i[5].y, on, its
   rounding that row's): this row's rounding less that row's. i[4]: b, d, and
   the console's line of picture row r, i[4].z + i[4].w * r */
vec2 m7_line_round(int row)
{
    ivec2 a = u.i[4].xy * (u.i[4].z + u.i[4].w * row), b = u.i[4].xy * (u.i[4].z + u.i[4].w * u.i[5].y);
    return vec2(((a & ~63) - a) - ((b & ~63) - b)) / 256.0;
}

uint word(int k) { return data.w[k]; }
float wordf(int k) { return uintBitsToFloat(data.w[k]); }

/* the palette version at picture point p: the draw's own, u.i[7].x - 1, where
   every row and column it covers has one (u.i[7].x 0 where not); else as its
   row has it: the frame's words from u.i[7].w hold two a row (its first
   entry's word, its entries), an entry the column the version starts at and
   the version */
int row_palette(vec2 p)
{
    if (u.i[7].x > 0)
        return u.i[7].x - 1;
    int r = clamp(int(floor(p.y)), 0, int(u.f[1].y) - 1), k = u.i[7].w + r * 2;
    int first = int(word(k)), n = int(word(k + 1)), pal = int(word(first + 1));
    for (int i = 1; i < n; i++)
        if (p.x >= float(int(word(first + i * 2))))
            pal = int(word(first + i * 2 + 1));
    return pal;
}
