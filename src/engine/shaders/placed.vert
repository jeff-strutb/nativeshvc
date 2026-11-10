#version 450
#extension GL_GOOGLE_include_directive : require
/* placed: a draw list drawn into a side target of its own (at its own samples),
   placed where its layer is in a screen: a quad over the rectangle.
   f[2]: the rectangle drawn; f[3]: the layer's flag in the maths, its depth
   tc0: the side target's texture */
#include "common.glsl"
layout(location = 0) out vec2 v_p;
void main()
{
    vec4 r = u.f[2];
    v_p = mix(r.xy, r.zw, corner(gl_VertexIndex));
    gl_Position = clip_at(v_p, u.f[3].y);
}
