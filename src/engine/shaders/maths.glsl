/* maths.glsl: the colour maths, brightness and blanking of the console's
 * picture, done where a layer's colour is written (a fragment stage, FRAG),
 * on the final colour of the layer's own pixel and what the other screen
 * shows there: each channel in 5-bit steps, or eighths of one for colours
 * past 15 bits, added or taken away, halved, kept to the range, then dimmed.
 *
 * The includer names, by macro, what it reads: MATHS_U the draw's uniform
 * block (common.glsl u), MATHS_DATA the frame's words (data), MATHS_SUB and
 * MATHS_AUX the sub screen and the main screen alone, as resolved textures
 * (bindings 11 and 12). Every name here is nshvc_ prefixed: a game's
 * shader (tools/nshvc-shaders) includes this beside its own code.
 *
 * Settings, ordinary float values in the uniform block that are whole
 * numbers (exact): MATHS_U.f[1].z, bits:
 *   0      a main screen's draw (the maths is done); clear, the colour is
 *          written as it is, with the layer's flag as its alpha (the sub and
 *          aux screens', and what is no layer's)
 *   1      subtract           2   half               3   the fixed colour in place of the sub screen
 *   4      any layer takes part in the maths
 *   5-6    the clip region     7-8  the prevent region   (0 never, 1 outside the colour window, 2 inside, 3 always)
 *   9-12   15 less the brightness
 *   13     an even half-column of a hires row (the sub screen's layers: the draw's
 *          partner is the main screen), else the main screen's
 *   14     a region is the colour window's: its bit table is read
 *   15     a hires row (pseudo-hires too): half-columns
 * Hires: the console makes each dot's two half-columns in this order: the even,
 * the sub screen's pixel of dot p, with the partner and flags the odd half-column
 * of dot p - 1 left behind (the main screen's pixel there, whether its layer takes
 * part in the maths, its clip and prevent regions at that column, the sub
 * screen's transparency one dot back); then the odd, the main screen's pixel of dot
 * p, with the sub screen's pixel of dot p. So the other screen's texel is read one
 * half-column to the left (for the even half-column's transparency, two), and the
 * aux target's alpha holds the main layer's flag. The half-column's width in window
 * pixels, nshvc_half, is worked out from the picture position's own change over the
 * window's pixels, so it is exact at any size and turn; the texel is the one the
 * point one half-column over falls in, which is the same place in the half-column
 * on the left. A fragment shader calls nshvc_half before anything can discard.
 *
 * MATHS_U.f[1].w: the fixed colour, 15-bit BGR. The colour window's table, a
 * bit a picture pixel, rows of (width + 31) / 32 words, set where inside the
 * colour window, starts at the word MATHS_DATA.w[MATHS_U.i[7].w - 1] (the word
 * before the rows table), read only when bit 14 is set. */

uvec3 nshvc_channels(vec3 c) { return uvec3(clamp(c, 0.0, 1.0) * 255.0 + 0.5); }

/* a region's mode: 0 never, 1 outside the window, 2 inside, 3 always */
bool nshvc_region(int mode, bool inside) { return mode == 3 || (mode == 1 && !inside) || (mode == 2 && inside); }

/* whether picture point p is inside the colour window */
bool nshvc_inside(vec2 p)
{
    int w = int(MATHS_U.f[1].x), h = int(MATHS_U.f[1].y), per = (w + 31) / 32;
    int x = clamp(int(p.x), 0, w - 1), y = clamp(int(p.y), 0, h - 1);
    return ((MATHS_DATA.w[MATHS_DATA.w[MATHS_U.i[7].w - 1] + uint(y * per + (x >> 5))] >> uint(x & 31)) & 1u) != 0u;
}

/* the window displacement of one half-column to the left, for a main screen's draw on a hires row
   (else 0): the picture point's change over a window pixel inverted. Not after a discard: it takes
   derivatives */
vec2 nshvc_half(vec2 p)
{
    if ((int(MATHS_U.f[1].z) & 0x8001) != 0x8001)
        return vec2(0.0);
    vec2 a = dFdx(p), b = dFdy(p);
    return mat2(b.y, -a.y, -b.x, a.x) * vec2(-0.5, 0.0) / (a.x * b.y - b.x * a.y);
}

/* the texel of t at window point f */
vec4 nshvc_texel(sampler2D t, vec2 f)
{
    return texelFetch(t, clamp(ivec2(floor(f)), ivec2(0), textureSize(t, 0) - 1), 0);
}

/* a layer's pixel at picture point p: colour col (0-1), its flag (1 if it takes
   part in the maths), hh = nshvc_half(p) -> the pixel the screen shows (alpha 1), or as written */
vec4 nshvc_maths(vec3 col, float flag, vec2 p, vec2 hh)
{
    int st = int(MATHS_U.f[1].z);
    if ((st & 1) == 0)
        return vec4(col, flag);
    bool subtract = (st & 2) != 0, half_ = (st & 4) != 0, use_fixed = (st & 8) != 0;
    bool hires = (st & (1 << 15)) != 0, even = (st & (1 << 13)) != 0;
    int bright = 15 - ((st >> 9) & 15), fixed_ = int(MATHS_U.f[1].w);
    vec2 f = gl_FragCoord.xy;
    vec4 aux = vec4(0.0);
    /* an even half-column has the previous dot's regions and flag: its clip and prevent are those of the column
       before, its flag the main pixel's there (the aux target's alpha) */
    bool inside = (st & (1 << 14)) != 0 && nshvc_inside(even ? p - vec2(1.0, 0.0) : p);
    bool clip = nshvc_region((st >> 5) & 3, inside), prevent = nshvc_region((st >> 7) & 3, inside);
    if (even) {
        aux = nshvc_texel(MATHS_AUX, f + hh);
        flag = aux.a;
    }
    bool maths = (st & 16) != 0 && flag >= 0.5 && !prevent;
    uvec3 c = clip ? uvec3(0u) : nshvc_channels(col), r;
    vec4 other = vec4(0.0);
    bool transparent = false;
    if (!maths && bright == 15)
        return vec4(vec3(c) / 255.0, 1.0);
    if (maths && !use_fixed) {
        if (even) {
            /* the partner is the main pixel one half-column back; whether the sub screen was transparent is
               the previous dot's too (two half-columns back) */
            other = vec4(aux.rgb, 1.0);
            transparent = nshvc_texel(MATHS_SUB, f + 2.0 * hh).a < 0.5;
        } else {
            other = nshvc_texel(MATHS_SUB, hires ? f + hh : f);
            transparent = other.a < 0.5;
        }
    }
    {
        /* each channel in eighths of a 5-bit step: a 15-bit colour's are its
           own, exactly, and are halved and dimmed in whole steps as the SNES
           does; a colour past 15 bits (eighths in it) keeps its eighths */
        uvec3 z = c - (c >> 5u);
        bvec3 fine = notEqual(z & 7u, uvec3(0u));
        if (maths) {
            bool halve = half_ && !clip;
            uvec3 y;
            if (use_fixed) {
                y = uvec3(fixed_ & 31, (fixed_ >> 5) & 31, (fixed_ >> 10) & 31) << 3u;
            } else if (transparent) {
                /* the other screen's backdrop: the fixed colour, not halved */
                y = uvec3(fixed_ & 31, (fixed_ >> 5) & 31, (fixed_ >> 10) & 31) << 3u;
                halve = false;
            } else {
                uvec3 oc = nshvc_channels(other.rgb);
                y = oc - (oc >> 5u);
                fine = bvec3(uvec3(fine) | uvec3(notEqual(y & 7u, uvec3(0u))));
            }
            if (subtract)
                z = uvec3(greaterThan(z, y)) * (z - y);
            else
                z = z + y;
            if (halve)
                z = mix((z >> 4u) << 3u, z >> 1u, fine);
            z = min(z, uvec3(248u));
        }
        if (bright != 15)
            z = mix(((((z >> 3u) * uint(bright) + 7u) * 4370u) >> 16u) << 3u, ((z * uint(bright) + 7u) * 4370u) >> 16u, fine);
        r = z + (z >> 5u);
    }
    return vec4(vec3(r) / 255.0, 1.0);
}
