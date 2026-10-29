#version 450
// Sharp bilinear: whole texels at any scale, blended only across the one
// target pixel a texel edge falls in (render/rr_size.h rr_ref_image): the
// sample point from the target pixel's own place and the texels a target
// pixel (an interpolated coordinate and fwidth fall to either side of a
// texel's edge), the four texels blended as rr_mix does, across then down,
// each rounded (the sampler's own blend weighs in fewer bits): 2 off a
// channel at a hard edge otherwise.
layout(set = 0, binding = 0) uniform sampler2D t;
layout(push_constant) uniform SU { float d0, d1, dx, dy; } U;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
void main() {
  vec2 dd = max(vec2(U.dx, U.dy), vec2(1e-5));
  vec2 px = (gl_FragCoord.xy - vec2(U.d0, U.d1)) * vec2(U.dx, U.dy) - 0.5;
  vec2 i = floor(px), f = px - i;
  f = clamp((f - 0.5) / dd + 0.5, 0.0, 1.0);
  ivec2 hi = textureSize(t, 0) - 1, a = clamp(ivec2(i), ivec2(0), hi), b = clamp(ivec2(i) + 1, ivec2(0), hi);
  vec3 c00 = round(texelFetch(t, ivec2(a.x, a.y), 0).rgb * 255.0), c01 = round(texelFetch(t, ivec2(b.x, a.y), 0).rgb * 255.0);
  vec3 c10 = round(texelFetch(t, ivec2(a.x, b.y), 0).rgb * 255.0), c11 = round(texelFetch(t, ivec2(b.x, b.y), 0).rgb * 255.0);
  vec3 top = floor(c00 + (c01 - c00) * f.x + 0.5), bot = floor(c10 + (c11 - c10) * f.x + 0.5);
  o = vec4(floor(top + (bot - top) * f.y + 0.5) / 255.0, 1.0);
}
