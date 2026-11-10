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

/* plane texel q in this row's palette, premultiplied: 0 where see-through */
vec4 m7_colour(ivec2 q)
{
    int kind = u.i[6].x;
    uint p = m7_texel(q, u.i[6].y, 0, u.i[6].z);
    if (p == 0x100u)
        return vec4(0.0);
    if (kind == 1)
        return p != 0u ? vec4(u.i[3].z != 0 ? direct(p, 0u) : palette(p, u.i[2].w), 1.0) : vec4(0.0);
    if ((p & 0x7Fu) != 0u && (p >> 7) == uint(kind - 2))
        return vec4(palette(p & 0x7Fu, u.i[2].w), 1.0);
    return vec4(0.0);
}

/* texel q of level l (0 or 1) of the plane in this row's palette, worked out from the plane */
vec4 level_texel(ivec2 q, int l)
{
    if (l == 0)
        return m7_colour(q);
    return (m7_colour(q * 2) + m7_colour(q * 2 + ivec2(1, 0)) + m7_colour(q * 2 + ivec2(0, 1)) + m7_colour(q * 2 + ivec2(1, 1))) * 0.25;
}

/* level l (0 or 1) at plane point t (level 0's texels), linear between its texels */
vec4 level_linear(vec2 t, int l)
{
    vec2 q = t * (l == 0 ? 1.0 : 0.5) - 0.5, f = fract(q);
    ivec2 k = ivec2(floor(q));
    return mix(mix(level_texel(k, l), level_texel(k + ivec2(1, 0), l), f.x), mix(level_texel(k + ivec2(0, 1), l), level_texel(k + ivec2(1, 1), l), f.x),
               f.y);
}

/* the kept levels at plane point t, level M7_STORED + lod, linear between texels
   and levels: the plane (or tile 0 outside it) in the base palette, then this
   row's palette's differences, each weighted by how much there has that value:
   filtered the same way, so exact */
vec4 kept(vec2 t, float lod)
{
    bool out_ = any(lessThan(t, vec2(0.0))) || any(greaterThanEqual(t, vec2(1024.0)));
    bool t0 = out_ && u.i[6].y == 2;
    vec2 p;
    vec4 c;
    if (out_ && u.i[6].y == 1)
        return vec4(0.0);
    p = t / (t0 ? 8.0 : 1024.0);
    c = t0 ? textureLod(tc2, p, lod) : textureLod(tc1, p, lod);
    if (u.i[7].y >= 0) {
        int at = u.i[7].y, n = int(word(at)), last = -1;
        vec4 cov = vec4(0.0);
        for (int e = 0; e < n; e++) {
            int k = at + 1 + e * 4, lc = int(word(k)), layer = lc & 255;
            if (layer != last) {
                cov = t0 ? textureLod(ta1, vec3(p, float(layer)), lod) : textureLod(ta0, vec3(p, float(layer)), lod);
                last = layer;
            }
            c.rgb += vec3(wordf(k + 1), wordf(k + 2), wordf(k + 3)) * cov[lc >> 8];
        }
        c.rgb = clamp(c.rgb, vec3(0.0), vec3(c.a));
    }
    return c;
}

/* the plane at point t and level lam, linear between levels: the finer
   levels worked out from the plane, the rest kept */
vec4 at_level(vec2 t, float lam)
{
    int l;
    float f;
    vec4 a;
    if (lam >= float(M7_STORED))
        return kept(t, lam - float(M7_STORED));
    l = int(floor(lam));
    f = lam - float(l);
    a = level_linear(t, l);
    if (f <= 0.0)
        return a;
    return mix(a, l + 1 >= M7_STORED ? kept(t, 0.0) : level_linear(t, l + 1), f);
}

void main()
{
    ivec2 k;
    /* the footprint: across, from this row's matrix; down, by the true step between rows */
    vec2 w, extra = row_step(v_p) - vec2(u.f[4].y, u.f[5].y);
    vec2 dx = dFdx(v_t) + extra * dFdx(v_p.y), dy = dFdy(v_t) + extra * dFdy(v_p.y);
    float lx = length(dx), ly = length(dy);
    vec4 acc = vec4(0.0);
    sharp(v_t, k, w);
    if (u.i[7].x != 0 && max(lx, ly) > 1.0) {
        /* more than a texel a pixel: filtered along the footprint, the same way
           on every GPU: up to 16 points along its longer side, each linear
           between texels and levels at the level its shorter side (or its
           share of the longer) spans */
        vec2 major = lx >= ly ? dx : dy;
        float mj = max(lx, ly), mn = max(min(lx, ly), 1.0 / 1024.0);
        int n = int(min(ceil(mj / mn), 16.0));
        float lam = max(log2(max(mn, mj / float(n))), 0.0);
        for (int i = 0; i < n; i++)
            acc += at_level(v_t + major * ((float(i) + 0.5) / float(n) - 0.5), lam);
        acc /= float(n);
    } else {
        int kind = u.i[6].x, prow = u.i[2].w;
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
