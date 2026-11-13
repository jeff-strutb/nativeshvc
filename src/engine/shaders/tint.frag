#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out vec4 o;

/* plane value t in colour, premultiplied: 0 where see-through */
vec4 tint_of(uint t, int kind)
{
    if (kind == 1)
        return t != 0u ? vec4(u.i[3].z != 0 ? direct(t, 0u) : palette(t, u.i[2].w), 1.0) : vec4(0.0);
    if ((t & 0x7Fu) != 0u && (t >> 7) == uint(kind - 2))
        return vec4(palette(t & 0x7Fu, u.i[2].w), 1.0);
    return vec4(0.0);
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int kind = u.i[6].x, tpp = u.i[0].y, b = u.i[0].z;
    ivec4 sum = ivec4(0);
    int n = b * b;
    /* the mean of the b x b texels this one is made of, in whole numbers of 255ths: each colour is an 8-bit one (a
       palette's entry or a direct colour, k / 255), taken as k, the sum divided by n rounded half up and written as k / 255.0,
       which the 8-bit target stores as that k on any GPU (mips.frag), so that no rounding is left to the GPU */
    for (int j = 0; j < b; j++)
        for (int i = 0; i < b; i++) {
            ivec2 q = p * b + ivec2(i, j);
            uint t = u.i[0].x == 0 ? texelFetch(ti0, q, 0).r : texelFetch(ti1, (q & 7) * tpp + tpp / 2, 0).r;
            sum += ivec4(floor(tint_of(t, kind) * 255.0 + 0.5));
        }
    o = vec4((sum + n / 2) / n) / 255.0;
}
