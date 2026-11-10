#version 450
/* show: a texture copied to the target a pixel for a pixel, mirrored left to right
   if i[0].y, then turned i[0].x quarter turns clockwise (the target is then the texture
   turned). f[0].xy: the target's size; f[0].w: the backend's code, bit 0 its clip y is down
   (the engine leaves i[0] 0: its targets are drawn turned, not turned after) */
layout(std140, binding = 0) uniform D { vec4 f[8]; ivec4 i[8]; } u;
void main()
{
    int i = gl_VertexIndex;
    vec2 k = vec2(i == 1 || i == 2 || i == 4 ? 1.0 : 0.0, i == 2 || i == 4 || i == 5 ? 1.0 : 0.0);
    gl_Position = vec4(k.x * 2.0 - 1.0, (k.y * 2.0 - 1.0) * ((int(u.f[0].w) & 1) != 0 ? 1.0 : -1.0), 0.0, 1.0);
}
