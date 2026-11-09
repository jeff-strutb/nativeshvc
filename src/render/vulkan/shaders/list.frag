#version 450
layout(std430, binding = 1) readonly buffer TB { uint t[]; };
layout(location = 0) in vec2 uv;
layout(location = 1) flat in uint colour;
layout(location = 0) out uint o;
layout(push_constant) uniform U { float w, h; uint base, tw, th, p0, p1, p2; vec4 inputs[2]; } u;
void main() {
    if (u.tw == 0u) { o = colour; return; }
    uint x = uint(clamp(int(floor(uv.x)), 0, int(u.tw) - 1)), y = uint(clamp(int(floor(uv.y)), 0, int(u.th) - 1));
    uint c = t[u.base + y * u.tw + x];
    if (c == 0u) discard;
    o = c;
}
