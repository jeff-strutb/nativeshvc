#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out vec4 o;
void main()
{
    vec4 c = u.i[0].x != 0 ? vec4(u.f[3].rgb, 0.0) : texelFetch(tc0, ivec2(gl_FragCoord.xy), 0);
    bool layer = c.a >= 0.5;
    if ((u.i[0].y == 1 && layer) || (u.i[0].y == 2 && !layer))
        discard;
    o = vec4(c.rgb, 1.0);
}
