#version 450
#extension GL_GOOGLE_include_directive : require
// The Vulkan backend's prelude to m7_body.glsl: the frame, the tables and
// the supersamples' offsets in the staging ring at the word offsets the
// push constants give, the planes in a 2D array image.
layout(std430, set = 0, binding = 0) readonly buffer Ring { uint w[]; };
layout(set = 0, binding = 1) uniform usampler2DArray planet;
layout(push_constant) uniform PC { int pw, ph, d0, d1, step_u, step_v, oP, oS, oPal, oVram, oPic, oCols, oRows, oDu, cpp, left, cvk, cvlen, oCv; } U;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
#define LD16(base, e) ((w[(base) + ((e) >> 1)] >> (16 * ((e) & 1))) & 0xFFFFu)
#define P_(i) uvec4(w[U.oP + 4 * (i)], w[U.oP + 4 * (i) + 1], w[U.oP + 4 * (i) + 2], w[U.oP + 4 * (i) + 3])
#define S_(i) int(w[U.oS + (i)])
#define PAL_(i) LD16(U.oPal, (i))
#define VRAM_(i) LD16(U.oVram, (i))
#define PIC_(i) w[U.oPic + (i)]
#define COL_(i) int(w[U.oCols + (i)])
#define ROW_(i) int(w[U.oRows + (i)])
#define PLANE_(x, y, l) texelFetch(planet, ivec3((x), (y), (l)), 0).r
#define CV_(i) ((w[U.oCv + ((i) >> 2)] >> (8 * ((i) & 3))) & 0xFFu)
#define U_pw U.pw
#define U_ph U.ph
#define U_d0 U.d0
#define U_d1 U.d1
#define U_step_u U.step_u
#define U_step_v U.step_v
#define U_cpp U.cpp
#define U_left U.left
#define U_cvk U.cvk
#define U_cvlen U.cvlen
#define U_du(k) int(w[U.oDu + (k)])
#define U_dv(k) int(w[U.oDu + 28 + (k)])
#define FRAG_X int(gl_FragCoord.x)
#define FRAG_Y int(gl_FragCoord.y)
#define OUT o
#include "m7_body.glsl"
