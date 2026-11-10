#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) out vec4 o;
void main() { o = texelFetch(tc0, ivec2(gl_FragCoord.xy), 0); }
