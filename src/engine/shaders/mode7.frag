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

/* where a pixel spans more than a texel: the plane in colour at plane point t,
   filtered by the GPU along the footprint (dx, dy, in texels): one read of
   the plane in this row's palette (tc1), or of tile 0 in it outside (tc2);
   past those kept full size, the base's plus this palette's (layer i[7].y of
   ta0, tile 0's of ta1, a quarter's side) less the base's at that size
   (layer i[6].x - 1) */
vec4 filtered(vec2 t, vec2 dx, vec2 dy)
{
    int over = u.i[7].y;
    bool out_ = any(lessThan(t, vec2(0.0))) || any(greaterThanEqual(t, vec2(1024.0)));
    vec2 p, gx, gy;
    vec4 c;
    if (out_ && u.i[6].y == 1)
        return vec4(0.0);
    if (out_ && u.i[6].y == 2) {
        p = t / 8.0;
        gx = dx / 8.0;
        gy = dy / 8.0;
        return over >= 0 ? textureGrad(ta1, vec3(p, float(over)), gx, gy) : textureGrad(tc2, p, gx, gy);
    }
    p = t / 1024.0;
    gx = dx / 1024.0;
    gy = dy / 1024.0;
    c = textureGrad(tc1, p, gx, gy);
    if (over >= 0) {
        c.rgb += textureGrad(ta0, vec3(p, float(over)), gx, gy).rgb - textureGrad(ta0, vec3(p, float(u.i[6].x - 1)), gx, gy).rgb;
        c.rgb = clamp(c.rgb, vec3(0.0), vec3(c.a));
    }
    return c;
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
        acc = filtered(v_t, dx, dy);
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
