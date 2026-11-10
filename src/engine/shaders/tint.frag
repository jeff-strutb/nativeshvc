#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out vec4 o;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int kind = u.i[6].x, tpp = u.i[0].y;
    uint t = u.i[0].x == 0 ? texelFetch(ti0, p, 0).r : texelFetch(ti1, p * tpp + tpp / 2, 0).r;
    o = vec4(0.0);
    if (kind == 1) {
        if (t != 0u)
            o = vec4(u.i[3].z != 0 ? direct(t, 0u) : palette(t, u.i[2].w), 1.0);
    } else if ((t & 0x7Fu) != 0u && (t >> 7) == uint(kind - 2)) {
        o = vec4(palette(t & 0x7Fu, u.i[2].w), 1.0);
    }
}
