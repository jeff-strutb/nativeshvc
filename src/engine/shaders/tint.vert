#version 450
#extension GL_GOOGLE_include_directive : require
/* tint: Mode 7's plane, or its tile 0 (8 x 8), in colour: premultiplied, 0
   where see-through, for the texture's smaller levels to be made from.
   f[1].xy: the texture's size; i[0]: 0 the plane (ti0), 1 tile 0 (ti1, its
   texels a pixel i[0].y); i[6].x: 1 BG1, 2 BG2's texels of priority 0, 3 of 1;
   i[2].w: the palette's version (tc0); i[3].z: direct colour */
#include "common.glsl"
void main()
{
    gl_Position = clip_at(corner(gl_VertexIndex) * u.f[1].xy, 0.0);
}
