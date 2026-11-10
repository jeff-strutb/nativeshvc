#version 450
#extension GL_GOOGLE_include_directive : require
/* mode7: Mode 7's plane over the rectangle, its texels through the matrix:
   texel = (f[4].x x + f[4].y y + f[4].z, f[5].x x + f[5].y y + f[5].z) for
   screen point (x, y).
   f[2]: the rectangle; f[3]: alpha written, depth at priority 0, at 1
   i[6]: 1 BG1, 2 BG2's texels of priority 0, 3 those of 1; outside; the tiles' texels a pixel
   i[2].w: the palette's version; i[3].z: direct colour
   i[7].x: 1 if the plane in colour is given, with its smaller levels (tc1), and tile 0 (tc2)
   ti0: the plane; ti1: its tiles */
#include "common.glsl"
layout(location = 0) out vec2 v_t;
void main()
{
    vec4 r = u.f[2];
    vec2 p = mix(r.xy, r.zw, corner(gl_VertexIndex));
    v_t = vec2(dot(u.f[4].xyz, vec3(p, 1.0)), dot(u.f[5].xyz, vec3(p, 1.0)));
    gl_Position = clip_at(p, u.i[6].x == 3 ? u.f[3].z : u.f[3].y);
}
