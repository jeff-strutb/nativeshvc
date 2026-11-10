#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
layout(location = 0) in vec2 v_p;
layout(location = 1) flat in int v_in;
layout(location = 0) out vec4 o;

uvec3 channels(vec4 c) { return uvec3(c.rgb * 255.0 + 0.5); }

/* a region's mode: 0 never, 1 outside the window, 2 inside, 3 always */
bool region(int mode, bool inside) { return mode == 3 || (mode == 1 && !inside) || (mode == 2 && inside); }

void main()
{
    ivec2 at = ivec2(gl_FragCoord.xy);
    int bright = u.i[2].z;
    bool even = u.i[3].x != 0 && fract(v_p.x) < 0.5;
    vec4 m = texelFetch(tc0, at, 0);
    vec4 other = even ? texelFetch(tc2, at, 0) : texelFetch(tc1, at, 0);
    bool inside = v_in != 0, clip = region(u.i[2].x, inside), prevent = region(u.i[2].y, inside);
    bool maths = u.i[1].w != 0 && m.a >= 0.5 && !prevent;
    uvec3 c = clip ? uvec3(0u) : channels(m), r;
    if (u.i[2].w != 0 || (u.i[3].x != 0 && v_p.x < 0.5)) {
        o = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if (!maths && bright == 15) {
        o = vec4(vec3(c) / 255.0, 1.0);
        return;
    }
    {
        /* each channel in eighths of a 5-bit step: a 15-bit colour's are its
           own, exactly, and are halved and dimmed in whole steps as the SNES
           does; a colour past 15 bits (eighths in it) keeps its eighths */
        uvec3 z = c - (c >> 5u);
        bvec3 fine = notEqual(z & 7u, uvec3(0u));
        if (maths) {
            bool halve = u.i[1].y != 0 && !clip;
            uvec3 y;
            if (u.i[1].z != 0) {
                y = uvec3(u.i[3].y & 31, (u.i[3].y >> 5) & 31, (u.i[3].y >> 10) & 31) << 3u;
            } else if (other.a < 0.5) {
                /* the other screen's backdrop: the fixed colour, not halved */
                y = uvec3(u.i[3].y & 31, (u.i[3].y >> 5) & 31, (u.i[3].y >> 10) & 31) << 3u;
                halve = false;
            } else {
                uvec3 oc = channels(other);
                y = oc - (oc >> 5u);
                fine = bvec3(uvec3(fine) | uvec3(notEqual(y & 7u, uvec3(0u))));
            }
            if (u.i[1].x != 0)
                z = uvec3(greaterThan(z, y)) * (z - y);
            else
                z = z + y;
            if (halve)
                z = mix((z >> 4u) << 3u, z >> 1u, fine);
            z = min(z, uvec3(248u));
        }
        if (bright != 15)
            z = mix(((((z >> 3u) * uint(bright) + 7u) * 4370u) >> 16u) << 3u, ((z * uint(bright) + 7u) * 4370u) >> 16u, notEqual(z & 7u, uvec3(0u)));
        r = z + (z >> 5u);
    }
    o = vec4(vec3(r) / 255.0, 1.0);
}
