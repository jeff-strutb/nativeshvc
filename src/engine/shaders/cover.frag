#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out vec4 o;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int tpp = u.i[0].y, b = u.i[0].z;
    vec4 acc = vec4(0.0);
    /* how much of the b x b texels this one is made of has each value */
    for (int j = 0; j < b; j++)
        for (int i = 0; i < b; i++) {
            ivec2 q = p * b + ivec2(i, j);
            int t = int(u.i[0].x == 0 ? texelFetch(ti0, q, 0).r : texelFetch(ti1, q * tpp + tpp / 2, 0).r);
            acc += vec4(equal(ivec4(t), u.i[1]));
        }
    o = acc / float(b * b);
}
