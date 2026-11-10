#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
void main()
{
    /* bilinear, each texel's colour weighted by its alpha */
    ivec2 last = u.i[0].xy - 1;
    vec2 f = v_uv - 0.5, b = floor(f), t = f - b;
    vec4 acc = vec4(0.0);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            vec4 c = texelFetch(tc0, clamp(ivec2(b) + ivec2(i, j), ivec2(0), last), 0);
            float wt = (i == 0 ? 1.0 - t.x : t.x) * (j == 0 ? 1.0 - t.y : t.y) * c.a;
            acc += vec4(c.rgb * wt, wt);
        }
    if (acc.a <= 0.0)
        discard;
    float a = acc.a * u.f[3].x;
    o = vec4(acc.rgb / acc.a * a, a);
}
