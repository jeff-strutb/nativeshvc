/*
 * rr_metal.m: the renderer backend on Metal (macOS, iOS): the frame is
 * drawn into an offscreen target the size of the
 * window's drawable, then copied into the window's CAMetalLayer. Pictures
 * go on with the sharp bilinear shader (rr_size.h's weight, in the shader:
 * fwidth gives the texels to one target pixel); a frame with Mode 7 with
 * rr_mode7.h's arithmetic in a fragment shader. The shader source is
 * compiled at start-up, so the build needs no Metal toolchain step.
 */
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr.h"
#include "rr_size.h"
#include "rr_mode7.h"
#include "host.h"

CAMetalLayer *host_metal_layer(void);
void host_metal_layer_fit(int *w, int *h);
/* without a window host (HOST=null) there is no layer: frames are drawn
   offscreen and read back, never shown */
__attribute__((weak)) CAMetalLayer *host_metal_layer(void) { return nil; }
__attribute__((weak)) void host_metal_layer_fit(int *w, int *h) { *w = *h = 0; }

#include "rr_compose_src.h"

/* the compositor (compose_body.h): its functions members of a structure
   that holds the frame's description, a thread a picture pixel */
static const char k_compose_pre[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define CP_FN\n"
    "#define CP_VAR\n"
    "#define U32 uint\n"
    "#define I32 int\n"
    "#define U_(x) uint(x)\n"
    "#define I_(x) int(x)\n"
    "#define FD_(i) fd[cp_fdb + (i)]\n"
    "struct CP {\n"
    "device const uint *fd;\n";
static const char k_compose_post[] =
    "};\n"
    /* wp: the woven picture's width (0: none) and its descriptions' first words */
    "kernel void cpk(device const uint *fd [[buffer(0)]], texture2d<float, access::write> o [[texture(0)]],\n"
    "                constant uint4 &wp [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
    "  CP c; uint p;\n"
    "  c.fd = fd;\n"
    "  c.cp_fdb = 0u;\n"
    "  if (g.x >= o.get_width() || g.y >= o.get_height()) return;\n"
    "  p = wp.x == 0u ? c.cp_pixel(g.x, g.y) : c.cp_woven(g.x, g.y, wp.x, wp.y, wp.z);\n"
    "  o.write(float4(float((p >> 16) & 255u), float((p >> 8) & 255u), float(p & 255u), 255.0) / 255.0, g); }\n"
    /* a frame presenting Mode 7 or a canvas: its picture and records */
    "kernel void cprk(device const uint *fd [[buffer(0)]], device uint4 *rec [[buffer(1)]], device uint *pic [[buffer(2)]],\n"
    "                 uint2 g [[thread_position_in_grid]]) {\n"
    "  CP c; uint i;\n"
    "  c.fd = fd;\n"
    "  c.cp_fdb = 0u;\n"
    "  if (g.x >= fd[CP_PW] || g.y >= fd[CP_PH]) return;\n"
    "  i = g.y * fd[CP_PW] + g.x;\n"
    "  pic[i] = c.cp_pixel(g.x, g.y);\n"
    "  c.cp_record(g.x, g.y);\n"
    "  rec[i] = uint4(c.cp_r0, c.cp_r1, c.cp_r2, c.cp_r3); }\n";

static NSString *const k_src = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct BO { float4 pos [[position]]; float2 uv; };\n"
"vertex BO bvs(uint i [[vertex_id]], const device float4 *v [[buffer(0)]]) { BO o; o.pos = float4(v[i].xy, 0, 1); o.uv = v[i].zw; return o; }\n"
/* one to one */
"fragment float4 bfs(BO in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
"  constexpr sampler s(filter::nearest, address::clamp_to_edge);\n"
"  return float4(t.sample(s, in.uv).rgb, 1); }\n"
/* sharp bilinear: whole texels at any scale, blended only across the one
   target pixel a texel edge falls in (rr_size.h rr_ref_image): the sample
   point from the target pixel's own place and the texels a target pixel
   (an interpolated coordinate and fwidth fall to either side of a texel's
   edge), the four texels blended as rr_mix does, across then down, each
   rounded (the sampler's own blend weighs in fewer bits): 2 off a channel
   at a hard edge otherwise */
"struct SU { float d0, d1, dx, dy; };\n"
"fragment float4 sfs(BO in [[stage_in]], constant SU &U [[buffer(0)]], texture2d<float> t [[texture(0)]]) {\n"
"  float2 ts = float2(t.get_width(), t.get_height()), dd = max(float2(U.dx, U.dy), float2(1e-5));\n"
"  float2 px = (in.pos.xy - float2(U.d0, U.d1)) * float2(U.dx, U.dy) - 0.5;\n"
"  float2 i = floor(px), f = px - i;\n"
"  f = clamp((f - 0.5) / dd + 0.5, 0.0, 1.0);\n"
"  int2 hi = int2(ts) - 1, a = clamp(int2(i), int2(0), hi), b = clamp(int2(i) + 1, int2(0), hi);\n"
"  float3 c00 = round(t.read(uint2(a.x, a.y)).rgb * 255.0), c01 = round(t.read(uint2(b.x, a.y)).rgb * 255.0);\n"
"  float3 c10 = round(t.read(uint2(a.x, b.y)).rgb * 255.0), c11 = round(t.read(uint2(b.x, b.y)).rgb * 255.0);\n"
"  float3 top = floor(c00 + (c01 - c00) * f.x + 0.5), bot = floor(c10 + (c11 - c10) * f.x + 0.5);\n"
"  return float4(floor(top + (bot - top) * f.y + 0.5) / 255.0, 1); }\n"
/* an overlay: bilinear, its alpha kept for the blend */
"fragment float4 ofs(BO in [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
"  constexpr sampler s(filter::linear, address::clamp_to_edge);\n"
"  return t.sample(s, in.uv); }\n"
/* a frame with Mode 7 presented (rr_mode7.h, the same arithmetic) */
"struct M7U { int pw, ph, d0, d1, step_u, step_v, du[28], dv[28], cpp, left, cvk, cvlen, cvw, cvh; };\n"
"static uint m7_direct(uint c) { return (c & 7u) << 2 | ((c & 0x38u) >> 1) << 5 | ((c & 0xC0u) >> 3) << 10; }\n"
"static uint m7_finish(uint mc, uint ml, uint sc, uint sl, uint fl, uint adsub, uint wsel, uint fixed, uint bright) {\n"
"  bool clip = (fl & 0x80u) != 0, prevent = (fl & 0x100u) != 0;\n"
"  uint bit = ml == 5 ? 5 : ml == 6 ? 8 : ml, c = clip ? 0 : mc, r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;\n"
"  if (bit < 8 && (adsub >> bit & 1) && !prevent) {\n"
"    uint o; bool halve = (adsub & 0x40) != 0 && !clip;\n"
"    if (wsel & 2) { if (sl == 5) { o = fixed; halve = false; } else o = sc; } else o = fixed;\n"
"    if (adsub & 0x80) {\n"
"      int rr = int(r) - int(o & 31), gg = int(g) - int((o >> 5) & 31), bb = int(b) - int((o >> 10) & 31);\n"
"      r = rr < 0 ? 0 : uint(rr); g = gg < 0 ? 0 : uint(gg); b = bb < 0 ? 0 : uint(bb);\n"
"      if (halve) { r >>= 1; g >>= 1; b >>= 1; }\n"
"    } else {\n"
"      r += o & 31; g += (o >> 5) & 31; b += (o >> 10) & 31;\n"
"      if (halve) { r >>= 1; g >>= 1; b >>= 1; }\n"
"      r = min(r, 31u); g = min(g, 31u); b = min(b, 31u);\n"
"    }\n"
"  }\n"
"  if (bright != 15) { r = ((r * bright + 7u) * 4370u) >> 16; g = ((g * bright + 7u) * 4370u) >> 16; b = ((b * bright + 7u) * 4370u) >> 16; }\n"
"  return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2); }\n"
"static uint m7_ptexel(texture2d_array<uint> plane, uint layer, const device ushort *v, uint sel, int px, int py) {\n"
"  if ((px | py) & ~0x3FF) {\n"
"    if ((sel & 0xC0) == 0x80) return 0;\n"
"    if ((sel & 0xC0) == 0xC0) return uint(v[uint(py & 7) * 8 + uint(px & 7)]) >> 8;\n"
"  }\n"
"  return plane.read(uint2(px & 1023, py & 1023), layer).r; }\n"
"static uint m7_mix(uint a, uint b, float t) {\n"
"  uint o = 0xFF000000u;\n"
"  for (uint s = 0; s < 24; s += 8) { float ca = float((a >> s) & 0xFF), cb = float((b >> s) & 0xFF); o |= uint(int(ca + (cb - ca) * t + 0.5)) << s; }\n"
"  return o; }\n"
"static int m7_fdiv(int a, int b) {\n"
"  int q = int(floor(float(a) / float(b)));\n"
"  if (q * b > a) q--; else if ((q + 1) * b <= a) q++;\n"
"  return q; }\n"
/* the next texel's part of the footprint past its edge (rr_mode7.h rr_m7_edge) */
"static inline __attribute__((always_inline)) int m7_edge(int f, int sa, int sb) {\n"
"  int A = max(sa, sb), B = min(sa, sb), T = 2 * (128 - f), cov;\n"
"  if (A == 0) return T < 0 ? 256 : T > 0 ? 0 : 128;\n"
"  if (T <= -(A + B)) cov = 0;\n"
"  else if (B > 0 && T <= -(A - B)) cov = m7_fdiv(256 * (T + A + B) * (T + A + B), 8 * A * B);\n"
"  else if (T < A - B || B == 0) cov = 128 + m7_fdiv(128 * T, A);\n"
"  else if (T < A + B) cov = 256 - m7_fdiv(256 * (A + B - T) * (A + B - T), 8 * A * B);\n"
"  else cov = 256;\n"
"  return 256 - clamp(cov, 0, 256); }\n"
/* a picture pixel's colour with Mode 7's pixel byte pix (rr_m7_compose) */
"static inline __attribute__((always_inline)) uint m7_compose(uint4 p, const device int *g, const device ushort *pl, uint pix) {\n"
"  uint fl = p.z & 0xFFFFu, pix1 = (fl & 0x200u) ? (p.w & 0xFF) : pix, pix2 = (fl & 0x400u) ? ((p.w >> 8) & 0xFF) : pix;\n"
"  uint mc = p.x & 0xFFFF, sc = p.x >> 16, mz = p.y & 0xFF, sz = (p.y >> 8) & 0xFF, ml = (p.y >> 16) & 0xFF, sl = p.y >> 24;\n"
"  if (pix1) {\n"
"    uint col = ((uint(g[4]) >> 8) & 1) ? m7_direct(pix1) : uint(pl[pix1]);\n"
"    if ((fl & 0x08u) && 3 > mz) { mc = col; mz = 3; ml = 0; }\n"
"    if ((fl & 0x10u) && 3 > sz) { sc = col; sz = 3; sl = 0; }\n"
"  }\n"
"  if (pix2 & 0x7F) {\n"
"    uint col = pl[pix2 & 0x7F], z = (pix2 & 0x80) ? 5 : 1;\n"
"    if ((fl & 0x20u) && z > mz) { mc = col; mz = z; ml = 1; }\n"
"    if ((fl & 0x40u) && z > sz) { sc = col; sz = z; sl = 1; }\n"
"  }\n"
"  if (fl & 4u) { uint t = mc; mc = sl == 5 ? uint(pl[0]) : sc; sc = t; t = ml; ml = sl; sl = t; }\n"
"  return m7_finish(mc, ml, sc, sl, fl, (uint(g[4]) >> 16) & 0xFF, uint(g[4]) >> 24, uint(g[5]) & 0xFFFF, (uint(g[5]) >> 16) & 0xFF); }\n"
/* a canvas pixel's colour with the canvas's texel pix (rr_cv_compose) */
"static uint cv_direct(uint c, uint pal) {\n"
"  return ((c & 7u) << 2 | (pal & 1u) << 1) | ((c & 0x38u) >> 1 | (pal & 2u)) << 5 | ((c & 0xC0u) >> 3 | (pal & 4u)) << 10; }\n"
"static uint cv_compose(uint4 p, const device int *g, const device ushort *pl, uint pix) {\n"
"  uint fl = p.z & 0xFFFFu, w3 = p.w, bpp = uint(g[7]) & 0xFF, mode = (uint(g[7]) >> 8) & 7;\n"
"  uint pal = (w3 >> 22) & 7, z = (w3 >> 25) & 15, n = (w3 >> 29) & 3;\n"
"  uint mc = p.x & 0xFFFF, sc = p.x >> 16, mz = p.y & 0xFF, sz = (p.y >> 8) & 0xFF, ml = (p.y >> 16) & 0xFF, sl = p.y >> 24;\n"
"  pix &= (1u << bpp) - 1u;\n"
"  if (pix) {\n"
"    uint col = bpp == 8 ? (((uint(g[4]) >> 8) & 1) ? cv_direct(pix, pal) : uint(pl[pix])) :\n"
"               bpp == 4 ? uint(pl[pal * 16 + pix]) : uint(pl[(mode == 0 ? n * 32 : 0) + pal * 4 + pix]);\n"
"    if ((fl & 0x08u) && z > mz) { mc = col; mz = z; ml = n; }\n"
"    if ((fl & 0x10u) && z > sz) { sc = col; sz = z; sl = n; }\n"
"  }\n"
"  return m7_finish(mc, ml, sc, sl, fl, (uint(g[4]) >> 16) & 0xFF, uint(g[4]) >> 24, uint(g[5]) & 0xFFFF, (uint(g[5]) >> 16) & 0xFF); }\n"
/* canvas pixel (i, r) over the output pixel centred at u, v: the canvas's
   texel under the centre (rr_cv_q) */
"static uint cv_q(constant M7U &U, uint4 p, const device int *g, const device ushort *pl, const device uchar *cv, int i, int r, int u, int v) {\n"
"  uint w3 = p.w, k = uint(U.cvk);\n"
"  int h = i / U.cpp - U.left, fu = clamp(u - 1024 * h + 512, 0, 1023), fv = clamp(v - 1024 * r + 512, 0, 1023);\n"
"  uint sx = (uint(fu) * k) >> 10, sy = (uint(fv) * k) >> 10;\n"
"  if ((w3 >> 20) & 1) sx = k - 1 - sx;\n"
"  if ((w3 >> 21) & 1) sy = k - 1 - sy;\n"
"  if ((w3 >> 31) != 0u) {\n"
"    uint ex = uint(i / U.cpp) * k + sx, ey = uint(r) * k + sy;\n"
"    return cv_compose(p, g, pl, ex < uint(U.cvw) && ey < uint(U.cvh) ? uint(cv[ey * uint(U.cvw) + ex]) : 0u); }\n"
"  uint at = (w3 & 0x3FFF) * 64u * k * k + (((w3 >> 17) & 7) * k + sy) * 8u * k + ((w3 >> 14) & 7) * k + sx;\n"
"  return cv_compose(p, g, pl, at < uint(U.cvlen) ? uint(cv[at]) : 0u); }\n"
/* picture pixel (i, r) over the output pixel centred at u, v: its Mode 7
   plane filtered by the footprint (rr_m7_q); one loop over the texels
   it takes, each composed once (a byte as the last one's: its colour) */
"static inline __attribute__((always_inline)) uint m7_q(constant M7U &U, const device uint4 *P, const device int *S, const device ushort *pal,\n"
"                 const device ushort *vram, const device uint *pic, texture2d_array<uint> plane, const device uchar *cv, int i, int r, int u, int v) {\n"
"  i = clamp(i, 0, U.pw - 1); r = clamp(r, 0, U.ph - 1);\n"
"  uint4 p = P[r * U.pw + i]; uint fl = p.z & 0xFFFFu;\n"
"  if (fl & 1u) return pic[r * U.pw + i];\n"
"  if (fl & 2u) return 0xFF000000u;\n"
"  const device int *g = S + (p.z >> 16) * 8;\n"
"  const device ushort *pl = pal + uint(g[6]) * 256u;\n"
"  if (fl & 0x800u) return cv_q(U, p, g, pl, cv, i, r, u, v);\n"
"  bool look = (fl & 0x78u) && (fl & 0x600u) != 0x600u;\n"
"  uint sel = uint(g[4]) & 0xFF;\n"
"  const device ushort *vr = vram + (uint(g[5]) >> 24) * 0x8000u;\n"
"  int a = int(short(g[2] & 0xFFFF)), b = g[2] >> 16, c = int(short(g[3] & 0xFFFF)), d = g[3] >> 16;\n"
"  int uu = u, ee = v - 1024 * r, sx = 1, sy = 1;\n"
"  if (sel & 1) { uu = 255 * 1024 - uu; sx = -1; }\n"
"  if (sel & 2) { ee = -ee; sy = -1; }\n"
"  int hx = g[0] + a * (uu >> 10) + b * (ee >> 10), lx = a * (uu & 1023) + b * (ee & 1023);\n"
"  int hy = g[1] + c * (uu >> 10) + d * (ee >> 10), ly = c * (uu & 1023) + d * (ee & 1023);\n"
"  int ax = (abs(a) * U.step_u) >> 10, bx = (abs(b) * U.step_v) >> 10, ay = (abs(c) * U.step_u) >> 10, by = (abs(d) * U.step_v) >> 10;\n"
"  int w = max(ax + bx, ay + by);\n"
"  int n, base, ix = 0, iy = 0, ex = 0, ey = 0;\n"
"  float fx = 0, fy = 0;\n"
"  if (!look) { n = 1; base = -2; }\n"
"  else if (w <= 256) {\n"
"    int qx = hx + (lx >> 10) - 128, qy = hy + (ly >> 10) - 128;\n"
"    ix = qx >> 8; iy = qy >> 8; ex = m7_edge(qx & 255, ax, bx); ey = m7_edge(qy & 255, ay, by);\n"
"    if (ex == 256) { ix++; ex = 0; }\n"
"    if (ey == 256) { iy++; ey = 0; }\n"
"    fx = float(ex) / 256.0; fy = float(ey) / 256.0;\n"
"    n = (ex ? 2 : 1) * (ey ? 2 : 1); base = -1;\n"
"  } else {\n"
"    n = w <= 512 ? 4 : w <= 1024 ? 8 : 16; base = n == 4 ? 0 : n == 8 ? 4 : 12;\n"
"  }\n"
"  uint last = 0x100u, col = 0, cur = 0, top = 0, sr = 0, sg = 0, sb = 0;\n"
"  for (int k = 0; k < n; k++) {\n"
"    int px, py, ci = 0, cj = 0;\n"
"    if (base < 0) { ci = ex ? (k & 1) : 0; cj = ex ? (k >> 1) : k; px = ix + ci; py = iy + cj; }\n"
"    else {\n"
"      int eu = sx * U.du[base + k], ev = sy * U.dv[base + k];\n"
"      px = (hx + ((lx + a * eu + b * ev) >> 10)) >> 8; py = (hy + ((ly + c * eu + d * ev) >> 10)) >> 8;\n"
"    }\n"
"    uint pix = base == -2 ? 0u : m7_ptexel(plane, uint(g[5]) >> 24, vr, sel, px, py);\n"
"    if (pix != last) { col = m7_compose(p, g, pl, pix); last = pix; }\n"
"    if (base >= 0) { sb += col & 0xFF; sg += (col >> 8) & 0xFF; sr += (col >> 16) & 0xFF; continue; }\n"
"    cur = ci ? m7_mix(cur, col, fx) : col;\n"
"    if (!ex || ci) top = cj ? m7_mix(top, cur, fy) : cur;\n"
"  }\n"
"  if (base < 0) return top;\n"
"  uint sh = n == 4 ? 2 : n == 8 ? 3 : 4, h = uint(n) / 2;\n"
"  return 0xFF000000u | ((sr + h) >> sh) << 16 | ((sg + h) >> sh) << 8 | ((sb + h) >> sh); }\n"
/* picture pixels (i, r) and (i + 1, r) (two) over the output pixel centred
   at u, v: their Mode 7 plane filtered by the footprint (rr_m7_q). Both
   are looked up at the same position, so on one line state their texels
   are the same: the plane is read once and its texels composed for each
   (once where their records are the same), each texel's byte composed
   once (a byte as the last one's: its colour). */
"static inline __attribute__((always_inline)) uint2 m7_row(constant M7U &U, const device uint4 *P, const device int *S, const device ushort *pal,\n"
"                 const device ushort *vram, const device uint *pic, texture2d_array<uint> plane, const device uchar *cv, int i, bool two, int r, int u, int v) {\n"
"  r = clamp(r, 0, U.ph - 1);\n"
"  int i0 = clamp(i, 0, U.pw - 1), i1 = clamp(i + 1, 0, U.pw - 1);\n"
"  uint4 p0 = P[r * U.pw + i0], p1 = two ? P[r * U.pw + i1] : p0;\n"
"  uint fl0 = p0.z & 0xFFFFu, fl1 = p1.z & 0xFFFFu;\n"
/* a canvas pixel in the pair: each pixel on its own */
"  if ((fl0 | fl1) & 0x800u) {\n"
"    uint a = m7_q(U, P, S, pal, vram, pic, plane, cv, i, r, u, v);\n"
"    return uint2(a, two ? m7_q(U, P, S, pal, vram, pic, plane, cv, i + 1, r, u, v) : a); }\n"
/* the same record, the same colour: but a picture pixel's (not on such a
   line) is its own */
"  bool same = all(p0 == p1) && !(fl0 & 1u);\n"
/* a picture pixel not on a Mode 7 line, or a hires line's first half pixel */
"  uint k0 = (fl0 & 1u) ? pic[r * U.pw + i0] : 0xFF000000u, k1 = (fl1 & 1u) ? pic[r * U.pw + i1] : 0xFF000000u;\n"
"  bool m0 = !(fl0 & 3u), m1 = two && !(fl1 & 3u) && !same;\n"
"  if (!m0 && !m1) return uint2(k0, same ? k0 : k1);\n"
"  uint4 pp = m0 ? p0 : p1;\n"
"  if (m0 && m1 && (p0.z >> 16) != (p1.z >> 16)) m1 = false;      /* another state: below */\n"
"  const device int *g = S + (pp.z >> 16) * 8;\n"
"  const device ushort *pl = pal + uint(g[6]) * 256u;\n"
"  bool l0 = m0 && (fl0 & 0x78u) && (fl0 & 0x600u) != 0x600u, l1 = m1 && (fl1 & 0x78u) && (fl1 & 0x600u) != 0x600u;\n"
"  uint sel = uint(g[4]) & 0xFF;\n"
"  const device ushort *vr = vram + (uint(g[5]) >> 24) * 0x8000u;\n"
"  int a = int(short(g[2] & 0xFFFF)), b = g[2] >> 16, c = int(short(g[3] & 0xFFFF)), d = g[3] >> 16;\n"
"  int uu = u, ee = v - 1024 * r, sx = 1, sy = 1;\n"
"  if (sel & 1) { uu = 255 * 1024 - uu; sx = -1; }\n"
"  if (sel & 2) { ee = -ee; sy = -1; }\n"
"  int hx = g[0] + a * (uu >> 10) + b * (ee >> 10), lx = a * (uu & 1023) + b * (ee & 1023);\n"
"  int hy = g[1] + c * (uu >> 10) + d * (ee >> 10), ly = c * (uu & 1023) + d * (ee & 1023);\n"
"  int ax = (abs(a) * U.step_u) >> 10, bx = (abs(b) * U.step_v) >> 10, ay = (abs(c) * U.step_u) >> 10, by = (abs(d) * U.step_v) >> 10;\n"
"  int w = max(ax + bx, ay + by);\n"
"  int n, base, ix = 0, iy = 0, ex = 0, ey = 0;\n"
"  float fx = 0, fy = 0;\n"
"  if (!l0 && !l1) { n = 1; base = -2; }\n"
"  else if (w <= 256) {\n"
"    int qx = hx + (lx >> 10) - 128, qy = hy + (ly >> 10) - 128;\n"
"    ix = qx >> 8; iy = qy >> 8; ex = m7_edge(qx & 255, ax, bx); ey = m7_edge(qy & 255, ay, by);\n"
"    if (ex == 256) { ix++; ex = 0; }\n"
"    if (ey == 256) { iy++; ey = 0; }\n"
"    fx = float(ex) / 256.0; fy = float(ey) / 256.0;\n"
"    n = (ex ? 2 : 1) * (ey ? 2 : 1); base = -1;\n"
"  } else {\n"
"    n = w <= 512 ? 4 : w <= 1024 ? 8 : 16; base = n == 4 ? 0 : n == 8 ? 4 : 12;\n"
"  }\n"
"  uint last = 0x100u, c0 = 0, c1 = 0, cur0 = 0, cur1 = 0, top0 = 0, top1 = 0;\n"
"  uint3 s0 = 0, s1 = 0;\n"
"  for (int k = 0; k < n; k++) {\n"
"    int px, py, ci = 0, cj = 0;\n"
"    if (base < 0) { ci = ex ? (k & 1) : 0; cj = ex ? (k >> 1) : k; px = ix + ci; py = iy + cj; }\n"
"    else {\n"
"      int eu = sx * U.du[base + k], ev = sy * U.dv[base + k];\n"
"      px = (hx + ((lx + a * eu + b * ev) >> 10)) >> 8; py = (hy + ((ly + c * eu + d * ev) >> 10)) >> 8;\n"
"    }\n"
"    uint pix = base == -2 ? 0u : m7_ptexel(plane, uint(g[5]) >> 24, vr, sel, px, py);\n"
"    if (pix != last) {\n"
"      last = pix;\n"
"      if (m0) c0 = m7_compose(p0, g, pl, l0 ? pix : 0u);\n"
"      if (m1) c1 = m7_compose(p1, g, pl, l1 ? pix : 0u);\n"
"    }\n"
"    if (base >= 0) { s0 += uint3((c0 >> 16) & 0xFF, (c0 >> 8) & 0xFF, c0 & 0xFF); s1 += uint3((c1 >> 16) & 0xFF, (c1 >> 8) & 0xFF, c1 & 0xFF); continue; }\n"
"    cur0 = ci ? m7_mix(cur0, c0, fx) : c0; cur1 = ci ? m7_mix(cur1, c1, fx) : c1;\n"
"    if (!ex || ci) { top0 = cj ? m7_mix(top0, cur0, fy) : cur0; top1 = cj ? m7_mix(top1, cur1, fy) : cur1; }\n"
"  }\n"
"  if (base >= 0) {\n"
"    uint sh = n == 4 ? 2 : n == 8 ? 3 : 4; uint3 h = uint3(uint(n) / 2);\n"
"    s0 = (s0 + h) >> sh; s1 = (s1 + h) >> sh;\n"
"    top0 = 0xFF000000u | s0.x << 16 | s0.y << 8 | s0.z; top1 = 0xFF000000u | s1.x << 16 | s1.y << 8 | s1.z;\n"
"  }\n"
"  uint r0 = m0 ? top0 : k0;\n"
"  if (!two) return uint2(r0, r0);\n"
"  if (same) return uint2(r0, r0);\n"
"  if (m1) return uint2(r0, top1);\n"
"  if (!(fl1 & 3u)) return uint2(r0, m7_q(U, P, S, pal, vram, pic, plane, cv, i + 1, r, u, v));\n"
"  return uint2(r0, k1); }\n"
"static uint m7_pic(constant M7U &U, const device uint *pic, int x, int y) { return pic[clamp(y, 0, U.ph - 1) * U.pw + clamp(x, 0, U.pw - 1)]; }\n"
/* an output pixel: the picture pixels under it blended with sharp
   bilinear (rr_m7_pixel), a row of them at a time */
"fragment float4 m7fs(BO in [[stage_in]], constant M7U &U [[buffer(0)]], const device uint4 *P [[buffer(1)]],\n"
"                     const device int *S [[buffer(2)]], const device ushort *pal [[buffer(3)]], const device ushort *vram [[buffer(4)]],\n"
"                     const device uint *pic [[buffer(5)]], const device int *cols [[buffer(6)]], const device int *rows [[buffer(7)]],\n"
"                     const device uchar *cv [[buffer(8)]], texture2d_array<uint> plane [[texture(0)]]) {\n"
"  int X = int(in.pos.x) - U.d0, Y = int(in.pos.y) - U.d1;\n"
"  const device int *col = cols + X * 3, *row = rows + Y * 4;\n"
"  int tx = col[0], ty = row[0], u = col[2], v = row[3];\n"
"  float fx = as_type<float>(col[1]), fy = as_type<float>(row[1]);\n"
"  bool m7 = row[2] != 0, two = fx > 0;\n"
"  int nr = fy > 0 ? 2 : 1;\n"
"  uint top = 0;\n"
"  for (int j = 0; j < nr; j++) {\n"
"    uint cur;\n"
"    if (m7) { uint2 q = m7_row(U, P, S, pal, vram, pic, plane, cv, tx, two, ty + j, u, v); cur = two ? m7_mix(q.x, q.y, fx) : q.x; }\n"
"    else cur = two ? m7_mix(m7_pic(U, pic, tx, ty + j), m7_pic(U, pic, tx + 1, ty + j), fx) : m7_pic(U, pic, tx, ty + j);\n"
"    top = j ? m7_mix(top, cur, fy) : cur;\n"
"  }\n"
"  return float4(float((top >> 16) & 0xFF), float((top >> 8) & 0xFF), float(top & 0xFF), 255.0) / 255.0; }\n";

static id<MTLDevice> s_dev;
static id<MTLCommandQueue> s_q;
static id<MTLRenderPipelineState> s_copy, s_sharp, s_m7;
static id<MTLComputePipelineState> s_cp, s_cpr; /* the compositor: a picture; a picture and its records */
static id<MTLBuffer> s_recbuf[3], s_picbuf[3];  /* each frame in flight's records and picture */
static id<MTLBuffer> s_cpbuf[3];              /* each frame in flight's description */
static id<MTLTexture> s_plane;             /* the Mode 7 planes (rr_mode7.h), as last uploaded */
static uint32_t s_plane_gen;
static int s_planes;
static id<MTLTexture> s_col;            /* the target */
static id<MTLTexture> s_pic[3];          /* each frame's picture (frames in flight) */
static int s_pi;
static id<MTLBuffer> s_vb[3];
static size_t s_vused;
static id<MTLCommandBuffer> s_cmd;
static id<MTLRenderCommandEncoder> s_enc;
static dispatch_semaphore_t s_inflight;
static int s_rw, s_rh;
static uint32_t s_clear;
static id<MTLRenderPipelineState> s_over;      /* overlays: blended */
static id<MTLTexture> s_otex[3][RR_OVERLAYS];  /* each frame in flight's overlays' images */
static int s_on;                                /* overlays this frame */
#define VB_SIZE (1u << 16)

static id<MTLRenderPipelineState> make_pipe_blend(NSString *fs, id<MTLLibrary> lib, int blend)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    NSError *err = nil;
    id<MTLRenderPipelineState> p;
    d.vertexFunction = [lib newFunctionWithName:@"bvs"];
    d.fragmentFunction = [lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (blend) {
        d.colorAttachments[0].blendingEnabled = YES;
        d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
    }
    p = [s_dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p)
        fprintf(stderr, "rr: metal pipeline %s: %s\n", [fs UTF8String], [[err description] UTF8String]);
    return p;
}

static id<MTLRenderPipelineState> make_pipe(NSString *fs, id<MTLLibrary> lib) { return make_pipe_blend(fs, lib, 0); }

static void targets(int w, int h)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                   width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModeShared;
    s_col = [s_dev newTextureWithDescriptor:td];
    s_rw = w;
    s_rh = h;
}

int rr_open(void)
{
    @autoreleasepool {
        NSError *err = nil;
        id<MTLLibrary> lib;
        CAMetalLayer *ml;
        int i, w, h;
        s_dev = MTLCreateSystemDefaultDevice();
        if (!s_dev)
            return 0;
        s_q = [s_dev newCommandQueue];
        lib = [s_dev newLibraryWithSource:k_src options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "rr: metal shader: %s\n", [[err description] UTF8String]);
            return 0;
        }
        s_copy = make_pipe(@"bfs", lib);
        s_sharp = make_pipe(@"sfs", lib);
        s_over = make_pipe_blend(@"ofs", lib, 1);
        s_m7 = make_pipe(@"m7fs", lib);
        if (!s_copy || !s_sharp || !s_over || !s_m7)
            return 0;
        {
            NSString *src = [NSString stringWithFormat:@"%s%s%s", k_compose_pre, k_compose_body, k_compose_post];
            id<MTLLibrary> cl = [s_dev newLibraryWithSource:src options:nil error:&err];
            id<MTLFunction> fn = cl ? [cl newFunctionWithName:@"cpk"] : nil;
            if (!fn) {
                fprintf(stderr, "rr: metal compositor: %s\n", [[err description] UTF8String]);
                return 0;
            }
            s_cp = [s_dev newComputePipelineStateWithFunction:fn error:&err];
            fn = [cl newFunctionWithName:@"cprk"];
            s_cpr = fn ? [s_dev newComputePipelineStateWithFunction:fn error:&err] : nil;
            if (!s_cp || !s_cpr)
                return 0;
        }
        for (i = 0; i < 3; i++)
            s_vb[i] = [s_dev newBufferWithLength:VB_SIZE options:MTLResourceStorageModeShared];
        s_inflight = dispatch_semaphore_create(3);
        ml = host_metal_layer();
        if (ml) {
            [ml setDevice:s_dev];
            [ml setMaximumDrawableCount:3];
            host_metal_layer_fit(&w, &h);
        }
        if (!ml || w < 1 || h < 1)
            rr_headless_size(&w, &h);
        targets(w, h);
    }
    return 1;
}

void rr_close(void) {}

const char *rr_name(void) { return "metal"; }

void rr_target(int *w, int *h)
{
    CAMetalLayer *ml = host_metal_layer();
    if (ml && !s_cmd) {                    /* the next frame at the window's size */
        int fw, fh;
        host_metal_layer_fit(&fw, &fh);
        if (fw > 0 && fh > 0 && (fw != s_rw || fh != s_rh))
            targets(fw, fh);
    }
    *w = s_rw;
    *h = s_rh;
}

/* a quad over target rectangle d, uv 0..1, into this frame's vertex buffer:
   its offset, or -1 */
static long quad(const int d[4])
{
    static const int ix[6] = { 0, 1, 2, 2, 1, 3 };
    float c[4][4] = { { (float)d[0], (float)d[1], 0, 0 }, { (float)d[2], (float)d[1], 1, 0 },
                      { (float)d[0], (float)d[3], 0, 1 }, { (float)d[2], (float)d[3], 1, 1 } };
    float *q;
    size_t o = (s_vused + 15) & ~(size_t)15;
    int i;
    if (o + 6 * 16 > VB_SIZE)
        return -1;
    s_vused = o + 6 * 16;
    q = (float *)((char *)[s_vb[s_pi] contents] + o);
    for (i = 0; i < 6; i++) {
        q[4 * i + 0] = c[ix[i]][0] / s_rw * 2 - 1;
        q[4 * i + 1] = 1 - c[ix[i]][1] / s_rh * 2;
        q[4 * i + 2] = c[ix[i]][2];
        q[4 * i + 3] = c[ix[i]][3];
    }
    return (long)o;
}

void rr_begin(uint32_t argb)
{
    @autoreleasepool {
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        int w, h;
        rr_target(&w, &h);
        dispatch_semaphore_wait(s_inflight, DISPATCH_TIME_FOREVER);
        s_cmd = [s_q commandBuffer];
        {
            dispatch_semaphore_t sem = s_inflight;
            [s_cmd addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(sem); }];
        }
        s_pi = (s_pi + 1) % 3;
        s_vused = 0;
        s_on = 0;
        s_clear = argb;
        rp.colorAttachments[0].texture = s_col;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(((argb >> 16) & 0xFF) / 255.0, ((argb >> 8) & 0xFF) / 255.0,
                                                              (argb & 0xFF) / 255.0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        s_enc = [s_cmd renderCommandEncoderWithDescriptor:rp];
    }
}

/* this frame's picture, w x h */
static id<MTLTexture> pic_tex(int w, int h)
{
    id<MTLTexture> t = s_pic[s_pi];
    if (!t || (int)t.width != w || (int)t.height != h) {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                       width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        t = s_pic[s_pi] = [s_dev newTextureWithDescriptor:td];
    }
    return t;
}

static void draw_pic(id<MTLTexture> t, int w, int h, const int d[4]);

void rr_image(const uint32_t *argb, int w, int h, const int d[4])
{
    @autoreleasepool {
        id<MTLTexture> t;
        if (!s_enc || w <= 0 || h <= 0 || d[2] <= d[0] || d[3] <= d[1])
            return;
        t = pic_tex(w, h);
        /* 0xAARRGGBB little-endian is BGRA in memory */
        [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0
               withBytes:argb bytesPerRow:(NSUInteger)w * 4];
        draw_pic(t, w, h, d);
    }
}

static int compose(const uint32_t *desc, size_t words, int w, int h, const uint32_t wp[4], const int d[4]);

int rr_compose(const uint32_t *desc, size_t words, int w, int h, const int d[4])
{
    static const uint32_t none[4] = { 0, 0, 0, 0 };
    return compose(desc, words, w, h, none, d);
}

int rr_compose_woven(const uint32_t *desc, size_t words, int ww, int wh, uint32_t base0, uint32_t base1,
                     const int d[4])
{
    uint32_t wp[4] = { (uint32_t)ww, base0, base1, 0 };
    return compose(desc, words, ww, wh, wp, d);
}

static int compose(const uint32_t *desc, size_t words, int w, int h, const uint32_t wp[4], const int d[4])
{
    @autoreleasepool {
        id<MTLTexture> t;
        id<MTLComputeCommandEncoder> ce;
        MTLRenderPassDescriptor *rp;
        if (!s_cp || !s_enc || w <= 0 || h <= 0 || d[2] <= d[0] || d[3] <= d[1])
            return 0;
        t = pic_tex(w, h);
        if (!s_cpbuf[s_pi] || [s_cpbuf[s_pi] length] < words * 4)
            s_cpbuf[s_pi] = [s_dev newBufferWithLength:words * 4 * 2 options:MTLResourceStorageModeShared];
        memcpy([s_cpbuf[s_pi] contents], desc, words * 4);
        /* the picture first, in a pass of its own, then the target's pass
           goes on */
        [s_enc endEncoding];
        ce = [s_cmd computeCommandEncoder];
        [ce setComputePipelineState:s_cp];
        [ce setBuffer:s_cpbuf[s_pi] offset:0 atIndex:0];
        [ce setBytes:wp length:16 atIndex:1];
        [ce setTexture:t atIndex:0];
        [ce dispatchThreads:MTLSizeMake((NSUInteger)w, (NSUInteger)h, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
        [ce endEncoding];
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = s_col;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        s_enc = [s_cmd renderCommandEncoderWithDescriptor:rp];
        draw_pic(t, w, h, d);
        return 1;
    }
}

/* picture t (w x h) over target rectangle d: one to one, or sharp bilinear */
static void draw_pic(id<MTLTexture> t, int w, int h, const int d[4])
{
    {
        long off;
        if ((off = quad(d)) < 0)
            return;
        [s_enc setRenderPipelineState:d[2] - d[0] == w && d[3] - d[1] == h ? s_copy : s_sharp];
        [s_enc setVertexBuffer:s_vb[s_pi] offset:(NSUInteger)off atIndex:0];
        {
            /* where the target's pixels sample: its rectangle's corner, and
               texels a target pixel (rr_ref_image's dx, dy) */
            float su[4] = { (float)d[0], (float)d[1], (float)w / (float)(d[2] - d[0]), (float)h / (float)(d[3] - d[1]) };
            [s_enc setFragmentBytes:su length:sizeof su atIndex:0];
        }
        [s_enc setFragmentTexture:t atIndex:0];
        [s_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
}

static void m7_draw(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s, id<MTLBuffer> px,
                    id<MTLBuffer> pic);

void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s) { m7_draw(m, d, s, nil, nil); }

int rr_compose_mode7(const uint32_t *desc, size_t words, const struct RrMode7 *m, const int d[4],
                     const struct RrM7Samples *s)
{
    @autoreleasepool {
        size_t np = (size_t)m->pw * (size_t)m->ph;
        id<MTLComputeCommandEncoder> ce;
        MTLRenderPassDescriptor *rp;
        if (!s_cpr || !s_enc || np == 0 || d[2] <= d[0] || d[3] <= d[1])
            return 0;
        if (!s_cpbuf[s_pi] || [s_cpbuf[s_pi] length] < words * 4)
            s_cpbuf[s_pi] = [s_dev newBufferWithLength:words * 4 * 2 options:MTLResourceStorageModeShared];
        if (!s_recbuf[s_pi] || [s_recbuf[s_pi] length] < np * 16) {
            s_recbuf[s_pi] = [s_dev newBufferWithLength:np * 16 options:MTLResourceStorageModePrivate];
            s_picbuf[s_pi] = [s_dev newBufferWithLength:np * 4 options:MTLResourceStorageModePrivate];
        }
        if (!s_cpbuf[s_pi] || !s_recbuf[s_pi] || !s_picbuf[s_pi])
            return 0;
        memcpy([s_cpbuf[s_pi] contents], desc, words * 4);
        /* the picture and records first, in a pass of their own */
        [s_enc endEncoding];
        ce = [s_cmd computeCommandEncoder];
        [ce setComputePipelineState:s_cpr];
        [ce setBuffer:s_cpbuf[s_pi] offset:0 atIndex:0];
        [ce setBuffer:s_recbuf[s_pi] offset:0 atIndex:1];
        [ce setBuffer:s_picbuf[s_pi] offset:0 atIndex:2];
        [ce dispatchThreads:MTLSizeMake((NSUInteger)m->pw, (NSUInteger)m->ph, 1) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
        [ce endEncoding];
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = s_col;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        s_enc = [s_cmd renderCommandEncoderWithDescriptor:rp];
        m7_draw(m, d, s, s_recbuf[s_pi], s_picbuf[s_pi]);
        return 1;
    }
}

/* m presented over d; its records and picture from px and pic where given
   (the compositor's), else m's own */
static void m7_draw(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s, id<MTLBuffer> px,
                    id<MTLBuffer> pic)
{
    @autoreleasepool {
        struct { int pw, ph, d0, d1, step_u, step_v, du[28], dv[28], cpp, left, cvk, cvlen, cvw, cvh; } u;
        int ow = d[2] - d[0], oh = d[3] - d[1];
        size_t np = (size_t)m->pw * (size_t)m->ph;
        id<MTLBuffer> b[8];
        long off;
        int i;
        if (!s_enc || ow <= 0 || oh <= 0 || np == 0 || (off = quad(d)) < 0)
            return;
        u.pw = m->pw;
        u.ph = m->ph;
        u.d0 = d[0];
        u.d1 = d[1];
        u.step_u = s->step_u;
        u.step_v = s->step_v;
        u.cpp = m->cpp;
        u.left = m->left;
        u.cvk = m->cv_k;
        u.cvlen = (int)m->cv_len;
        u.cvw = m->cv_w;
        u.cvh = m->cv_h;
        /* the supersamples' offsets: 4 from 0, 8 from 4, 16 from 12 */
        memcpy(u.du, s->du4, sizeof s->du4);
        memcpy(u.du + 4, s->du8, sizeof s->du8);
        memcpy(u.du + 12, s->du16, sizeof s->du16);
        memcpy(u.dv, s->dv4, sizeof s->dv4);
        memcpy(u.dv + 4, s->dv8, sizeof s->dv8);
        memcpy(u.dv + 12, s->dv16, sizeof s->dv16);
        /* the frame's record, its states, palettes, VRAM and picture, and
           the tables: each draw its own copies, the GPU reading them while
           the next frame is made */
        b[0] = px ? px : [s_dev newBufferWithBytes:m->px length:np * sizeof *m->px options:MTLResourceStorageModeShared];
        b[1] = [s_dev newBufferWithBytes:m->seg length:(size_t)(m->segs ? m->segs : 1) * sizeof *m->seg
                                 options:MTLResourceStorageModeShared];
        b[2] = [s_dev newBufferWithBytes:m->pal length:(size_t)(m->pals ? m->pals : 1) * 512 options:MTLResourceStorageModeShared];
        b[3] = [s_dev newBufferWithBytes:m->vram length:(size_t)(m->vrams ? m->vrams : 1) * 0x10000
                                 options:MTLResourceStorageModeShared];
        b[4] = pic ? pic : [s_dev newBufferWithBytes:m->pic length:np * 4 options:MTLResourceStorageModeShared];
        b[5] = [s_dev newBufferWithBytes:s->cols length:(size_t)ow * 3 * 4 options:MTLResourceStorageModeShared];
        b[6] = [s_dev newBufferWithBytes:s->rows length:(size_t)oh * 4 * 4 options:MTLResourceStorageModeShared];
        {
            /* the canvas's texels (a byte where there is none) */
            static const uint8_t none = 0;
            b[7] = [s_dev newBufferWithBytes:m->cv && m->cv_len ? (const void *)m->cv : &none length:m->cv && m->cv_len ? m->cv_len : 1
                                     options:MTLResourceStorageModeShared];
        }
        for (i = 0; i < 8; i++)
            if (!b[i])
                return;
        if (!s_plane || s_planes < m->vrams || s_plane_gen != m->plane_gen) {
            /* the planes changed: uploaded again, after the frames in
               flight that read them (this frame's draws so far have none) */
            int layers = m->vrams ? m->vrams : 1, k;
            if (!s_plane || (int)s_plane.arrayLength < layers) {
                MTLTextureDescriptor *td = [MTLTextureDescriptor new];
                td.textureType = MTLTextureType2DArray;
                td.pixelFormat = MTLPixelFormatR8Uint;
                td.width = td.height = 1024;
                td.arrayLength = (NSUInteger)layers;
                td.usage = MTLTextureUsageShaderRead;
                td.storageMode = MTLStorageModeShared;
                s_plane = [s_dev newTextureWithDescriptor:td];
            } else {
                id<MTLCommandBuffer> c = [s_q commandBuffer];
                [c commit];
                [c waitUntilCompleted];
            }
            for (k = 0; k < m->vrams; k++)
                [s_plane replaceRegion:MTLRegionMake2D(0, 0, 1024, 1024) mipmapLevel:0 slice:(NSUInteger)k
                             withBytes:m->plane + (size_t)k * 0x100000u bytesPerRow:1024 bytesPerImage:0x100000];
            s_plane_gen = m->plane_gen;
            s_planes = m->vrams;
        }
        [s_enc setRenderPipelineState:s_m7];
        [s_enc setFragmentTexture:s_plane atIndex:0];
        [s_enc setVertexBuffer:s_vb[s_pi] offset:(NSUInteger)off atIndex:0];
        [s_enc setFragmentBytes:&u length:sizeof u atIndex:0];
        for (i = 0; i < 8; i++)
            [s_enc setFragmentBuffer:b[i] offset:0 atIndex:(NSUInteger)i + 1];
        [s_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
}

void rr_overlay(const uint32_t *argb, int w, int h, const float q[6])
{
    @autoreleasepool {
        static const int ix[6] = { 0, 1, 2, 2, 1, 3 };
        float c[4][4] = { { q[0], q[1], 0, 0 }, { q[2], q[3], 1, 0 }, { q[4], q[5], 0, 1 },
                          { q[2] + q[4] - q[0], q[3] + q[5] - q[1], 1, 1 } };
        id<MTLTexture> t;
        size_t o = (s_vused + 15) & ~(size_t)15;
        float *v;
        int i;
        if (!s_enc || w <= 0 || h <= 0 || s_on == RR_OVERLAYS || o + 6 * 16 > VB_SIZE)
            return;
        t = s_otex[s_pi][s_on];
        if (!t || (int)t.width != w || (int)t.height != h) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                           width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead;
            t = s_otex[s_pi][s_on] = [s_dev newTextureWithDescriptor:td];
        }
        s_on++;
        [t replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h) mipmapLevel:0
               withBytes:argb bytesPerRow:(NSUInteger)w * 4];
        s_vused = o + 6 * 16;
        v = (float *)((char *)[s_vb[s_pi] contents] + o);
        for (i = 0; i < 6; i++) {
            v[4 * i + 0] = c[ix[i]][0] / s_rw * 2 - 1;
            v[4 * i + 1] = 1 - c[ix[i]][1] / s_rh * 2;
            v[4 * i + 2] = c[ix[i]][2];
            v[4 * i + 3] = c[ix[i]][3];
        }
        [s_enc setRenderPipelineState:s_over];
        [s_enc setVertexBuffer:s_vb[s_pi] offset:o atIndex:0];
        [s_enc setFragmentTexture:t atIndex:0];
        [s_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
}

void rr_present(void)
{
    @autoreleasepool {
        CAMetalLayer *ml = host_window_visible() ? host_metal_layer() : nil;
        if (!s_enc)
            return;
        [s_enc endEncoding];
        s_enc = nil;
        if (ml) {
            id<CAMetalDrawable> dr = [ml nextDrawable];
            if (dr && (int)[dr texture].width == s_rw && (int)[dr texture].height == s_rh) {
                id<MTLBlitCommandEncoder> b = [s_cmd blitCommandEncoder];
                [b copyFromTexture:s_col sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                        sourceSize:MTLSizeMake((NSUInteger)s_rw, (NSUInteger)s_rh, 1)
                         toTexture:[dr texture] destinationSlice:0 destinationLevel:0
                 destinationOrigin:MTLOriginMake(0, 0, 0)];
                [b endEncoding];
                [s_cmd presentDrawable:dr];
            }
        }
        [s_cmd commit];
        s_cmd = nil;
    }
}

int rr_read(uint32_t *argb)
{
    @autoreleasepool {
        id<MTLCommandBuffer> c = [s_q commandBuffer];
        [c commit];
        [c waitUntilCompleted];                /* everything before it is done */
        [s_col getBytes:argb bytesPerRow:(NSUInteger)s_rw * 4
             fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)s_rw, (NSUInteger)s_rh) mipmapLevel:0];
        return 1;
    }
}
