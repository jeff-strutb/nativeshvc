#version 450
#extension GL_GOOGLE_include_directive : require
/* tiles: a tile layer's tiles with some texels 0 (mixed), the instances the vertex stage kept for them
   (tiles.vert, i[6].x 2): a texel 0 is see-through, covering the pixel by what the others cover */
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 1) flat in uvec2 v_e;
layout(location = 2) in vec2 v_p;
layout(location = 0) out vec4 o;
#include "tiles_colour.glsl"


void main()
{
    vec2 hh = nshvc_half(v_p);    /* before anything discards */
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
            if (c != 0u)
                acc += vec4(colour(c, v_e.y, prow) * wt, wt);
        }
    int m = cover_mask(acc.a);
    if (m == 0)
        discard;
    gl_SampleMask[0] = m;
    o = nshvc_maths(acc.rgb / acc.a, u.f[3].x, v_p, hh);
}
