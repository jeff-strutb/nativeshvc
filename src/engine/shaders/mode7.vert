#version 450
#extension GL_GOOGLE_include_directive : require
/* mode7: Mode 7's plane over the rectangle, its texels through the matrix:
   texel = (f[4].x x + f[4].y y + f[4].z, f[5].x x + f[5].y y + f[5].z) for
   screen point (x, y).
   f[2]: the rectangle; f[3]: alpha written, depth at priority 0, at 1
   i[6]: 1 BG1, 2 BG2's texels of priority 0, 3 those of 1; outside; the tiles' texels a pixel
   i[2].w: the palette's version; i[3].z: direct colour
   i[7].x: 1 if the plane in colour is given, from level M7_STORED down (tc1), and tile 0 (tc2),
         in the palette most of Mode 7's rows have; finer levels are worked out from the plane in
         this row's palette; i[7].y: where this row's palette differs, the
         first word of its corrections, else -1: a count, then four words each: a layer of the
         coverage arrays (ta0 the plane's, ta1 tile 0's, from level M7_STORED down) | its channel << 8, and the colour that
         the plane value there gains (red, green, blue, floats)
   f[6], f[7]: the texel step from the row above the strip's first row (i[7].z) to it, and from
         its last row (before i[6].w) to the row below, each by those rows' own matrices: the
         step at screen column x is f.xy + f.zw * x (within the strip the step is f[4].y, f[5].y)
   ti0: the plane; ti1: its tiles */
#include "common.glsl"
layout(location = 0) out vec2 v_t;
layout(location = 1) out vec2 v_p;
void main()
{
    vec4 r = u.f[2];
    vec2 p = mix(r.xy, r.zw, corner(gl_VertexIndex));
    v_t = vec2(dot(u.f[4].xyz, vec3(p, 1.0)), dot(u.f[5].xyz, vec3(p, 1.0)));
    v_p = p;
    gl_Position = clip_at(p, u.i[6].x == 3 ? u.f[3].z : u.f[3].y);
}
