#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 1) flat in uvec2 v_e;
layout(location = 0) out vec4 o;

vec3 colour(uint c, uint pal, int prow)
{
    int bpp = u.i[3].x;
    if (bpp == 8)
        return u.i[3].z != 0 ? direct(c, pal) : palette(c, prow);
    if (bpp == 4)
        return palette(pal * 16u + c, prow);
    return palette(uint(u.i[3].y) + pal * 4u + c, prow);
}

void main()
{
    int s = 8 * u.i[2].x, cols = u.i[2].y, prow = u.i[2].w;
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
    o = vec4(acc.rgb / acc.a, u.f[3].x);
}
