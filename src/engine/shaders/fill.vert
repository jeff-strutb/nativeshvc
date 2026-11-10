#version 450
#extension GL_GOOGLE_include_directive : require
/* fill: rectangles of one colour.
   i[0].x: the rectangles' first word, four floats each (x0, y0, x1, y1) in the picture's units
   f[3]: the colour (and alpha) written; f[4].x: the depth */
#include "common.glsl"
void main()
{
    int k = u.i[0].x + gl_InstanceIndex * 4;
    vec2 p0 = vec2(wordf(k), wordf(k + 1)), p1 = vec2(wordf(k + 2), wordf(k + 3));
    gl_Position = clip_at(mix(p0, p1, corner(gl_VertexIndex)), u.f[4].x);
}
