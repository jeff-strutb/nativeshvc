#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 0) out vec4 o;
void main()
{
    vec4 c = v_colour;
    if (u.i[0].w != 0) {
        /* the image's texels, sharp, mixed by their alpha */
        ivec2 k, last = u.i[0].yz - 1;
        vec2 w;
        vec4 acc = vec4(0.0);
        sharp(v_uv, k, w);
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++) {
                float wt = share(w, i, j);
                if (wt <= 0.0)
                    continue;
                vec4 t = texelFetch(tc0, clamp(k - 1 + ivec2(i, j), ivec2(0), last), 0);
                acc += vec4(t.rgb * t.a, t.a) * wt;
            }
        c = acc.a > 0.0 ? vec4(acc.rgb / acc.a, acc.a) : vec4(0.0);
    }
    int m = cover_mask(c.a);
    if (m == 0)
        discard;
    gl_SampleMask[0] = m;
    o = vec4(c.rgb, u.f[3].x);
}
