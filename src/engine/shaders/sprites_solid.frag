#version 450
#extension GL_GOOGLE_include_directive : require
/* sprites_solid: the sprites all of whose tiles are solid (no texel 0), drawn with sprites.vert's pieces
   and sprites.frag's uniforms. Every texel it reads has a colour, so nothing is see-through: no
   discard and no sample mask, the colour exactly sprites.frag's (that stage's test for texel 0 never
   fails, and a pixel's weights then sum to the whole of it) */
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 1) flat in uvec2 v_s;
layout(location = 2) in vec2 v_p;
layout(location = 0) out vec4 o;
void main()
{
    vec2 hh = nshvc_half(v_p);
    int tpp = u.i[0].z, s = 8 * tpp, cols = u.i[0].y, ls = u.i[1].x, lc = u.i[1].y, prow = row_palette(v_p);
    bool pow2 = ls >= 0 && lc >= 0;
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
            uint col = pow2 ? uint(q.x >> ls) : uint(q.x / s), row = pow2 ? uint(q.y >> ls) : uint(q.y / s);
            uint t = (tile & 0xFF00u) | (((tile & 0xF0u) + row * 16u) & 0xF0u) | (((tile & 0xFFu) + col) & 0x0Fu);
            int tx = pow2 ? int(t) & (cols - 1) : int(t) % cols, ty = pow2 ? int(t) >> lc : int(t) / cols;
            int px = pow2 ? q.x & (s - 1) : q.x % s, py = pow2 ? q.y & (s - 1) : q.y % s;
            uint c = texelFetch(ti0, ivec2(tx * s + px, ty * s + py), 0).r;
            acc += vec4(palette(128u + pal * 16u + (c & 15u), prow) * wt, wt);
        }
    o = nshvc_maths(acc.rgb / acc.a, pal >= 4u ? u.f[3].x : u.f[3].y, v_p, hh);
}
