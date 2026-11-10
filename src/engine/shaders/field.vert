#version 450
#extension GL_GOOGLE_include_directive : require
/* field: rectangles of a texture copied a pixel for a pixel into the target
   (the same size): an interlaced picture's field, its half-rows. An instance
   a rectangle of four words: x0, y0, x1, y1 (floats, picture units).
   i[0].x: the rectangles' first word
   tc0: the texture */
#include "common.glsl"
void main()
{
    int k = u.i[0].x + gl_InstanceIndex * 4;
    vec2 p0 = vec2(wordf(k), wordf(k + 1)), p1 = vec2(wordf(k + 2), wordf(k + 3));
    gl_Position = clip_at(mix(p0, p1, corner(gl_VertexIndex)), 0.0);
}
