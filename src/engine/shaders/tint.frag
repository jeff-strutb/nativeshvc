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
    vec4 acc = vec4(0.0);
    /* the mean of the b x b texels this one is made of */
    for (int j = 0; j < b; j++)
        for (int i = 0; i < b; i++) {
            ivec2 q = p * b + ivec2(i, j);
            uint t = u.i[0].x == 0 ? texelFetch(ti0, q, 0).r : texelFetch(ti1, (q & 7) * tpp + tpp / 2, 0).r;
            acc += tint_of(t, kind);
        }
    o = acc / float(b * b);
}
