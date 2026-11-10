#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_p;
layout(location = 0) out vec4 o;
void main()
{
    vec2 hh = nshvc_half(v_p);
    o = nshvc_maths(u.f[3].rgb, u.f[3].a, v_p, hh);
}
