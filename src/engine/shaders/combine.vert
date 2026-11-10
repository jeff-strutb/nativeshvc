#version 450
#extension GL_GOOGLE_include_directive : require
/* combine: the colour maths' other screen onto the frame, over the rectangle.
   f[2]: the rectangle; f[3].rgb: the fixed colour
   i[0]: 1 the fixed colour, 0 the other screen (tc0, a resolved target: alpha
         1 where a layer is, 0 its backdrop); 1 nothing where the other screen
         has a layer, 2 nothing where it has its backdrop, 0 everywhere
   Writes the colour and alpha 1, for the draw's blend state to combine. */
#include "common.glsl"
void main()
{
    vec4 r = u.f[2];
    gl_Position = clip_at(mix(r.xy, r.zw, corner(gl_VertexIndex)), 0.0);
}
