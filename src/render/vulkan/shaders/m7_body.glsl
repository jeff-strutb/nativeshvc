// A frame with Mode 7 presented (render/rr_mode7.h, the same arithmetic),
// the GLSL shared by the OpenGL and Vulkan backends (shaders/build.sh
// writes it into both). The prelude before it gives the data's reads:
//   P_(i) S_(i) PAL_(i) VRAM_(i) PIC_(i) COL_(i) ROW_(i)  the record, the
//     states' words, palettes, VRAM, the picture and the tables, by index
//   PLANE_(x, y, layer)  a plane's pixel byte; U_pw U_ph U_d0 U_d1
//   U_step_u U_step_v U_du(k) U_dv(k)  the presentation; FRAG_X FRAG_Y the
//   target pixel (top left origin); OUT the colour written
uint m7_direct(uint c) { return (c & 7u) << 2 | ((c & 0x38u) >> 1) << 5 | ((c & 0xC0u) >> 3) << 10; }
uint m7_finish(uint mc, uint ml, uint sc, uint sl, uint fl, uint adsub, uint wsel, uint fixc, uint bright) {
    bool clip = (fl & 0x80u) != 0u, prevent = (fl & 0x100u) != 0u;
    uint bit = ml == 5u ? 5u : ml == 6u ? 8u : ml, c = clip ? 0u : mc, r = c & 31u, g = (c >> 5) & 31u, b = (c >> 10) & 31u;
    if (bit < 8u && ((adsub >> bit) & 1u) != 0u && !prevent) {
        uint o; bool halve = (adsub & 0x40u) != 0u && !clip;
        if ((wsel & 2u) != 0u) { if (sl == 5u) { o = fixc; halve = false; } else o = sc; } else o = fixc;
        if ((adsub & 0x80u) != 0u) {
            int rr = int(r) - int(o & 31u), gg = int(g) - int((o >> 5) & 31u), bb = int(b) - int((o >> 10) & 31u);
            r = rr < 0 ? 0u : uint(rr); g = gg < 0 ? 0u : uint(gg); b = bb < 0 ? 0u : uint(bb);
            if (halve) { r >>= 1; g >>= 1; b >>= 1; }
        } else {
            r += o & 31u; g += (o >> 5) & 31u; b += (o >> 10) & 31u;
            if (halve) { r >>= 1; g >>= 1; b >>= 1; }
            r = min(r, 31u); g = min(g, 31u); b = min(b, 31u);
        }
    }
    if (bright != 15u) { r = ((r * bright + 7u) * 4370u) >> 16; g = ((g * bright + 7u) * 4370u) >> 16; b = ((b * bright + 7u) * 4370u) >> 16; }
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}
int s16(int v) { return (v << 16) >> 16; }
uint m7_ptexel(int layer, int v, uint sel, int px, int py) {
    if (((px | py) & ~0x3FF) != 0) {
        if ((sel & 0xC0u) == 0x80u) return 0u;
        if ((sel & 0xC0u) == 0xC0u) return VRAM_(v + (py & 7) * 8 + (px & 7)) >> 8;
    }
    return PLANE_(px & 1023, py & 1023, layer);
}
uint m7_mix(uint a, uint b, float t) {
    uint o = 0xFF000000u;
    for (int s = 0; s < 24; s += 8) { float ca = float((a >> s) & 0xFFu), cb = float((b >> s) & 0xFFu); o |= uint(int(ca + (cb - ca) * t + 0.5)) << s; }
    return o;
}
// an exact floor division without the GPU's integer divide: a float
// quotient corrected by one
int m7_fdiv(int a, int b) {
    int q = int(floor(float(a) / float(b)));
    if (q * b > a) q--; else if ((q + 1) * b <= a) q++;
    return q;
}
// the next texel's part of the footprint past its edge (rr_m7_edge)
int m7_edge(int f, int sa, int sb) {
    int A = max(sa, sb), B = min(sa, sb), T = 2 * (128 - f), cov;
    if (A == 0) return T < 0 ? 256 : T > 0 ? 0 : 128;
    if (T <= -(A + B)) cov = 0;
    else if (B > 0 && T <= -(A - B)) cov = m7_fdiv(256 * (T + A + B) * (T + A + B), 8 * A * B);
    else if (T < A - B || B == 0) cov = 128 + m7_fdiv(128 * T, A);
    else if (T < A + B) cov = 256 - m7_fdiv(256 * (A + B - T) * (A + B - T), 8 * A * B);
    else cov = 256;
    return 256 - clamp(cov, 0, 256);
}
// a picture pixel's colour with Mode 7's pixel byte pix (rr_m7_compose):
// its record p, its state's words from g, its palette from pl
uint m7_compose(uvec4 p, int g, int pl, uint pix) {
    uint fl = p.z & 0xFFFFu, pix1 = (fl & 0x200u) != 0u ? (p.w & 0xFFu) : pix, pix2 = (fl & 0x400u) != 0u ? ((p.w >> 8) & 0xFFu) : pix;
    uint mc = p.x & 0xFFFFu, sc = p.x >> 16, mz = p.y & 0xFFu, sz = (p.y >> 8) & 0xFFu, ml = (p.y >> 16) & 0xFFu, sl = p.y >> 24;
    uint g4 = uint(S_(g + 4)), g5 = uint(S_(g + 5));
    if (pix1 != 0u) {
        uint col = ((g4 >> 8) & 1u) != 0u ? m7_direct(pix1) : PAL_(pl + int(pix1));
        if ((fl & 0x08u) != 0u && 3u > mz) { mc = col; mz = 3u; ml = 0u; }
        if ((fl & 0x10u) != 0u && 3u > sz) { sc = col; sz = 3u; sl = 0u; }
    }
    if ((pix2 & 0x7Fu) != 0u) {
        uint col = PAL_(pl + int(pix2 & 0x7Fu)), z = (pix2 & 0x80u) != 0u ? 5u : 1u;
        if ((fl & 0x20u) != 0u && z > mz) { mc = col; mz = z; ml = 1u; }
        if ((fl & 0x40u) != 0u && z > sz) { sc = col; sz = z; sl = 1u; }
    }
    if ((fl & 4u) != 0u) { uint t = mc; mc = sl == 5u ? PAL_(pl) : sc; sc = t; t = ml; ml = sl; sl = t; }
    return m7_finish(mc, ml, sc, sl, fl, (g4 >> 16) & 0xFFu, g4 >> 24, g5 & 0xFFFFu, (g5 >> 16) & 0xFFu);
}
// picture pixels (i, r) and (i + 1, r) (two) over the output pixel centred
// at u, v: their Mode 7 plane filtered by the footprint (rr_m7_q). Both are
// looked up at the same position, so on one line state their texels are
// the same: the plane is read once and its texels composed for each (once
// where their records are the same), each byte composed once (a byte as
// the last one's: its colour). Two states (a line's segments): the second
// pixel alone, by a second call (one).
uvec2 m7_row(int i, bool two, bool one, int r, int u, int v) {
    r = clamp(r, 0, U_ph - 1);
    int i0 = clamp(i, 0, U_pw - 1), i1 = clamp(i + 1, 0, U_pw - 1);
    if (one) i0 = i1;
    uvec4 p0 = P_(r * U_pw + i0), p1 = two ? P_(r * U_pw + i1) : p0;
    uint fl0 = p0.z & 0xFFFFu, fl1 = p1.z & 0xFFFFu;
    bool same = all(equal(p0, p1));
    uint k0 = (fl0 & 1u) != 0u ? PIC_(r * U_pw + i0) : 0xFF000000u, k1 = (fl1 & 1u) != 0u ? PIC_(r * U_pw + i1) : 0xFF000000u;
    bool m0 = (fl0 & 3u) == 0u, m1 = two && (fl1 & 3u) == 0u && !same;
    if (!m0 && !m1) return uvec2(k0, same ? k0 : k1);
    uvec4 pp = m0 ? p0 : p1;
    bool split = m0 && m1 && (p0.z >> 16) != (p1.z >> 16);
    if (split) m1 = false;
    int g = int(pp.z >> 16) * 8, pl = int(S_(g + 6)) * 256;
    bool l0 = m0 && (fl0 & 0x78u) != 0u && (fl0 & 0x600u) != 0x600u, l1 = m1 && (fl1 & 0x78u) != 0u && (fl1 & 0x600u) != 0x600u;
    uint g4 = uint(S_(g + 4)), g5 = uint(S_(g + 5)), sel = g4 & 0xFFu;
    int layer = int(g5 >> 24), vr = layer * 0x8000;
    int g2 = S_(g + 2), g3 = S_(g + 3), a = s16(g2), b = g2 >> 16, c = s16(g3), d = g3 >> 16;
    int uu = u, ee = v - 1024 * r, sx = 1, sy = 1;
    if ((sel & 1u) != 0u) { uu = 255 * 1024 - uu; sx = -1; }
    if ((sel & 2u) != 0u) { ee = -ee; sy = -1; }
    int hx = S_(g) + a * (uu >> 10) + b * (ee >> 10), lx = a * (uu & 1023) + b * (ee & 1023);
    int hy = S_(g + 1) + c * (uu >> 10) + d * (ee >> 10), ly = c * (uu & 1023) + d * (ee & 1023);
    int ax = (abs(a) * U_step_u) >> 10, bx = (abs(b) * U_step_v) >> 10, ay = (abs(c) * U_step_u) >> 10, by = (abs(d) * U_step_v) >> 10;
    int span = max(ax + bx, ay + by);
    int n, base, ix = 0, iy = 0, ex = 0, ey = 0;
    float fx = 0.0, fy = 0.0;
    if (!l0 && !l1) { n = 1; base = -2; }
    else if (span <= 256) {
        int qx = hx + (lx >> 10) - 128, qy = hy + (ly >> 10) - 128;
        ix = qx >> 8; iy = qy >> 8; ex = m7_edge(qx & 255, ax, bx); ey = m7_edge(qy & 255, ay, by);
        if (ex == 256) { ix++; ex = 0; }
        if (ey == 256) { iy++; ey = 0; }
        fx = float(ex) / 256.0; fy = float(ey) / 256.0;
        n = (ex != 0 ? 2 : 1) * (ey != 0 ? 2 : 1); base = -1;
    } else {
        n = span <= 512 ? 4 : span <= 1024 ? 8 : 16; base = n == 4 ? 0 : n == 8 ? 4 : 12;
    }
    uint last = 0x100u, c0 = 0u, c1 = 0u, cur0 = 0u, cur1 = 0u, top0 = 0u, top1 = 0u;
    uvec3 s0 = uvec3(0u), s1 = uvec3(0u);
    for (int k = 0; k < n; k++) {
        int px, py, ci = 0, cj = 0;
        if (base < 0) { ci = ex != 0 ? (k & 1) : 0; cj = ex != 0 ? (k >> 1) : k; px = ix + ci; py = iy + cj; }
        else {
            int eu = sx * U_du(base + k), ev = sy * U_dv(base + k);
            px = (hx + ((lx + a * eu + b * ev) >> 10)) >> 8; py = (hy + ((ly + c * eu + d * ev) >> 10)) >> 8;
        }
        uint pix = base == -2 ? 0u : m7_ptexel(layer, vr, sel, px, py);
        if (pix != last) {
            last = pix;
            if (m0) c0 = m7_compose(p0, g, pl, l0 ? pix : 0u);
            if (m1) c1 = m7_compose(p1, g, pl, l1 ? pix : 0u);
        }
        if (base >= 0) {
            s0 += uvec3((c0 >> 16) & 0xFFu, (c0 >> 8) & 0xFFu, c0 & 0xFFu);
            s1 += uvec3((c1 >> 16) & 0xFFu, (c1 >> 8) & 0xFFu, c1 & 0xFFu);
            continue;
        }
        cur0 = ci != 0 ? m7_mix(cur0, c0, fx) : c0; cur1 = ci != 0 ? m7_mix(cur1, c1, fx) : c1;
        if (ex == 0 || ci != 0) { top0 = cj != 0 ? m7_mix(top0, cur0, fy) : cur0; top1 = cj != 0 ? m7_mix(top1, cur1, fy) : cur1; }
    }
    if (base >= 0) {
        uint sh = n == 4 ? 2u : n == 8 ? 3u : 4u; uvec3 h = uvec3(uint(n) / 2u);
        s0 = (s0 + h) >> sh; s1 = (s1 + h) >> sh;
        top0 = 0xFF000000u | s0.x << 16 | s0.y << 8 | s0.z; top1 = 0xFF000000u | s1.x << 16 | s1.y << 8 | s1.z;
    }
    uint r0 = m0 ? top0 : k0;
    if (!two || same) return uvec2(r0, r0);
    if (m1) return uvec2(r0, top1);
    return uvec2(r0, split ? 0xFFFFFFFFu : k1);   // split: the second pixel by a second call
}
uint m7_pic(int x, int y) { return PIC_(clamp(y, 0, U_ph - 1) * U_pw + clamp(x, 0, U_pw - 1)); }
// an output pixel: the picture pixels under it blended with sharp
// bilinear (rr_m7_pixel), a row of them at a time
void main() {
    int X = FRAG_X - U_d0, Y = FRAG_Y - U_d1;
    int tx = COL_(X * 3), ty = ROW_(Y * 4), u = COL_(X * 3 + 2), v = ROW_(Y * 4 + 3);
    float fx = intBitsToFloat(COL_(X * 3 + 1)), fy = intBitsToFloat(ROW_(Y * 4 + 1));
    bool m7 = ROW_(Y * 4 + 2) != 0, two = fx > 0.0;
    int nr = fy > 0.0 ? 2 : 1;
    uint top = 0u;
    for (int j = 0; j < nr; j++) {
        uint cur;
        if (m7) {
            uvec2 q = m7_row(tx, two, false, ty + j, u, v);
            if (q.y == 0xFFFFFFFFu) q.y = m7_row(tx, false, true, ty + j, u, v).x;
            cur = two ? m7_mix(q.x, q.y, fx) : q.x;
        } else {
            cur = two ? m7_mix(m7_pic(tx, ty + j), m7_pic(tx + 1, ty + j), fx) : m7_pic(tx, ty + j);
        }
        top = j != 0 ? m7_mix(top, cur, fy) : cur;
    }
    OUT = vec4(float((top >> 16) & 0xFFu), float((top >> 8) & 0xFFu), float(top & 0xFFu), 255.0) / 255.0;
}
