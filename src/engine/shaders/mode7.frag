#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 1) in vec2 v_p;
layout(location = 0) out vec4 o;

/* the texel step from one screen row to the next at p: the mean of the steps
   from the row above and to the row below, each by its row's own matrix */
vec2 row_step(vec2 p)
{
    vec2 b = vec2(u.f[4].y, u.f[5].y);
    float r = floor(p.y);
    vec2 up = r > float(u.i[7].z) ? b : u.f[6].xy + u.f[6].zw * p.x;
    vec2 down = r < float(u.i[6].w) - 1.0 ? b : u.f[7].xy + u.f[7].zw * p.x;
    return (up + down) * 0.5;
}

void main()
{
    int kind = u.i[6].x, prow = u.i[2].w;
    ivec2 k;
    /* the footprint: across, from this row's matrix; down, by the true step between rows */
    vec2 w, extra = row_step(v_p) - vec2(u.f[4].y, u.f[5].y);
    vec2 dx = dFdx(v_t) + extra * dFdx(v_p.y), dy = dFdy(v_t) + extra * dFdy(v_p.y);
    vec4 acc = vec4(0.0);
    sharp(v_t, k, w);
    if (u.i[7].x != 0 && max(length(dx), length(dy)) > 1.0) {
        /* more than a texel a pixel: the plane in colour, filtered between its
           levels along the pixel's footprint (tc1; tc2 tile 0 outside) */
        bool out_ = any(lessThan(v_t, vec2(0.0))) || any(greaterThanEqual(v_t, vec2(1024.0)));
        vec4 c = vec4(0.0);
        if (!out_ || u.i[6].y != 1) {
            /* the plane, or tile 0 outside it, in the base palette; then this row's
               palette's differences, each weighted by how much of the footprint
               has that value: filtered the same way, so exact */
            bool t0 = out_ && u.i[6].y == 2;
            float scale = t0 ? 8.0 : 1024.0;
            vec2 p = v_t / scale, gx = dx / scale, gy = dy / scale;
            c = t0 ? textureGrad(tc2, p, gx, gy) : textureGrad(tc1, p, gx, gy);
            if (u.i[7].y >= 0) {
                int at = u.i[7].y, n = int(word(at)), last = -1;
                vec4 cov = vec4(0.0);
                for (int e = 0; e < n; e++) {
                    int k = at + 1 + e * 4, lc = int(word(k)), layer = lc & 255;
                    if (layer != last) {
                        cov = t0 ? textureGrad(ta1, vec3(p, float(layer)), gx, gy) : textureGrad(ta0, vec3(p, float(layer)), gx, gy);
                        last = layer;
                    }
                    c.rgb += vec3(wordf(k + 1), wordf(k + 2), wordf(k + 3)) * cov[lc >> 8];
                }
                c.rgb = clamp(c.rgb, vec3(0.0), vec3(c.a));
            }
        }
        acc = c;
    } else {
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
    }
    int m = cover_mask(acc.a);
    if (m == 0)
        discard;
    gl_SampleMask[0] = m;
    o = vec4(acc.rgb / acc.a, u.f[3].x);
}
