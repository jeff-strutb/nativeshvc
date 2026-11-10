#version 450
/* mips_layer: mips.* for a layer of an array, given as an array of that one layer */
layout(std140, binding = 0) uniform D { vec4 f[8]; ivec4 i[8]; } u;
void main()
{
    int i = gl_VertexIndex;
    vec2 k = vec2(i == 1 || i == 2 || i == 4 ? 1.0 : 0.0, i == 2 || i == 4 || i == 5 ? 1.0 : 0.0);
    gl_Position = vec4(k.x * 2.0 - 1.0, (k.y * 2.0 - 1.0) * ((int(u.f[0].w) & 1) != 0 ? 1.0 : -1.0), 0.0, 1.0);
}
