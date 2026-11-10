#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) flat in uint v_tile;
layout(location = 0) out uint o;
void main()
{
    int cols = u.i[0].z, tpp = u.i[0].w, s = 8 * tpp;
    ivec2 p = ivec2(gl_FragCoord.xy) & 7;
    /* a tile past the set's reads 0, as tile_texel's */
    o = int(v_tile) / cols * s >= textureSize(ti1, 0).y ? 0u
      : texelFetch(ti1, ivec2(int(v_tile) % cols * s + p.x * tpp + tpp / 2, int(v_tile) / cols * s + p.y * tpp + tpp / 2), 0).r;
}
