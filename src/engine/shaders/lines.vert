#version 450
#extension GL_GOOGLE_include_directive : require
/* lines: a draw list's lines, each a quad one list pixel wide and reaching
   half a pixel past each end, six vertices a line. Its words, uniforms and
   outputs are list.vert's, with which the game's shaders and list.frag pair. */
#include "common.glsl"
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_colour;
layout(location = 2) noperspective out vec2 v_p;
void main()
{
    int line = gl_VertexIndex / 6;
    vec2 k = corner(gl_VertexIndex % 6);
    int a = u.i[0].x + line * 14, b = a + 7, e = k.x < 0.5 ? a : b;
    vec2 pa = vec2(wordf(a), wordf(a + 1)), pb = vec2(wordf(b), wordf(b + 1)), d = pb - pa;
    float w = wordf(e + 3);
    uint c = word(e + 6);
    vec2 along, across, p;
    if (w == 0.0)
        w = 1.0;
    along = dot(d, d) > 0.0 ? normalize(d) : vec2(1.0, 0.0);
    across = vec2(-along.y, along.x);
    p = (k.x < 0.5 ? pa - along * 0.5 : pb + along * 0.5) + across * (k.y - 0.5);
    v_p = p * u.f[6].xy;
    gl_Position = clip_at(v_p, mix(u.f[6].z, u.f[6].w, wordf(e + 2))) * w;
    v_uv = vec2(wordf(e + 4), wordf(e + 5));
    v_colour = vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) / 255.0;
}
