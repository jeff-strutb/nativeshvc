#version 450
/* mips: a texture's level made from the one before (binding 2), each texel
   the mean of the four it covers; f[0].xy: the level's size */
layout(std140, binding = 0) uniform D { vec4 f[8]; ivec4 i[8]; } u;
void main()
{
    int i = gl_VertexIndex;
    vec2 k = vec2(i == 1 || i == 2 || i == 4 ? 1.0 : 0.0, i == 2 || i == 4 || i == 5 ? 1.0 : 0.0);
    gl_Position = vec4(k.x * 2.0 - 1.0, (k.y * 2.0 - 1.0) * u.f[0].w, 0.0, 1.0);
}
