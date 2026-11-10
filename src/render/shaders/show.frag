#version 450
layout(std140, binding = 0) uniform D { vec4 f[8]; ivec4 i[8]; } u;
layout(binding = 2) uniform sampler2D tc0;
layout(location = 0) out vec4 o;
void main()
{
    /* the texel this pixel shows, the picture mirrored left to right if i[0].y, then
       turned i[0].x quarter turns clockwise */
    ivec2 p = ivec2(gl_FragCoord.xy), s = textureSize(tc0, 0), q = p;
    int r = u.i[0].x & 3;
    if (r == 1)
        q = ivec2(p.y, s.y - 1 - p.x);
    else if (r == 2)
        q = s - 1 - p;
    else if (r == 3)
        q = ivec2(s.x - 1 - p.y, p.x);
    if (u.i[0].y != 0)
        q.x = s.x - 1 - q.x;
    o = texelFetch(tc0, q, 0);
}
