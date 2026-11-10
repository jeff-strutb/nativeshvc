#version 450
#extension GL_GOOGLE_include_directive : require
/* overlay: an image over the window, in window pixels.
   f[3].x: its alpha; f[4]: its top left and top right corners; f[5].xy: its bottom left
   i[0].xy: the image's width and height (tc0) */
#include "common.glsl"
layout(location = 0) out vec2 v_uv;
void main()
{
    vec2 k = corner(gl_VertexIndex);
    vec2 a = u.f[4].xy, b = u.f[4].zw, c = u.f[5].xy;
    v_uv = k * vec2(u.i[0].xy);
    gl_Position = clip_at_target(a + k.x * (b - a) + k.y * (c - a));
}
