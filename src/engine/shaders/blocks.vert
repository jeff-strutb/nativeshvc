#version 450
#extension GL_GOOGLE_include_directive : require
/* blocks: a layer under mosaic, an instance for each block, which shows the
   one texel at its top left, read here.
   f[2]: the rectangle drawn, screen pixels; f[3]: alpha written, depth at priority 0, at 1
   f[6]: the block's size, the grid's origin x and y, screen pixels
   i[0]: the first block column and row, columns, rows
   i[6]: what it shows: 0 a tile layer (fields as tiles.vert's, ti0 its tiles, ti1
         its map, ti2 its offset-per-tile map); 1 Mode 7's BG1, 2 its BG2
         (f[4], f[5] the plane's texel = (f[4].x x + f[4].y y + f[4].z,
         f[5].x x + f[5].y y + f[5].z), ti0 the plane, ti1 its tiles);
         Mode 7's outside, its tiles' texels a pixel
   i[3].z: direct colour; the palette's version is the row's (row_palette), read by the fragment stage */
#include "common.glsl"
layout(location = 0) flat out uvec2 v_e;       /* the texel's value, its tile's palette */
layout(location = 1) out vec2 v_p;

void main()
{
    int bx = u.i[0].x + gl_InstanceIndex % u.i[0].z, by = u.i[0].y + gl_InstanceIndex / u.i[0].z;
    float m = u.f[6].x;
    vec2 b0 = u.f[6].yz + vec2(float(bx), float(by)) * m, b1 = b0 + m;
    vec2 q0 = b0, q1 = b1;
    int kind = u.i[6].x;
    bool high = false;
    if (!keep_to(q0, q1, u.f[2])) {
        gl_Position = NOWHERE;
        return;
    }
    if (kind == 0) {
        int sxs = int(u.f[4].x), sys = int(u.f[4].y);
        int lx = sxs * int(b0.x) + int(u.f[4].z), ly = sys * int(b0.y) + int(u.f[4].w);
        if (u.i[3].w != 0) {
            int sx = u.i[5].y, sy = u.i[5].z, hires = u.i[5].w;
            int optw = hires != 0 ? 16 : 8, fine = hires != 0 ? ((sx << 1) & 7) : (sx & 7);
            int col = fdiv(lx - sxs * sx + fine, optw);
            if (col > 0) {
                int w3 = u.i[4].x, h3 = u.i[4].y, x3 = u.i[4].z, y3 = u.i[4].w, bit = u.i[5].x, nsx = sx, nsy = sy;
                int obx = imod(((col - 1) * 8 + (x3 & ~7)) >> 3, w3);
                uint hv = texelFetch(ti2, ivec2(obx, imod(y3 >> 3, h3)), 0).r;
                if (u.i[3].w == 2) {
                    if (((hv >> bit) & 1u) != 0u) {
                        if (((hv >> 21) & 1u) != 0u)
                            nsy = int(hv & 0x3FFu);
                        else
                            nsx = (sx & 7) | int(hv & 0x3F8u);
                    }
                } else {
                    uint vv = texelFetch(ti2, ivec2(obx, imod((y3 + 8) >> 3, h3)), 0).r;
                    if (((hv >> bit) & 1u) != 0u)
                        nsx = (sx & 7) | int(hv & 0x3F8u);
                    if (((vv >> bit) & 1u) != 0u)
                        nsy = int(vv & 0x3FFu);
                }
                lx += sxs * (nsx - sx);
                ly += nsy - sy;
            }
        }
        int mw = u.i[1].x, mh = u.i[1].y, tw = u.i[1].z, th = u.i[1].w, tpp = u.i[2].x;
        if (u.i[2].z != 0) {
            lx = imod(lx, mw * tw);
            ly = imod(ly, mh * th);
        } else if (lx < 0 || ly < 0 || lx >= mw * tw || ly >= mh * th) {
            gl_Position = NOWHERE;
            return;
        }
        uint e = texelFetch(ti1, ivec2(lx / tw, ly / th), 0).r;
        int px = lx % tw, py = ly % th;
        if (((e >> 20) & 1u) != 0u)
            px = tw - 1 - px;
        if (((e >> 21) & 1u) != 0u)
            py = th - 1 - py;
        uint ch = (e & 0xFFFFu) + (px >= 8 ? 1u : 0u) + (py >= 8 ? 16u : 0u);
        uint c = tile_texel(ch, (px & 7) * tpp + tpp / 2, (py & 7) * tpp + tpp / 2, 8 * tpp, u.i[2].y);
        if (c == 0u) {
            gl_Position = NOWHERE;
            return;
        }
        v_e = uvec2(c, (e >> 16) & 7u);
        high = ((e >> 19) & 1u) != 0u;
    } else {
        ivec2 t = ivec2(floor(vec2(dot(u.f[4].xyz, vec3(b0, 1.0)), dot(u.f[5].xyz, vec3(b0, 1.0))) + m7_line_round(int(floor(b0.y)))));
        uint p = m7_texel(t, u.i[6].y, 0, u.i[6].z);
        if (kind == 1) {
            if (p == 0u || p == 0x100u) {
                gl_Position = NOWHERE;
                return;
            }
            v_e = uvec2(p, 0u);
        } else {
            if (p == 0x100u || (p & 0x7Fu) == 0u) {
                gl_Position = NOWHERE;
                return;
            }
            v_e = uvec2(p & 0x7Fu, 0u);
            high = (p & 0x80u) != 0u;
        }
    }
    v_p = mix(q0, q1, corner(gl_VertexIndex));
    gl_Position = clip_at(v_p, high ? u.f[3].z : u.f[3].y);
}
