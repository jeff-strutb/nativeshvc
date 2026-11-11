/* the sprites' fragment stage, in four programs (sprites.frag, sprites_solid.frag, sprites_p2.frag,
   sprites_solid_p2.frag): each defines SOLID where all of a draw's tiles are solid (no texel 0: nothing is
   see-through, so no discard and no sample mask, the colour exactly the see-through program's: its test for
   texel 0 never fails, and a pixel's weights then sum to the whole of it) and POW2 where the tile set's texels
   a side and tiles a row are both powers of two (shifts and masks in place of the divisions; the choice is the
   program's, so no pixel does any work for it).
   Sprites' pieces and uniforms are sprites.vert's; i[1] (POW2): log2 of the texels a side and of the tiles a row */
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 1) flat in uvec2 v_s;
layout(location = 2) in vec2 v_p;
layout(location = 0) out vec4 o;
void main()
{
    vec2 hh = nshvc_half(v_p);    /* before anything discards */
    int tpp = u.i[0].z, s = 8 * tpp, cols = u.i[0].y, ah = u.i[0].w, prow = row_palette(v_p);
#ifdef POW2
    int ls = u.i[1].x, lc = u.i[1].y;
#endif
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
#ifdef POW2
            uint col = uint(q.x >> ls), row = uint(q.y >> ls);
#else
            uint col = uint(q.x / s), row = uint(q.y / s);
#endif
            uint t = (tile & 0xFF00u) | (((tile & 0xF0u) + row * 16u) & 0xF0u) | (((tile & 0xFFu) + col) & 0x0Fu);
#ifdef POW2
            int tx = int(t) & (cols - 1), ty = int(t) >> lc;
            int px = q.x & (s - 1), py = q.y & (s - 1);
#else
            int tx = int(t) % cols, ty = int(t) / cols;
            int px = q.x % s, py = q.y % s;
#endif
#ifdef SOLID
            uint c = texelFetch(ti0, ivec2(tx * s + px, ty * s + py), 0).r;
            acc += vec4(palette(128u + pal * 16u + (c & 15u), prow) * wt, wt);
#else
            /* a tile past the set's reads 0, as tile_texel's, the atlas's height from the uniform */
            uint c = ty * s >= ah ? 0u : texelFetch(ti0, ivec2(tx * s + px, ty * s + py), 0).r;
            if (c != 0u)
                acc += vec4(palette(128u + pal * 16u + (c & 15u), prow) * wt, wt);
#endif
        }
#ifndef SOLID
    int m = cover_mask(acc.a);
    if (m == 0)
        discard;
    gl_SampleMask[0] = m;
#endif
    o = nshvc_maths(acc.rgb / acc.a, pal >= 4u ? u.f[3].x : u.f[3].y, v_p, hh);
}
