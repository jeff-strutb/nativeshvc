#version 450
layout(binding = 2) uniform sampler2D tc0;
layout(location = 0) out vec4 o;
void main() { o = texelFetch(tc0, ivec2(gl_FragCoord.xy), 0); }
