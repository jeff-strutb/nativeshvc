#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 1) flat in uvec2 v_s;
layout(location = 2) in vec2 v_p;
layout(location = 0) out vec4 o;
void main()
{
    int tpp = u.i[0].z, s = 8 * tpp, cols = u.i[0].y, prow = row_palette(v_p);
    uint n = v_s.x, tile = n & 0xFFFFu, pal = (n >> 16) & 7u;
    ivec2 last = ivec2(int(v_s.y & 0xFFFFu), int(v_s.y >> 16)) * s - 1;
    ivec2 k;
    vec2 w;
    vec4 acc = vec4(0.0);
    sharp(v_t, k, w);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            float wt = share(w, i, j);
            if (wt <= 0.0)
                continue;
            ivec2 q = clamp(k - 1 + ivec2(i, j), ivec2(0), last);
            uint col = uint(q.x / s), row = uint(q.y / s);
            uint t = (tile & 0xFF00u) | (((tile & 0xF0u) + row * 16u) & 0xF0u) | (((tile & 0xFFu) + col) & 0x0Fu);
            uint c = tile_texel(t, q.x % s, q.y % s, s, cols);
            if (c != 0u)
                acc += vec4(palette(128u + pal * 16u + (c & 15u), prow) * wt, wt);
        }
    int m = cover_mask(acc.a);
    if (m == 0)
        discard;
    gl_SampleMask[0] = m;
    o = nshvc_maths(acc.rgb / acc.a, pal >= 4u ? u.f[3].x : u.f[3].y, v_p);
}
