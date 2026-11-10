#version 450
#extension GL_GOOGLE_include_directive : require
/* tiles: a tile layer, an instance for each 8 x 8 texels of a tile (a char)
   that the rectangle shows, the map read here.
   f[2]: the rectangle drawn, screen pixels; f[3]: alpha written, depth at priority 0, at 1
   f[4]: layer pixel = (f[4].x * x + f[4].z, f[4].y * y + f[4].w) for screen pixel (x, y)
   i[0]: the first char column, -, columns, rows
   i[1]: the map's width and height in tiles, a tile's width and height in layer pixels
   i[2]: texels a pixel, the atlas's tiles a row, wrap; the palette's version is the row's (row_palette)
   i[3]: bits a texel, mode 0's palette group, direct colour, offset-per-tile (1 modes 2 and 6, 2 mode 4)
   i[4]: offset-per-tile: its map's width and height, BG3's scroll x and y
   i[5]: offset-per-tile: this layer's bit in an entry, its own scroll x and y, hires
   i[6]: the class of tile the draw is for (1 solid, 2 mixed: tiles_solid.frag, tiles.frag), the frame's
         word where the set's classes start (two bits a tile, 16 a word: 0 blank, 1 solid, 2 mixed),
         and the set's tile count; an instance whose tile is another class is put nowhere, a blank
         tile (or one past the set's count) is in neither draw
   ti0: the tiles; ti1: the map; ti2: the offset-per-tile map */
#include "common.glsl"
layout(location = 0) out vec2 v_t;
layout(location = 1) flat out uvec2 v_e;
layout(location = 2) out vec2 v_p;
void main()
{
    int cols = u.i[0].z;
    int c = u.i[0].x + gl_InstanceIndex % cols, ri = gl_InstanceIndex / cols;
    vec4 r = u.f[2];
    vec2 sc = u.f[4].xy;
    float offx = u.f[4].z, offy = u.f[4].w;
    int cc = c;
    if (u.i[3].w != 0) {
        /* offset-per-tile: this char's column of the screen takes its scroll from BG3's map */
        int sx = u.i[5].y, sy = u.i[5].z, hires = u.i[5].w;
        int optw = hires != 0 ? 16 : 8, fine = hires != 0 ? ((sx << 1) & 7) : (sx & 7);
        float hx = float(c * 8 + 4) - sc.x * float(sx);
        int col = int(floor((hx + float(fine)) / float(optw)));
        if (col > 0) {
            int w3 = u.i[4].x, h3 = u.i[4].y, x3 = u.i[4].z, y3 = u.i[4].w, bit = u.i[5].x, nsx = sx, nsy = sy;
            int bx = imod(((col - 1) * 8 + (x3 & ~7)) >> 3, w3);
            uint hv = texelFetch(ti2, ivec2(bx, imod(y3 >> 3, h3)), 0).r;
            if (u.i[3].w == 2) {
                if (((hv >> bit) & 1u) != 0u) {
                    if (((hv >> 21) & 1u) != 0u)
                        nsy = int(hv & 0x3FFu);
                    else
                        nsx = (sx & 7) | int(hv & 0x3F8u);
                }
            } else {
                uint vv = texelFetch(ti2, ivec2(bx, imod((y3 + 8) >> 3, h3)), 0).r;
                if (((hv >> bit) & 1u) != 0u)
                    nsx = (sx & 7) | int(hv & 0x3F8u);
                if (((vv >> bit) & 1u) != 0u)
                    nsy = int(vv & 0x3FFu);
            }
            cc = c + (nsx - sx) * int(sc.x) / 8;
            offy += float(nsy - sy);
        }
    }
    int row = int(floor((sc.y * r.y + offy) / 8.0)) + ri;
    vec2 p0 = (vec2(float(c * 8), float(row * 8)) - vec2(offx, offy)) / sc;
    vec2 p1 = (vec2(float(c * 8 + 8), float(row * 8 + 8)) - vec2(offx, offy)) / sc;
    vec2 q0 = p0, q1 = p1;
    if (!keep_to(q0, q1, r)) {
        gl_Position = NOWHERE;
        return;
    }
    int mw = u.i[1].x, mh = u.i[1].y, tw = u.i[1].z, th = u.i[1].w;
    int tx = fdiv(cc * 8, tw), ty = fdiv(row * 8, th);
    if (u.i[2].z != 0) {
        tx = imod(tx, mw);
        ty = imod(ty, mh);
    } else if (tx < 0 || ty < 0 || tx >= mw || ty >= mh) {
        gl_Position = NOWHERE;
        return;
    }
    uint e = texelFetch(ti1, ivec2(tx, ty), 0).r;
    bool hf = ((e >> 20) & 1u) != 0u, vf = ((e >> 21) & 1u) != 0u;
    int subx = imod(cc, tw / 8), suby = imod(row, th / 8);
    if (hf)
        subx = tw / 8 - 1 - subx;
    if (vf)
        suby = th / 8 - 1 - suby;
    vec2 p = mix(q0, q1, corner(gl_VertexIndex));
    vec2 t = (p - p0) / (p1 - p0) * 8.0;
    if (hf)
        t.x = 8.0 - t.x;
    if (vf)
        t.y = 8.0 - t.y;
    v_t = t * float(u.i[2].x);
    v_p = p;
    v_e = uvec2((e & 0xFFFFu) + uint(subx) + 16u * uint(suby), (e >> 16) & 7u);
    {
        uint tile = v_e.x, cl = tile < uint(u.i[6].z) ? (word(u.i[6].y + int(tile >> 4)) >> ((tile & 15u) * 2u)) & 3u : 0u;
        if (int(cl) != u.i[6].x) {
            gl_Position = NOWHERE;
            return;
        }
    }
    gl_Position = clip_at(p, ((e >> 19) & 1u) != 0u ? u.f[3].z : u.f[3].y);
}
