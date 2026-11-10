#version 450
#extension GL_GOOGLE_include_directive : require
/* image: an image as a layer, over the screen from its top left.
   f[2]: the rectangle drawn; f[3]: alpha written, depth; f[4].xy: the image's texels a screen pixel
   i[0]: the image's width and height, 1 if full colour (tc1) or 0 indexed (ti0, through the row's palette) */
#include "common.glsl"
layout(location = 0) out vec2 v_t;
layout(location = 1) out vec2 v_p;
void main()
{
    vec4 r = u.f[2];
    vec2 p = mix(r.xy, r.zw, corner(gl_VertexIndex));
    v_t = p * u.f[4].xy;
    v_p = p;
    gl_Position = clip_at(p, u.f[3].y);
}
