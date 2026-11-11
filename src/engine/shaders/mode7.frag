#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_t;
layout(location = 1) in vec2 v_p;
layout(location = 0) out vec4 o;

/* the texel step from one screen row to the next at p: the shorter, by length, of the steps from the
   row above and to the row below, each by its row's own matrix. A jump of the matrix between two rows
   is then in neither row's step: each keeps its other, ordinary neighbour's. Where both agree (a
   perspective ramp) it is that step. A neighbour that is no Mode 7 (or the picture's edge) is marked
   by 1e30 in f[6] or f[7], and the step is the other's; with neither, the matrix's own */
vec2 row_step(vec2 p)
{
    vec2 b = vec2(u.f[4].y, u.f[5].y);
    float r = floor(p.y);
    bool in_up = r > float(u.i[7].z), in_down = r < float(u.i[6].w) - 1.0;
    bool has_up = in_up || u.f[6].x < 1e29, has_down = in_down || u.f[7].x < 1e29;
    vec2 up = in_up ? b : u.f[6].xy + u.f[6].zw * p.x;
    vec2 down = in_down ? b : u.f[7].xy + u.f[7].zw * p.x;
    if (!has_up)
        return has_down ? down : b;
    if (!has_down)
        return up;
    return dot(up, up) <= dot(down, down) ? up : down;
}

/* where a pixel spans more than a texel: the plane in colour at plane point t,
   filtered by the GPU along the footprint (dx, dy, in texels), one read of
   it in this row's palette. Outside the plane (i[6].y): 0 it repeats, read
   repeating (tc1); 1 it is see-through, read as transparent outside (tc2);
   2 tile 0 repeats there: tile 0 repeated everywhere (tc1, 8 x 8), plus the
   plane, less tile 0 repeated across the plane (layer i[5].z of ta2), both
   transparent outside, so every footprint mixes the plane and tile 0 by how
   much of it each has. Past the full-size planes: the base palette's (tc1,
   tc2, ta2), plus this palette's at a quarter's side (layer i[7].y of ta0 or
   ta1, as the plane is read), less the base's at that size (layer i[6].x - 1);
   tile 0 this palette's (ta0's layer i[7].y), and the plane of tile 0
   repeated corrected the same way (layers i[5].x and i[6].x + 2 of ta1) */
vec4 filtered(vec2 t, vec2 dx, vec2 dy)
{
    int outside = u.i[6].y, over = u.i[7].y, kind = u.i[6].x;
    vec2 p = t / 1024.0, gx = dx / 1024.0, gy = dy / 1024.0;
    vec4 c;
    if (outside == 0) {
        c = textureGrad(tc1, p, gx, gy);
        if (over >= 0)
            c.rgb += textureGrad(ta0, vec3(p, float(over)), gx, gy).rgb - textureGrad(ta0, vec3(p, float(kind - 1)), gx, gy).rgb;
    } else {
        c = textureGrad(tc2, p, gx, gy);
        if (over >= 0)
            c.rgb += textureGrad(ta1, vec3(p, float(over)), gx, gy).rgb - textureGrad(ta1, vec3(p, float(kind - 1)), gx, gy).rgb;
        if (outside == 2) {
            vec2 q = t / 8.0, qx = dx / 8.0, qy = dy / 8.0;
            vec4 t0 = over >= 0 ? textureGrad(ta0, vec3(q, float(over)), qx, qy) : textureGrad(tc1, q, qx, qy);
            vec4 r = textureGrad(ta2, vec3(p, float(u.i[5].z)), gx, gy);
            if (over >= 0 && u.i[5].x >= 0)
                r.rgb += textureGrad(ta1, vec3(p, float(u.i[5].x)), gx, gy).rgb - textureGrad(ta1, vec3(p, float(kind + 2)), gx, gy).rgb;
            c += t0 - r;
        }
    }
    c.a = clamp(c.a, 0.0, 1.0);
    c.rgb = clamp(c.rgb, vec3(0.0), vec3(c.a));
    return c;
}

void main()
{
    vec2 hh = nshvc_half(v_p);    /* before anything discards */
    ivec2 k;
    /* the footprint: across, from this row's matrix; down, by the true step between rows */
    vec2 w, extra = row_step(v_p) - vec2(u.f[4].y, u.f[5].y);
    vec2 dx = dFdx(v_t) + extra * dFdx(v_p.y), dy = dFdy(v_t) + extra * dFdy(v_p.y);
    float lx = length(dx), ly = length(dy);
    vec4 acc = vec4(0.0);
    /* the texel point with this row's own rounding; the span across a pixel, the matrix's */
    vec2 t = v_t + m7_line_round(int(floor(v_p.y)));
    sharp_at(t, fwidth(v_t), k, w);
    if (u.i[7].x != 0 && max(lx, ly) > 1.0) {
        acc = filtered(t, dx, dy);
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
    o = nshvc_maths(acc.rgb / acc.a, u.f[3].x, v_p, hh);
}
