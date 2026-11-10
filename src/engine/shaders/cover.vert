#version 450
#extension GL_GOOGLE_include_directive : require
/* cover: how much of each texel of Mode 7's plane, or of its tile 0 (8 x 8),
   is each of four plane values (1 or 0), into a layer of an array, for its
   smaller levels to be made from: so that a row whose palette differs from
   the one the plane in colour was made with can add its differences,
   filtered. f[1].xy: the size; i[0]: 0 the plane (ti0), 1 tile 0 (ti1, its
   texels a pixel i[0].y); i[1]: the four values, -1 none */
#include "common.glsl"
void main()
{
    gl_Position = clip_at(corner(gl_VertexIndex) * u.f[1].xy, 0.0);
}
