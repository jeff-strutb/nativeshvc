#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_p;
layout(location = 0) out vec4 o;
void main()
{
    vec2 hh = nshvc_half(v_p);    /* before anything discards */
    /* the side target's pixel (resolved: its alpha how much of the pixel the list covers,
       its colour that much of it), as the list's own pixel in this screen */
    vec4 t = texelFetch(tc0, ivec2(gl_FragCoord.xy), 0);
    int m = cover_mask(t.a);
    if (m == 0)
        discard;
    gl_SampleMask[0] = m;
    o = nshvc_maths(t.rgb / t.a, u.f[3].x, v_p, hh);
}
