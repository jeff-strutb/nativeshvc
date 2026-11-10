/* list_fs.glsl: the fragment stage of a draw list's triangles (list.frag) and lines (lines.frag) */
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_colour;
layout(location = 0) out vec4 o;
void main()
{
    vec4 c = v_colour;
    ivec2 k, last = u.i[0].yz - 1;
    vec2 w;
    sharp(v_uv, k, w);
    if (u.i[0].w != 0) {
        /* the image's texels, sharp, mixed by their alpha: full colour (tc0),
           or indexed (ti0) through the palette (tc1), 0 see-through */
        vec4 acc = vec4(0.0);
        for (int j = 0; j < 2; j++)
            for (int i = 0; i < 2; i++) {
                float wt = share(w, i, j);
                ivec2 q = clamp(k - 1 + ivec2(i, j), ivec2(0), last);
                vec4 t;
                if (wt <= 0.0)
                    continue;
                if (u.i[1].x != 0) {
                    uint x = texelFetch(ti0, q, 0).r;
                    t = x != 0u ? vec4(texelFetch(tc1, ivec2(int(x), u.i[1].y), 0).rgb, 1.0) : vec4(0.0);
                } else {
                    t = texelFetch(tc0, q, 0);
                }
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
