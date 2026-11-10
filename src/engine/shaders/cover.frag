#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out vec4 o;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int tpp = u.i[0].y;
    int t = int(u.i[0].x == 0 ? texelFetch(ti0, p, 0).r : texelFetch(ti1, p * tpp + tpp / 2, 0).r);
    o = vec4(equal(ivec4(t), u.i[1]));
}
