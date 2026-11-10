#version 450
#extension GL_GOOGLE_include_directive : require
/* list: a draw list's triangles or lines, seven words a vertex: x, y, z, w,
   u, v (floats) and 0xAARRGGBB. Its vertex shader is also the game's
   shaders' (tools/nshvc-shaders), whose inputs match these outputs.
   f[2]: the rectangle it is kept to (the draw's scissor); f[3].x: alpha written
   f[4], f[5]: the material's eight inputs
   f[6]: screen pixels a list pixel across and down, the depth of z 0 and of z 1
   i[0]: the first vertex's word, the image's width and height, 1 if it has an image (tc0) */
#include "common.glsl"
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_colour;
void main()
{
    int k = u.i[0].x + gl_VertexIndex * 7;
    float w = wordf(k + 3);
    uint c = word(k + 6);
    if (w == 0.0)
        w = 1.0;
    vec2 p = vec2(wordf(k), wordf(k + 1)) * u.f[6].xy;
    gl_Position = clip_at(p, mix(u.f[6].z, u.f[6].w, wordf(k + 2))) * w;
    v_uv = vec2(wordf(k + 4), wordf(k + 5));
    v_colour = vec4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), float(c >> 24)) / 255.0;
}
