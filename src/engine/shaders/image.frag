#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 1) in vec2 v_p;
layout(location = 0) out vec4 o;
void main()
{
    ivec2 k, last = u.i[0].xy - 1;
    vec2 w;
    vec4 acc = vec4(0.0);
    sharp(v_t, k, w);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            float wt = share(w, i, j);
            if (wt <= 0.0)
                continue;
            ivec2 q = clamp(k - 1 + ivec2(i, j), ivec2(0), last);
            if (u.i[0].z != 0) {
                /* alpha: how much of the texel covers the pixel, as a draw list's image */
                vec4 c = texelFetch(tc1, q, 0);
                acc += vec4(c.rgb * c.a, c.a) * wt;
            } else {
                uint c = texelFetch(ti0, q, 0).r;
                if (c != 0u)
                    acc += vec4(palette(c, row_palette(v_p)) * wt, wt);
            }
        }
    int m = cover_mask(acc.a);
    if (m == 0)
        discard;
    gl_SampleMask[0] = m;
    o = nshvc_maths(acc.rgb / acc.a, u.f[3].x, v_p);
}
