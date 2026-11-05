#version 450
// An overlay: bilinear, its alpha times the constant's kept for the blend.
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(push_constant) uniform P { float alpha; } p;
void main() {
    o = texture(t, v_uv);
    o.a *= p.alpha;
}
