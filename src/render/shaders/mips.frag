#version 450
layout(binding = 2) uniform sampler2D tc0;
layout(location = 0) out vec4 o;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy) * 2, last = textureSize(tc0, 0) - 1;
    o = (texelFetch(tc0, min(p, last), 0) + texelFetch(tc0, min(p + ivec2(1, 0), last), 0) + texelFetch(tc0, min(p + ivec2(0, 1), last), 0) +
         texelFetch(tc0, min(p + ivec2(1, 1), last), 0)) * 0.25;
}
