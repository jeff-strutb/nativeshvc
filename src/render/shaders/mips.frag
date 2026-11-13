#version 450
layout(binding = 2) uniform sampler2D tc0;
/* A level of a plane in colour is made here and stored as 8-bit unorm (BGRA8: every backend's colour texture). The rounding
   is the shader's, not the GPU's: each fetched channel is taken as the integer round(v * 255) (a stored k reads back as
   k / 255 to within a float's last place, so the rounding recovers k), the four are summed, the mean is (sum + 2) >> 2 (the
   mean rounded half up: a tie goes up on every GPU), and it is written as k / 255.0. The conversion of that to 8 bits
   is round(f * 255) on every conforming GPU (Vulkan and Metal specify round to nearest; Direct3D allows 0.6 of a step):
   f * 255 is within 1e-4 of the whole number k (float's relative error 1.2e-7 of at most 255, a division's one more
   place), 5,000 times nearer k than any tie, so every rounding gives k and has nothing to decide. */
layout(location = 0) out vec4 o;
ivec4 whole(vec4 v) { return ivec4(floor(v * 255.0 + 0.5)); }
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) * 2, last = textureSize(tc0, 0) - 1;
    ivec4 sum = whole(texelFetch(tc0, min(p, last), 0)) + whole(texelFetch(tc0, min(p + ivec2(1, 0), last), 0)) +
                whole(texelFetch(tc0, min(p + ivec2(0, 1), last), 0)) + whole(texelFetch(tc0, min(p + ivec2(1, 1), last), 0));
    o = vec4((sum + 2) >> 2) / 255.0;
}
