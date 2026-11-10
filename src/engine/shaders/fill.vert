#version 450
#extension GL_GOOGLE_include_directive : require
/* fill: rectangles of one colour.
   i[0].x: the rectangles' first word, four floats each (x0, y0, x1, y1) in the picture's units
   f[3]: the colour, and its alpha (the layer's flag in the maths, or the alpha written); f[4].x: the depth */
#include "common.glsl"
layout(location = 0) out vec2 v_p;
void main()
{
    int k = u.i[0].x + gl_InstanceIndex * 4;
    vec2 p0 = vec2(wordf(k), wordf(k + 1)), p1 = vec2(wordf(k + 2), wordf(k + 3));
    v_p = mix(p0, p1, corner(gl_VertexIndex));
    gl_Position = clip_at(v_p, u.f[4].x);
}
