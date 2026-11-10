#version 450
#extension GL_GOOGLE_include_directive : require
/* sprites: an instance for each piece of a sprite, eight words: its
   rectangle (x0, y0, x1, y1) in screen pixels, the sprite's own pixel at its
   top left (x, y), the sprite's size in tiles (w | h << 16), and tile |
   palette << 16 | priority << 19 | hflip << 21 | vflip << 22.
   f[2]: the rectangle drawn; f[3]: alpha written by palettes 4-7, by 0-3; f[4]: depth at priorities 0-3
   i[0]: the pieces' first word, the atlas's tiles a row, texels a pixel, the atlas's height in texels;
   i[1]: log2 of the texels a side and of the tiles a row, each -1 when not a power of two; the palette's version is the row's
   ti0: the tiles */
#include "common.glsl"
layout(location = 0) out vec2 v_t;
layout(location = 1) flat out uvec2 v_s;
layout(location = 2) out vec2 v_p;
void main()
{
    int k = u.i[0].x + gl_InstanceIndex * 8;
    vec2 p0 = vec2(float(int(word(k))), float(int(word(k + 1)))), p1 = vec2(float(int(word(k + 2))), float(int(word(k + 3))));
    vec2 l0 = vec2(float(int(word(k + 4))), float(int(word(k + 5))));
    uint size = word(k + 6), n = word(k + 7);
    vec2 q0 = p0, q1 = p1;
    if (!keep_to(q0, q1, u.f[2])) {
        gl_Position = NOWHERE;
        return;
    }
    vec2 p = mix(q0, q1, corner(gl_VertexIndex));
    vec2 t = l0 + (p - p0);
    if (((n >> 21) & 1u) != 0u)
        t.x = float((size & 0xFFFFu) * 8u) - t.x;
    if (((n >> 22) & 1u) != 0u)
        t.y = float((size >> 16) * 8u) - t.y;
    v_t = t * float(u.i[0].z);
    v_s = uvec2(n, size);
    v_p = p;
    gl_Position = clip_at(p, u.f[4][int((n >> 19) & 3u)]);
}
