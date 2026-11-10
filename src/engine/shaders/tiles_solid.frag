#version 450
#extension GL_GOOGLE_include_directive : require
/* tiles_solid: a tile layer's solid tiles, those with no texel 0 (tiles.vert, i[6].x 1). Every
   texel it reads is opaque, so it mixes them as tiles.frag does and nothing in it can reject
   a pixel: no discard, no sample mask, so the GPU can skip what a solid tile hides. */
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 1) flat in uvec2 v_e;
layout(location = 2) in vec2 v_p;
layout(location = 0) out vec4 o;
#include "tiles_colour.glsl"

void main()
{
    int s = 8 * u.i[2].x, cols = u.i[2].y, prow = row_palette(v_p);
    ivec2 k;
    vec2 w;
    vec4 acc = vec4(0.0);
    sharp(v_t, k, w);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            float wt = share(w, i, j);
            if (wt <= 0.0)
                continue;
            ivec2 q = clamp(k - 1 + ivec2(i, j), ivec2(0), ivec2(s - 1));
            uint c = tile_texel(v_e.x, q.x, q.y, s, cols);
            acc += vec4(colour(c, v_e.y, prow) * wt, wt);
        }
    o = nshvc_maths(acc.rgb / acc.a, u.f[3].x, v_p);
}
