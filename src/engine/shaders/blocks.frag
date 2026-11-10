#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) flat in uvec2 v_e;
layout(location = 1) in vec2 v_p;
layout(location = 0) out vec4 o;
void main()
{
    int prow = row_palette(v_p), kind = u.i[6].x, bpp = u.i[3].x;
    uint c = v_e.x, pal = v_e.y;
    vec3 col;
    if (kind == 1)
        col = u.i[3].z != 0 ? direct(c, 0u) : palette(c, prow);
    else if (kind == 2)
        col = palette(c, prow);
    else if (bpp == 8)
        col = u.i[3].z != 0 ? direct(c, pal) : palette(c, prow);
    else if (bpp == 4)
        col = palette(pal * 16u + c, prow);
    else
        col = palette(uint(u.i[3].y) + pal * 4u + c, prow);
    o = vec4(col, u.f[3].x);
}
