#version 450
#extension GL_GOOGLE_include_directive : require
/* plane: Mode 7's 1024 x 1024 plane, drawn into a texture from its 128 x 128
   map (an instance a tile) and tiles.
   f[1].xy: 1024, 1024; i[0]: the map's width and height, the atlas's tiles a row, texels a pixel
   ti0: the map; ti1: the tiles */
#include "common.glsl"
layout(location = 0) flat out uint v_tile;
void main()
{
    int mx = gl_InstanceIndex % 128, my = gl_InstanceIndex / 128;
    v_tile = mx < u.i[0].x && my < u.i[0].y ? texelFetch(ti0, ivec2(mx, my), 0).r & 0xFFu : 0u;
    gl_Position = clip_at((vec2(float(mx), float(my)) + corner(gl_VertexIndex)) * 8.0, 0.0);
}
