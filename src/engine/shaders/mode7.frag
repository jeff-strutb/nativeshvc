#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 0) out vec4 o;
void main()
{
    int kind = u.i[6].x, prow = u.i[2].w;
    ivec2 k;
    vec2 w;
    vec4 acc = vec4(0.0);
    sharp(v_t, k, w);
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            float wt = share(w, i, j);
            if (wt <= 0.0)
                continue;
            uint p = m7_texel(k - 1 + ivec2(i, j), u.i[6].y, 0, u.i[6].z);
            if (p == 0x100u)
                continue;
            if (kind == 1) {
                if (p != 0u)
                    acc += vec4((u.i[3].z != 0 ? direct(p, 0u) : palette(p, prow)) * wt, wt);
            } else if ((p & 0x7Fu) != 0u && (p >> 7) == uint(kind - 2)) {
                acc += vec4(palette(p & 0x7Fu, prow) * wt, wt);
            }
        }
    int m = cover_mask(acc.a);
    if (m == 0)
        discard;
    gl_SampleMask[0] = m;
    o = vec4(acc.rgb / acc.a, u.f[3].x);
}
