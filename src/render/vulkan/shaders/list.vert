#version 450
layout(location = 0) in vec4 a;
layout(location = 1) in uint colour;
layout(location = 0) out vec2 uv;
layout(location = 1) flat out uint c;
layout(push_constant) uniform U { float w, h; uint base, tw, th, p0, p1, p2; vec4 inputs[2]; } u;
void main() { gl_Position = vec4(a.x / u.w * 2.0 - 1.0, a.y / u.h * 2.0 - 1.0, 0.0, 1.0); uv = a.zw; c = colour; }
