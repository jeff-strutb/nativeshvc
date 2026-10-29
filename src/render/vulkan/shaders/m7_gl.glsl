#version 330 core
// The OpenGL backend's prelude to m7_body.glsl: the frame in buffer
// textures, the planes in a 2D array texture, the presentation in uniforms.
uniform usamplerBuffer Pb, palb, vramb, picb;
uniform isamplerBuffer Sb, colsb, rowsb;
uniform usampler2DArray planet;
uniform usamplerBuffer cvb;
uniform int U_pw, U_ph, U_d0, U_d1, U_step_u, U_step_v, U_rh, U_cpp, U_left, U_cvk, U_cvlen, U_cvw, U_cvh;
uniform int U_du_[28], U_dv_[28];
in vec2 uv;
out vec4 o;
#define P_(i) texelFetch(Pb, (i))
#define S_(i) texelFetch(Sb, (i)).r
#define PAL_(i) texelFetch(palb, (i)).r
#define VRAM_(i) texelFetch(vramb, (i)).r
#define PIC_(i) texelFetch(picb, (i)).r
#define COL_(i) texelFetch(colsb, (i)).r
#define ROW_(i) texelFetch(rowsb, (i)).r
#define PLANE_(x, y, l) texelFetch(planet, ivec3((x), (y), (l)), 0).r
#define CV_(i) texelFetch(cvb, (i)).r
#define U_du(k) U_du_[k]
#define U_dv(k) U_dv_[k]
#define FRAG_X int(gl_FragCoord.x)
#define FRAG_Y (U_rh - 1 - int(gl_FragCoord.y))
#define OUT o
