/*
 * compose_body.h: the console's picture, one pixel at a time, from a
 * frame's description (ppu.c ppu_compose): draw_line's rules (ppu.c)
 * written again so that every pixel stands alone, for a GPU to draw the
 * picture at any size. One source for C (rr_compose.h, the reference the
 * shaders are held to, itself held to ppu_draw_frame), GLSL (OpenGL and
 * Vulkan) and Metal: its prelude gives
 *
 *   CP_FN          a function's qualifiers
 *   CP_VAR         a variable private to the pixel being drawn
 *   U32, I32       32-bit unsigned and signed integers
 *   U_(x), I_(x)   conversions to them
 *   FD_(i)         word cp_fdb + i of the descriptions (cp_fdb: the one
 *                  drawn, 0 but in an interlaced frame's weave)
 *
 * and nothing here uses pointers, structures, arrays or the remainder of a
 * negative number.
 *
 * The description, in 32-bit words (ppu.c ppu_compose writes it):
 *
 *   header      CP_* below
 *   rows        for each picture row (the frame's own): its first part, how
 *               many parts, the hardware's row (draw_row: line - 1, below 0
 *               and past the hardware's lines on a taller screen) and the
 *               row the vertical mosaic repeats
 *   parts       for each part of a row (a line's segments, ppu.c seg_x): the
 *               hardware's column it starts at, its state, its sprites
 *               (first entry, how many)
 *   states      CP_ST words each: the screen state packed (CP_S_*), its
 *               palette's word and its VRAM's first 16-bit word
 *   palettes    256 colours, two to a word
 *   VRAM        the frame's copies, two 16-bit words to a word
 *   sprites     for each row and part, the sprites on its row in priority
 *               order (the first on top), the console's line limits
 *               decided (ppu.c obj_eval): CP_SE words each
 *   canvas      the canvas's texels, four to a word
 *   outside     BG1-4's maps outside the console's picture
 *               (nshvc_ext_layer_outside), when one is on
 *   segments    with a frame presenting Mode 7 or a canvas: each part's
 *               entry in the presentation's segment table (rr_mode7.h, made
 *               by ppu.c ppu_mode7_segs), for its records (cp_record)
 */

#define CP_PW 0          /* the picture's width: the screen's, twice on a frame with a hires line */
#define CP_PH 1          /* its height (the screen's) */
#define CP_SW 2          /* the screen's width */
#define CP_SL 3          /* the hardware's column 0 on it */
#define CP_LINES 4       /* the hardware's lines */
#define CP_FIELD 5
#define CP_LIMITS 6      /* the sprite line limits on (nshvc_ext_sprite_limits) */
#define CP_ROWS 7        /* the sections' first words */
#define CP_PARTS 8
#define CP_STATES 9
#define CP_PALS 10
#define CP_VRAM 11
#define CP_SPR 12
#define CP_CV 13
#define CP_CVON 14       /* the canvas: on, layer, word address, characters, k, texels */
#define CP_CVLAYER 15
#define CP_CVADDR 16
#define CP_CVCOUNT 17
#define CP_CVK 18
#define CP_CVLEN 19
#define CP_M7SEG 20      /* the parts' segments (0: none, the frame presents neither) */
#define CP_OUT 21        /* the maps outside the picture: on (bits 0-3) | sizes << 4, then
                            4096 entries a layer, two a word (0: none) */
#define CP_HEADER 22

#define CP_ST 20         /* a state's words */
#define CP_S_INIDISP 0   /* inidisp, obsel, bgmode, mosaic */
#define CP_S_BGSC 1
#define CP_S_NBA 2       /* bgnba[0], bgnba[1], m7sel, setini */
#define CP_S_WSEL 3      /* w12sel, w34sel, wobjsel, wbglog */
#define CP_S_WH 4
#define CP_S_TM 5        /* wobjlog, tm, ts, tmw */
#define CP_S_TSW 6       /* tsw, cgwsel, cgadsub, clip */
#define CP_S_HOFS 7      /* bghofs, two a word */
#define CP_S_VOFS 9
#define CP_S_FIXED 11    /* the fixed colour: r, g, b */
#define CP_S_M7AB 12
#define CP_S_M7CD 13
#define CP_S_M7XY 14
#define CP_S_M7OFS 15    /* m7hofs, m7vofs */
#define CP_S_PAGE 16     /* page[0-3] */
#define CP_S_PAGE4 17    /* page[4], oam_prio, oamaddr's sprite (first) */
#define CP_S_PAL 18
#define CP_S_VBASE 19

#define CP_SE 3          /* a sprite entry: X; row, columns, flips, palette, priority, kept; name */

/* the description drawn from: its first word (cp_woven sets it) */
CP_VAR U32 cp_fdb;

/* the pixel's state's words (cp_load), and the main and sub screens'
   pixels at it (cp_screens) */
CP_VAR U32 cp_st[CP_ST];
CP_VAR U32 cp_main, cp_sub;
/* the layers the screens leave out (cp_screens, a canvas's record), and
   where cp_bg found the canvas: its character | the pixel's column << 14
   | row << 17 | flips << 20 | palette << 22 | priority << 25, or ~0u */
CP_VAR U32 cp_tmask, cp_cvat;

CP_FN void cp_load(U32 si)
{
    U32 k, at = FD_(CP_STATES) + si * U_(CP_ST);
    for (k = 0u; k < U_(CP_ST); k++)
        cp_st[k] = FD_(at + k);
}

/* the state's byte k (0-3) of word w (si: the state cp_load took) */
CP_FN U32 cp_b(U32 si, U32 w, U32 k) { return (cp_st[w] >> (8u * k)) & 0xFFu; }
CP_FN U32 cp_h(U32 si, U32 w, U32 k) { return (cp_st[w] >> (16u * k)) & 0xFFFFu; }
CP_FN U32 cp_sw(U32 si, U32 w) { return cp_st[w]; }

/* palette colour i of state si */
CP_FN U32 cp_pal(U32 si, U32 i)
{
    return (FD_(cp_sw(si, U_(CP_S_PAL)) + (i >> 1)) >> (16u * (i & 1u))) & 0xFFFFu;
}

/* VRAM's word a (0-0x7FFF) in the page `page` of state si's copy */
CP_FN U32 cp_vram(U32 si, U32 page, U32 a)
{
    U32 e = cp_sw(si, U_(CP_S_VBASE)) + (page << 15) + (a & 0x7FFFu);
    return (FD_(FD_(CP_VRAM) + (e >> 1)) >> (16u * (e & 1u))) & 0xFFFFu;
}

CP_FN I32 cp_floor_to(I32 x, I32 m) { return x >= 0 ? x - x % m : x - (m - 1 - (-x - 1) % m); }

CP_FN U32 cp_bpp(U32 mode, U32 n)
{
    /* { 2, 2, 2, 2 }, { 4, 4, 2, 0 }, { 4, 4, 0, 0 }, { 8, 4, 0, 0 },
       { 8, 2, 0, 0 }, { 4, 2, 0, 0 }, { 4, 0, 0, 0 }, { 8, 0, 0, 0 } */
    if (mode == 0u) return 2u;
    if (mode == 1u) return n < 2u ? 4u : n == 2u ? 2u : 0u;
    if (mode == 2u) return n < 2u ? 4u : 0u;
    if (mode == 3u) return n == 0u ? 8u : n == 1u ? 4u : 0u;
    if (mode == 4u) return n == 0u ? 8u : n == 1u ? 2u : 0u;
    if (mode == 5u) return n == 0u ? 4u : n == 1u ? 2u : 0u;
    if (mode == 6u) return n == 0u ? 4u : 0u;
    return n == 0u ? 8u : 0u;
}

CP_FN U32 cp_map_entry(U32 si, U32 n, U32 tx, U32 ty)
{
    U32 sc = cp_b(si, U_(CP_S_BGSC), n), base = (sc & 0xFCu) << 8, off = (ty & 31u) << 5 | (tx & 31u);
    if ((tx & 32u) != 0u && (sc & 1u) != 0u)
        off += 0x400u;
    if ((ty & 32u) != 0u && (sc & 2u) != 0u)
        off += (sc & 1u) != 0u ? 0x800u : 0x400u;
    return cp_vram(si, cp_b(si, U_(CP_S_PAGE), n), (base + off) & 0x7FFFu);
}

CP_FN U32 cp_tile_pixel(U32 si, U32 page, U32 ca, U32 y, U32 x, U32 bpp)
{
    U32 bit = 7u - x, c = 0u, p;
    for (p = 0u; p < bpp; p += 2u) {
        U32 w = cp_vram(si, page, (ca + p * 4u + y) & 0x7FFFu);
        c |= ((w >> bit) & 1u) << p;
        c |= ((w >> (bit + 8u)) & 1u) << (p + 1u);
    }
    return c;
}

CP_FN U32 cp_direct(U32 c, U32 pal)
{
    U32 r = (c & 7u) << 2 | (pal & 1u) << 1, g = (c & 0x38u) >> 1 | (pal & 2u), b = (c & 0xC0u) >> 3 | (pal & 4u);
    return r | g << 5 | b << 10;
}

/* a layer's pixel: colour | ci << 15 | priority << 23 | opaque << 24 */
#define CP_LP(color, ci, prio) ((color) | (ci) << 15 | (prio) << 23 | 0x1000000u)

/* the canvas's texel for char address ca of layer n (ppu.c cv_char and
   cv_centre), or 256 when the canvas does not cover it */
CP_FN U32 cp_cv(U32 si, U32 n, U32 ca, U32 bpp, U32 px, U32 py, U32 flip)
{
    U32 wpc = bpp * 4u, a = ca & 0x7FFFu, k, sx, sy, i, rel;
    if (FD_(CP_CVON) == 0u || FD_(CP_CVLAYER) != n)
        return 256u;
    if (FD_(CP_CVADDR) >= 0x8000u)
        a |= cp_b(si, U_(CP_S_PAGE), n) << 15;
    if (a < FD_(CP_CVADDR))
        return 256u;
    rel = a - FD_(CP_CVADDR);
    if (rel % wpc != 0u || rel / wpc >= FD_(CP_CVCOUNT))
        return 256u;
    k = FD_(CP_CVK);
    sx = k >> 1;
    sy = k >> 1;
    if ((flip & 1u) != 0u)
        sx = k - 1u - sx;
    if ((flip & 2u) != 0u)
        sy = k - 1u - sy;
    cp_cvat = rel / wpc;
    i = (rel / wpc) * 64u * k * k + (py * k + sy) * 8u * k + px * k + sx;
    if (i >= FD_(CP_CVLEN))
        return 0u;
    return (FD_(FD_(CP_CV) + (i >> 2)) >> (8u * (i & 3u))) & 0xFFu;
}

/* background n's pixel x of the line (ppu.c draw_bg: 0 to the screen's
   width, twice on a hires line), on row dr with the mosaic's row my */
CP_FN U32 cp_bg(U32 si, U32 n, U32 x, I32 dr, U32 my)
{
    U32 bgmode = cp_b(si, U_(CP_S_INIDISP), 2u), mode = bgmode & 7u, mosaic = cp_b(si, U_(CP_S_INIDISP), 3u);
    U32 bpp = cp_bpp(mode, n), big = (bgmode >> (4u + n)) & 1u;
    U32 hires = mode == 5u || mode == 6u ? 1u : 0u;
    U32 xshift = hires != 0u || big != 0u ? 4u : 3u, yshift = big != 0u ? 4u : 3u;
    U32 msize = (mosaic & (1u << n)) != 0u ? (mosaic >> 4) + 1u : 1u;
    I32 y = msize > 1u ? I_(my) : dr + 1;
    U32 nbaw = cp_b(si, U_(CP_S_NBA), n < 2u ? 0u : 1u), nba = ((nbaw >> ((n & 1u) * 4u)) & 0x0Fu) << 12;
    U32 opt = mode == 2u || mode == 4u || mode == 6u ? 1u : 0u;
    I32 sl = I_(FD_(CP_SL)), hx, sx;
    /* (each word named: a register, not an array indexed at run time) */
    U32 hofs = ((n >> 1) == 0u ? cp_st[CP_S_HOFS] : cp_st[CP_S_HOFS + 1]) >> (16u * (n & 1u)) & 0xFFFFu;
    U32 vofs = ((n >> 1) == 0u ? cp_st[CP_S_VOFS] : cp_st[CP_S_VOFS + 1]) >> (16u * (n & 1u)) & 0xFFFFu;
    U32 xx, yy, tx, ty, px, py, c, e, ch, ca, pal, opt_h = 0u, flip, cv;
    if (hires != 0u && (cp_b(si, U_(CP_S_NBA), 3u) & 1u) != 0u)
        y = 2 * y + ((FD_(CP_FIELD) != 0u && (mosaic & (1u << n)) == 0u) ? 1 : 0);
    hx = I_(x) - (hires != 0u ? 2 * sl : sl);
    sx = msize > 1u ? cp_floor_to(hx, I_(hires != 0u ? 2u * msize : msize)) : hx;
    if (opt != 0u && n < 2u) {
        I32 col = hires != 0u ? (sx + I_((hofs << 1) & 7u)) >> 4 : (sx + I_(hofs & 7u)) >> 3;
        if (col > 0) {
            U32 big3 = (bgmode >> 6) & 1u;
            U32 sx3 = big3 != 0u && hires == 0u ? 4u : 3u, sy3 = big3 != 0u ? 4u : 3u;
            U32 bx = (U_(col - 1) * 8u + (cp_h(si, U_(CP_S_HOFS) + 1u, 0u) & ~7u)) >> sx3;
            U32 v3 = cp_h(si, U_(CP_S_VOFS) + 1u, 0u);
            U32 hv = cp_map_entry(si, 2u, bx, v3 >> sy3), vv;
            if (mode == 4u) {
                if ((hv & 0x8000u) != 0u) {
                    if ((hv & (0x2000u << n)) != 0u)
                        vofs = hv & 0x3FFu;
                } else if ((hv & (0x2000u << n)) != 0u) {
                    hofs = (hofs & 7u) | (hv & 0x3F8u);
                    opt_h = 1u;
                }
            } else {
                vv = cp_map_entry(si, 2u, bx, (v3 + 8u) >> sy3);
                if ((hv & (0x2000u << n)) != 0u) {
                    hofs = (hofs & 7u) | (hv & 0x3F8u);
                    opt_h = 1u;
                }
                if ((vv & (0x2000u << n)) != 0u)
                    vofs = vv & 0x3FFu;
            }
        }
    }
    if (hires != 0u)
        xx = (U_(sx) + (opt_h != 0u ? ((hofs << 1) & 7u) + (hofs & ~7u) : hofs << 1)) & 0x7FFu;
    else
        xx = (U_(sx) + hofs) & 0x3FFu;
    yy = (U_(y) + vofs) & 0x3FFu;
    tx = xx >> xshift;
    ty = yy >> yshift;
    {
        /* outside the console's picture, the layer's own map there when it
           has one */
        U32 ob = FD_(CP_OUT), col = hires != 0u ? x >> 1 : x;
        if (ob != 0u && ((FD_(ob) >> n) & 1u) != 0u &&
            (I_(col) < sl || I_(col) >= sl + 256 || dr < 0 || dr >= I_(FD_(CP_LINES)))) {
            U32 sc = (FD_(ob) >> (4u + 2u * n)) & 3u, off = (ty & 31u) << 5 | (tx & 31u);
            if ((tx & 32u) != 0u && (sc & 1u) != 0u)
                off += 0x400u;
            if ((ty & 32u) != 0u && (sc & 2u) != 0u)
                off += (sc & 1u) != 0u ? 0x800u : 0x400u;
            off &= 0xFFFu;
            e = (FD_(ob + 1u + n * 2048u + (off >> 1)) >> (16u * (off & 1u))) & 0xFFFFu;
        } else {
            e = cp_map_entry(si, n, tx, ty);
        }
    }
    px = xx & ((1u << xshift) - 1u);
    py = yy & ((1u << yshift) - 1u);
    if ((e & 0x4000u) != 0u)
        px = (1u << xshift) - 1u - px;
    if ((e & 0x8000u) != 0u)
        py = (1u << yshift) - 1u - py;
    ch = e & 0x3FFu;
    if (px >= 8u) ch += 1u;
    if (py >= 8u) ch += 16u;
    ca = (nba + (ch & 0x3FFu) * (bpp * 4u)) & 0xFFFFu;
    pal = (e >> 10) & 7u;
    flip = ((e & 0x4000u) != 0u ? 1u : 0u) | ((e & 0x8000u) != 0u ? 2u : 0u);
    cv = hires != 0u || msize > 1u ? 256u : cp_cv(si, n, ca, bpp, px & 7u, py & 7u, flip);
    if (cv != 256u) {
        c = cv & ((1u << bpp) - 1u);
        cp_cvat |= (px & 7u) << 14 | (py & 7u) << 17 | flip << 20 | pal << 22 | ((e >> 13) & 1u) << 25;
    }
    else
        c = cp_tile_pixel(si, cp_b(si, U_(CP_S_PAGE), n), ca, py & 7u, px & 7u, bpp);
    if (c == 0u)
        return ((e >> 13) & 1u) << 23;
    if (bpp == 8u)
        return CP_LP((cp_b(si, U_(CP_S_TSW), 1u) & 1u) != 0u ? cp_direct(c, pal) : cp_pal(si, c), c, (e >> 13) & 1u);
    if (bpp == 4u)
        return CP_LP(cp_pal(si, pal * 16u + c), pal * 16u + c, (e >> 13) & 1u);
    {
        U32 ci = (mode == 0u ? n * 32u : 0u) + pal * 4u + c;
        return CP_LP(cp_pal(si, ci), ci, (e >> 13) & 1u);
    }
}

CP_FN I32 cp_sext13(I32 v) { return (v & 0x1000) != 0 ? v | ~0x1FFF : v & 0x1FFF; }
CP_FN I32 cp_clip10(I32 v) { return (v & 0x2000) != 0 ? v | ~0x3FF : v & 0x3FF; }
CP_FN I32 cp_s16(U32 v) { return I_(v) - ((v & 0x8000u) != 0u ? 0x10000 : 0); }

/* Mode 7's pixel byte at texel (tpx, tpy) (rr_mode7.h rr_m7_texel): 0
   where the wrap mode leaves it clear */
CP_FN U32 cp_m7_texel(U32 si, I32 tpx, I32 tpy)
{
    U32 m7sel = cp_b(si, U_(CP_S_NBA), 2u), page = cp_b(si, U_(CP_S_PAGE), 0u), tile;
    if (((tpx | tpy) & ~0x3FF) != 0) {
        if ((m7sel & 0xC0u) == 0x80u)
            return 0u;
        if ((m7sel & 0xC0u) == 0xC0u)
            tile = 0u;
        else
            tile = cp_vram(si, page, U_(((tpy >> 3) & 127) << 7 | ((tpx >> 3) & 127))) & 0xFFu;
    } else {
        tile = cp_vram(si, page, U_((tpy >> 3) << 7 | (tpx >> 3))) & 0xFFu;
    }
    return cp_vram(si, page, (tile * 64u + U_(tpy & 7) * 8u + U_(tpx & 7)) & 0x7FFFu) >> 8;
}

/* the line's Mode 7 origin (ppu.c m7_origin) for line y: ox, or oy (which 1) */
CP_FN I32 cp_m7_origin(U32 si, I32 y, U32 which)
{
    I32 a = cp_s16(cp_h(si, U_(CP_S_M7AB), 0u)), b = cp_s16(cp_h(si, U_(CP_S_M7AB), 1u));
    I32 c = cp_s16(cp_h(si, U_(CP_S_M7CD), 0u)), d = cp_s16(cp_h(si, U_(CP_S_M7CD), 1u));
    I32 cx = cp_sext13(I_(cp_h(si, U_(CP_S_M7XY), 0u))), cy = cp_sext13(I_(cp_h(si, U_(CP_S_M7XY), 1u)));
    I32 hofs = cp_sext13(I_(cp_h(si, U_(CP_S_M7OFS), 0u))), vofs = cp_sext13(I_(cp_h(si, U_(CP_S_M7OFS), 1u)));
    I32 x0 = cp_clip10(hofs - cx), y0 = cp_clip10(vofs - cy);
    if (which == 0u)
        return ((a * x0) & ~63) + ((b * y) & ~63) + ((b * y0) & ~63) + cx * 256;
    return ((c * x0) & ~63) + ((d * y) & ~63) + ((d * y0) & ~63) + cy * 256;
}

/* Mode 7 layer n's pixel at the screen's column x (ppu.c draw_m7, m7_lookup) */
CP_FN U32 cp_m7(U32 si, U32 n, U32 x, I32 dr, U32 my)
{
    U32 mosaic = cp_b(si, U_(CP_S_INIDISP), 3u), m7sel = cp_b(si, U_(CP_S_NBA), 2u);
    U32 msize = (mosaic & (1u << n)) != 0u ? (mosaic >> 4) + 1u : 1u, page = cp_b(si, U_(CP_S_PAGE), 0u);
    I32 y = msize > 1u ? I_(my) : dr + 1;
    I32 a = cp_s16(cp_h(si, U_(CP_S_M7AB), 0u)), b = cp_s16(cp_h(si, U_(CP_S_M7AB), 1u));
    I32 c = cp_s16(cp_h(si, U_(CP_S_M7CD), 0u)), d = cp_s16(cp_h(si, U_(CP_S_M7CD), 1u));
    I32 cx = cp_sext13(I_(cp_h(si, U_(CP_S_M7XY), 0u))), cy = cp_sext13(I_(cp_h(si, U_(CP_S_M7XY), 1u)));
    I32 hofs = cp_sext13(I_(cp_h(si, U_(CP_S_M7OFS), 0u))), vofs = cp_sext13(I_(cp_h(si, U_(CP_S_M7OFS), 1u)));
    I32 x0 = cp_clip10(hofs - cx), y0 = cp_clip10(vofs - cy), ox, oy, hx, sx, tpx, tpy;
    U32 tile, pix, prio = 0u;
    if ((m7sel & 2u) != 0u)
        y = 255 - y;
    ox = ((a * x0) & ~63) + ((b * y) & ~63) + ((b * y0) & ~63) + cx * 256;
    oy = ((c * x0) & ~63) + ((d * y) & ~63) + ((d * y0) & ~63) + cy * 256;
    hx = I_(x) - I_(FD_(CP_SL));
    sx = msize > 1u ? cp_floor_to(hx, I_(msize)) : hx;
    if ((m7sel & 1u) != 0u)
        sx = 255 - sx;
    tpx = (ox + a * sx) >> 8;
    tpy = (oy + c * sx) >> 8;
    if (((tpx | tpy) & ~0x3FF) != 0) {
        if ((m7sel & 0xC0u) == 0x80u)
            return 0u;
        if ((m7sel & 0xC0u) == 0xC0u)
            tile = 0u;
        else
            tile = cp_vram(si, page, U_(((tpy >> 3) & 127) << 7 | ((tpx >> 3) & 127))) & 0xFFu;
    } else {
        tile = cp_vram(si, page, U_((tpy >> 3) << 7 | (tpx >> 3))) & 0xFFu;
    }
    pix = cp_vram(si, page, (tile * 64u + U_(tpy & 7) * 8u + U_(tpx & 7)) & 0x7FFFu) >> 8;
    if (n == 1u) {
        prio = pix >> 7;
        pix &= 0x7Fu;
    }
    if (pix == 0u)
        return prio << 23;
    return CP_LP((cp_b(si, U_(CP_S_TSW), 1u) & 1u) != 0u && n == 0u ? cp_direct(pix, 0u) : cp_pal(si, pix), pix, prio);
}

/* the sprites' pixel at the screen's column x of row part p (ppu.c
   draw_obj): colour | priority << 16 | maths << 18 | opaque << 24, or 0 */
CP_FN U32 cp_obj(U32 si, U32 p, U32 x)
{
    U32 at = FD_(FD_(CP_PARTS) + 4u * p + 2u), n = FD_(FD_(CP_PARTS) + 4u * p + 3u), i;
    U32 obsel = cp_b(si, U_(CP_S_INIDISP), 1u), page = cp_b(si, U_(CP_S_PAGE4), 0u);
    U32 base = (obsel & 7u) << 13, gap = (((obsel >> 3) & 3u) + 1u) << 12;
    I32 hx = I_(x) - I_(FD_(CP_SL));
    for (i = 0u; i < n; i++) {
        U32 e = FD_(CP_SPR) + (at + i) * U_(CP_SE);
        I32 sxp = I_(FD_(e));
        U32 f = FD_(e + 1u), name = FD_(e + 2u);
        U32 r = f & 0xFFu, cols = (f >> 8) & 15u, hflip = (f >> 12) & 1u, pal = (f >> 13) & 7u, prio = (f >> 16) & 3u;
        I32 keep = I_((f >> 18) & 63u) - 1;
        I32 dx = hx - sxp, first;
        U32 c, px, col, ix, t, ca, ci, kept;
        if (dx < 0 || dx >= I_(cols * 8u))
            continue;
        c = U_(dx) >> 3;
        px = U_(dx) & 7u;
        /* the console's tiles: in its columns, those it keeps (counted
           from its first tile in them) */
        if (hx >= 0 && hx < 256) {
            if (FD_(CP_LIMITS) != 0u) {
                first = sxp > -8 ? 0 : (-sxp - 8) / 8 + 1;
                kept = keep >= 0 && I_(c) - first < keep ? 1u : 0u;
                if (kept == 0u)
                    continue;
            }
        }
        col = hflip != 0u ? cols - 1u - c : c;
        ix = hflip != 0u ? 7u - px : px;
        t = (((name & 0xF0u) + ((r >> 3) << 4)) & 0xF0u) | ((name + col) & 0x0Fu);
        ca = (base + t * 16u + ((name & 0x100u) != 0u ? gap : 0u)) & 0xFFFFu;
        ci = cp_tile_pixel(si, page, ca, r & 7u, ix, 4u);
        if (ci == 0u)
            continue;
        ci = 128u + pal * 16u + ci;
        return cp_pal(si, ci) | prio << 16 | (pal >= 4u ? 1u : 0u) << 18 | 0x1000000u;
    }
    return 0u;
}

CP_FN U32 cp_win(U32 si, U32 sel, U32 logic, I32 x)
{
    U32 w1 = x >= I_(cp_b(si, U_(CP_S_WH), 0u)) && x <= I_(cp_b(si, U_(CP_S_WH), 1u)) ? 1u : 0u;
    U32 w2 = x >= I_(cp_b(si, U_(CP_S_WH), 2u)) && x <= I_(cp_b(si, U_(CP_S_WH), 3u)) ? 1u : 0u;
    U32 e1 = sel & 2u, e2 = sel & 8u;
    if (e1 == 0u && e2 == 0u)
        return 0u;
    if ((sel & 1u) != 0u) w1 ^= 1u;
    if ((sel & 4u) != 0u) w2 ^= 1u;
    if (e1 != 0u && e2 == 0u) return w1;
    if (e2 != 0u && e1 == 0u) return w2;
    logic &= 3u;
    if (logic == 0u) return w1 | w2;
    if (logic == 1u) return w1 & w2;
    if (logic == 2u) return w1 ^ w2;
    return (w1 ^ w2) ^ 1u;
}

CP_FN U32 cp_sel(U32 si, U32 layer)
{
    if (layer < 4u)
        return (cp_b(si, U_(CP_S_WSEL), layer >> 1) >> (4u * (layer & 1u))) & 0x0Fu;
    return (cp_b(si, U_(CP_S_WSEL), 2u) >> (4u * (layer - 4u))) & 0x0Fu;
}

CP_FN U32 cp_logic(U32 si, U32 layer)
{
    if (layer < 4u)
        return cp_b(si, U_(CP_S_WSEL), 3u) >> (layer * 2u);
    return cp_b(si, U_(CP_S_TM), 0u) >> ((layer - 4u) * 2u);
}

CP_FN U32 cp_clipped(U32 si, U32 n, U32 x, I32 dr)
{
    I32 sl = I_(FD_(CP_SL));
    return (cp_b(si, U_(CP_S_TSW), 3u) >> n & 1u) != 0u &&
                   (I_(x) < sl || I_(x) >= sl + 256 || dr < 0 || dr >= I_(FD_(CP_LINES)))
               ? 1u
               : 0u;
}

/* a screen's pixel: colour | depth << 16 | layer << 21 */
#define CP_PX(color, z, layer) ((color) | (z) << 16 | (layer) << 21)

/* put a layer's pixel (m on the main screen, b on the sub) at depth z
   (ppu.c put_layer); which (0 main, 1 sub) */
CP_FN U32 cp_put(U32 si, U32 layer, U32 lp, U32 zz, U32 cur, U32 which, U32 x, I32 dr)
{
    U32 on = ((cp_b(si, U_(CP_S_TM), which != 0u ? 2u : 1u) & ~cp_tmask) >> layer) & 1u;
    U32 tmw = cp_b(si, U_(CP_S_TM), 3u), tsw = cp_b(si, U_(CP_S_TSW), 0u), wn = which != 0u ? tsw : tmw, inside = 0u;
    if (on == 0u || (lp & 0x1000000u) == 0u || zz == 0u || zz <= ((cur >> 16) & 31u) || cp_clipped(si, layer, x, dr) != 0u)
        return cur;
    if ((((tmw | tsw) >> layer) & 1u) != 0u)
        inside = cp_win(si, cp_sel(si, layer), cp_logic(si, layer), I_(x) - I_(FD_(CP_SL)));
    if (inside != 0u && ((wn >> layer) & 1u) != 0u)
        return cur;
    return CP_PX(lp & 0x7FFFu, zz, layer);
}

/* background n's depth for priority prio in the mode (draw_line's tables) */
CP_FN U32 cp_zbg(U32 bgmode, U32 n, U32 prio)
{
    U32 mode = bgmode & 7u;
    if (mode == 0u) return n == 0u ? (prio != 0u ? 11u : 8u) : n == 1u ? (prio != 0u ? 10u : 7u) : n == 2u ? (prio != 0u ? 5u : 2u) : (prio != 0u ? 4u : 1u);
    if (mode == 1u) {
        if (n == 0u) return prio != 0u ? 9u : 6u;
        if (n == 1u) return prio != 0u ? 8u : 5u;
        return prio != 0u ? ((bgmode & 8u) != 0u ? 11u : 3u) : 1u;
    }
    if (mode == 7u) return n == 0u ? 3u : (prio != 0u ? 5u : 1u);
    return n == 0u ? (prio != 0u ? 7u : 3u) : (prio != 0u ? 5u : 1u);
}

CP_FN U32 cp_zobj(U32 mode, U32 prio)
{
    if (mode == 0u) return 3u + 3u * prio;
    if (mode == 1u) return prio == 0u ? 2u : prio == 1u ? 4u : prio == 2u ? 7u : 10u;
    if (mode == 7u) return prio == 0u ? 2u : prio == 1u ? 4u : prio == 2u ? 6u : 7u;
    return 2u + 2u * prio;
}

CP_FN U32 cp_region(U32 mode, U32 inside)
{
    mode &= 3u;
    if (mode == 0u) return 0u;
    if (mode == 1u) return inside ^ 1u;
    if (mode == 2u) return inside;
    return 1u;
}

/* one pixel's colour out of its screen m and the other one o (ppu.c finish) */
CP_FN U32 cp_finish(U32 si, U32 m, U32 o, I32 x)
{
    U32 cgwsel = cp_b(si, U_(CP_S_TSW), 1u), cgadsub = cp_b(si, U_(CP_S_TSW), 2u);
    U32 fixc = cp_b(si, U_(CP_S_FIXED), 0u) | cp_b(si, U_(CP_S_FIXED), 1u) << 5 | cp_b(si, U_(CP_S_FIXED), 2u) << 10;
    U32 bright = cp_b(si, U_(CP_S_INIDISP), 0u) & 0x0Fu;
    U32 cw = cp_win(si, cp_sel(si, 5u), cp_logic(si, 5u), x);
    U32 clip = cp_region(cgwsel >> 6, cw), prevent = cp_region(cgwsel >> 4, cw);
    U32 ml = (m >> 21) & 7u, bit = ml == 5u ? 5u : ml == 6u ? 8u : ml;
    U32 c = clip != 0u ? 0u : m & 0x7FFFu, r = c & 31u, g = (c >> 5) & 31u, b = (c >> 10) & 31u;
    if (bit < 8u && ((cgadsub >> bit) & 1u) != 0u && prevent == 0u) {
        U32 oc, halve = (cgadsub & 0x40u) != 0u && clip == 0u ? 1u : 0u;
        if ((cgwsel & 2u) != 0u) {
            if (((o >> 21) & 7u) == 5u) {
                oc = fixc;
                halve = 0u;
            } else {
                oc = o & 0x7FFFu;
            }
        } else {
            oc = fixc;
        }
        if ((cgadsub & 0x80u) != 0u) {
            r = r > (oc & 31u) ? r - (oc & 31u) : 0u;
            g = g > ((oc >> 5) & 31u) ? g - ((oc >> 5) & 31u) : 0u;
            b = b > ((oc >> 10) & 31u) ? b - ((oc >> 10) & 31u) : 0u;
            if (halve != 0u) {
                r >>= 1;
                g >>= 1;
                b >>= 1;
            }
        } else {
            r += oc & 31u;
            g += (oc >> 5) & 31u;
            b += (oc >> 10) & 31u;
            if (halve != 0u) {
                r >>= 1;
                g >>= 1;
                b >>= 1;
            }
            if (r > 31u) r = 31u;
            if (g > 31u) g = 31u;
            if (b > 31u) b = 31u;
        }
    }
    if (bright != 15u) {
        r = (r * bright + 7u) / 15u;
        g = (g * bright + 7u) / 15u;
        b = (b * bright + 7u) / 15u;
    }
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* the main and sub screens' pixels at the screen's column x of part p,
   state si (ppu.c draw_line): into cp_main and cp_sub */
CP_FN void cp_screens(U32 si, U32 p, U32 x, I32 dr, U32 my)
{
    U32 bgmode = cp_b(si, U_(CP_S_INIDISP), 2u), mode = bgmode & 7u, n, lp, lps, ob;
    U32 tm = cp_b(si, U_(CP_S_TM), 1u) & ~cp_tmask, ts = cp_b(si, U_(CP_S_TM), 2u) & ~cp_tmask;
    U32 setini = cp_b(si, U_(CP_S_NBA), 3u), bg_hires = mode == 5u || mode == 6u ? 1u : 0u;
    U32 fixc = cp_b(si, U_(CP_S_FIXED), 0u) | cp_b(si, U_(CP_S_FIXED), 1u) << 5 | cp_b(si, U_(CP_S_FIXED), 2u) << 10;
    cp_main = CP_PX(cp_pal(si, 0u), 0u, 5u);
    cp_sub = CP_PX(fixc, 0u, 5u);
    if (mode == 7u) {
        if (((tm | ts) & 1u) != 0u) {
            lp = cp_m7(si, 0u, x, dr, my);
            cp_main = cp_put(si, 0u, lp, cp_zbg(bgmode, 0u, (lp >> 23) & 1u), cp_main, 0u, x, dr);
            cp_sub = cp_put(si, 0u, lp, cp_zbg(bgmode, 0u, (lp >> 23) & 1u), cp_sub, 1u, x, dr);
        }
        if ((setini & 0x40u) != 0u && ((tm | ts) & 2u) != 0u) {
            lp = cp_m7(si, 1u, x, dr, my);
            cp_main = cp_put(si, 1u, lp, cp_zbg(bgmode, 1u, (lp >> 23) & 1u), cp_main, 0u, x, dr);
            cp_sub = cp_put(si, 1u, lp, cp_zbg(bgmode, 1u, (lp >> 23) & 1u), cp_sub, 1u, x, dr);
        }
    } else {
        for (n = 0u; n < 4u; n++) {
            if (cp_bpp(mode, n) == 0u || (((tm | ts) >> n) & 1u) == 0u)
                continue;
            /* a hires layer's 512 pixels: the main screen the odd ones,
               the sub screen the even */
            if (bg_hires != 0u) {
                lp = (tm >> n & 1u) != 0u ? cp_bg(si, n, 2u * x + 1u, dr, my) : 0u;
                lps = (ts >> n & 1u) != 0u ? cp_bg(si, n, 2u * x, dr, my) : 0u;
            } else {
                lp = cp_bg(si, n, x, dr, my);
                lps = lp;
            }
            cp_main = cp_put(si, n, lp, cp_zbg(bgmode, n, (lp >> 23) & 1u), cp_main, 0u, x, dr);
            cp_sub = cp_put(si, n, lps, cp_zbg(bgmode, n, (lps >> 23) & 1u), cp_sub, 1u, x, dr);
        }
    }
    if (((tm | ts) & 0x10u) != 0u) {
        ob = cp_obj(si, p, x);
        if (ob != 0u && cp_clipped(si, 4u, x, dr) == 0u) {
            U32 zz = cp_zobj(mode, (ob >> 16) & 3u), layer = ((ob >> 18) & 1u) != 0u ? 4u : 6u;
            U32 tmw = cp_b(si, U_(CP_S_TM), 3u), tsw = cp_b(si, U_(CP_S_TSW), 0u), inside = 0u;
            if ((((tmw | tsw) >> 4) & 1u) != 0u)
                inside = cp_win(si, cp_sel(si, 4u), cp_logic(si, 4u), I_(x) - I_(FD_(CP_SL)));
            if ((tm >> 4 & 1u) != 0u && zz > ((cp_main >> 16) & 31u) && (inside == 0u || ((tmw >> 4) & 1u) == 0u))
                cp_main = CP_PX(ob & 0x7FFFu, zz, layer);
            if ((ts >> 4 & 1u) != 0u && zz > ((cp_sub >> 16) & 31u) && (inside == 0u || ((tsw >> 4) & 1u) == 0u))
                cp_sub = CP_PX(ob & 0x7FFFu, zz, layer);
        }
    }
}

/* the picture's pixel (X, R): 0xFFRRGGBB (ppu.c ppu_draw_frame) */
CP_FN U32 cp_pixel(U32 X, U32 R)
{
    U32 ri = FD_(CP_ROWS) + 4u * R, p0 = FD_(ri), np = FD_(ri + 1u), my = FD_(ri + 3u), p = p0, k;
    I32 dr = I_(FD_(ri + 2u));
    U32 pw = FD_(CP_PW), sw = FD_(CP_SW), sl = FD_(CP_SL), scale = pw == sw ? 1u : 2u, x = X >> (scale - 1u), si, bgmode, m, sb;
    for (k = 1u; k < np; k++)
        if ((FD_(FD_(CP_PARTS) + 4u * (p0 + k)) + sl) * scale <= X)
            p = p0 + k;
    si = FD_(FD_(CP_PARTS) + 4u * p + 1u);
    cp_load(si);
    cp_tmask = 0u;
    if ((cp_b(si, U_(CP_S_INIDISP), 0u) & 0x80u) != 0u)
        return 0xFF000000u;
    bgmode = cp_b(si, U_(CP_S_INIDISP), 2u);
    if (scale == 2u && ((bgmode & 7u) == 5u || (bgmode & 7u) == 6u || (cp_b(si, U_(CP_S_NBA), 3u) & 8u) != 0u)) {
        /* a hires line: the sub screen's pixel in its own right (its
           backdrop the main screen's), then the main screen's; its first
           half pixel black */
        if (X == 0u)
            return 0xFF000000u;
        cp_screens(si, p, x, dr, my);
        m = cp_main;
        if ((X & 1u) != 0u)
            return cp_finish(si, m, cp_sub, I_(x) - I_(sl));
        sb = cp_sub;
        if (((sb >> 21) & 7u) == 5u)
            sb = CP_PX(cp_pal(si, 0u), (sb >> 16) & 31u, 5u);
        return cp_finish(si, sb, m, I_(x) - I_(sl));
    }
    cp_screens(si, p, x, dr, my);
    return cp_finish(si, cp_main, cp_sub, I_(x) - I_(sl));
}

/* An interlaced frame's woven picture's pixel (X, Y), ww wide (ppu.c
   weave): each row parity's rows from the description at word base0 or
   base1 (the finished frame's, or the frame before's), doubled where it is
   half as wide */
CP_FN U32 cp_woven(U32 X, U32 Y, U32 ww, U32 base0, U32 base1)
{
    U32 pw;
    cp_fdb = (Y & 1u) != 0u ? base1 : base0;
    pw = FD_(CP_PW);
    return cp_pixel(pw < ww ? X >> 1 : X, Y >> 1);
}

/* ---- a frame presenting Mode 7 or a canvas: each picture pixel's record ---- */
/* (rr_mode7.h RrM7Px, ppu.c ppu_mode7) into cp_r0 to cp_r3 */
CP_VAR U32 cp_r0, cp_r1, cp_r2, cp_r3;

#define CP_R_NOT 0x0001u
#define CP_R_BLACK 0x0002u
#define CP_R_SWAP 0x0004u
#define CP_R_BG1_MAIN 0x0008u
#define CP_R_BG1_SUB 0x0010u
#define CP_R_BG2_MAIN 0x0020u
#define CP_R_BG2_SUB 0x0040u
#define CP_R_CLIP 0x0080u
#define CP_R_PREVENT 0x0100u
#define CP_R_BG1_FIX 0x0200u
#define CP_R_BG2_FIX 0x0400u
#define CP_R_CV 0x0800u

/* whether layer n may show on a screen (which 1: the sub) at the screen's
   column x: its enable bit, the clip and the windows (ppu.c cv_may, m7_may) */
CP_FN U32 cp_may(U32 si, U32 n, U32 x, I32 dr, U32 which)
{
    U32 on = (cp_b(si, U_(CP_S_TM), which != 0u ? 2u : 1u) >> n) & 1u;
    U32 tmw = cp_b(si, U_(CP_S_TM), 3u), tsw = cp_b(si, U_(CP_S_TSW), 0u), wn = which != 0u ? tsw : tmw, inside = 0u;
    if (on == 0u || cp_clipped(si, n, x, dr) != 0u)
        return 0u;
    if ((((tmw | tsw) >> n) & 1u) != 0u)
        inside = cp_win(si, cp_sel(si, n), cp_logic(si, n), I_(x) - I_(FD_(CP_SL)));
    return inside != 0u && ((wn >> n) & 1u) != 0u ? 0u : 1u;
}

/* a screen's pixel of the sprites over the backdrop (ppu.c m7_base):
   colour | depth << 16 | layer << 21 */
CP_FN U32 cp_m7_base(U32 si, U32 ob, U32 x, I32 dr, U32 which)
{
    U32 fixc = cp_b(si, U_(CP_S_FIXED), 0u) | cp_b(si, U_(CP_S_FIXED), 1u) << 5 | cp_b(si, U_(CP_S_FIXED), 2u) << 10;
    U32 base = which != 0u ? CP_PX(fixc, 0u, 5u) : CP_PX(cp_pal(si, 0u), 0u, 5u);
    if (ob == 0u || cp_may(si, 4u, x, dr, which) == 0u)
        return base;
    return CP_PX(ob & 0x7FFFu, cp_zobj(7u, (ob >> 16) & 3u), ((ob >> 18) & 1u) != 0u ? 4u : 6u);
}

CP_FN void cp_record(U32 X, U32 R)
{
    U32 ri = FD_(CP_ROWS) + 4u * R, p0 = FD_(ri), np = FD_(ri + 1u), my = FD_(ri + 3u), p = p0, k;
    I32 dr = I_(FD_(ri + 2u));
    U32 pw = FD_(CP_PW), sw = FD_(CP_SW), sl = FD_(CP_SL), cpp = pw == sw ? 1u : 2u, x = X >> (cpp - 1u);
    U32 si, g, inidisp, bgmode, mode, tm, ts, setini, mosaic, cgwsel, cw, flags = 0u, fix = 0u;
    for (k = 1u; k < np; k++)
        if ((FD_(FD_(CP_PARTS) + 4u * (p0 + k)) + sl) * cpp <= X)
            p = p0 + k;
    si = FD_(FD_(CP_PARTS) + 4u * p + 1u);
    g = FD_(FD_(CP_M7SEG) + p);
    cp_load(si);
    cp_tmask = 0u;
    cp_r0 = 0u;
    cp_r1 = 0u;
    cp_r2 = CP_R_NOT;
    cp_r3 = 0u;
    inidisp = cp_b(si, U_(CP_S_INIDISP), 0u);
    bgmode = cp_b(si, U_(CP_S_INIDISP), 2u);
    mode = bgmode & 7u;
    mosaic = cp_b(si, U_(CP_S_INIDISP), 3u);
    tm = cp_b(si, U_(CP_S_TM), 1u);
    ts = cp_b(si, U_(CP_S_TM), 2u);
    setini = cp_b(si, U_(CP_S_NBA), 3u);
    cgwsel = cp_b(si, U_(CP_S_TSW), 1u);
    if ((inidisp & 0x80u) != 0u)
        return;
    cw = cp_win(si, cp_sel(si, 5u), cp_logic(si, 5u), I_(x) - I_(sl));
    if (cp_region(cgwsel >> 6, cw) != 0u)
        flags |= CP_R_CLIP;
    if (cp_region(cgwsel >> 4, cw) != 0u)
        flags |= CP_R_PREVENT;
    {
        /* the canvas's layer (ppu.c pres_is_cv): the screens drawn
           without it, then its character's place in the canvas */
        U32 cn = FD_(CP_CVLAYER);
        if (FD_(CP_CVON) != 0u && mode != 7u && cp_bpp(mode, cn) != 0u && (((tm | ts) >> cn) & 1u) != 0u &&
            mode != 5u && mode != 6u && (setini & 8u) == 0u && !((mosaic & (1u << cn)) != 0u && (mosaic >> 4) != 0u)) {
            cp_tmask = 1u << cn;
            cp_screens(si, p, x, dr, my);
            cp_tmask = 0u;
            cp_cvat = 0xFFFFFFFFu;
            cp_bg(si, cn, x, dr, my);
            if (cp_cvat == 0xFFFFFFFFu)
                return;
            flags |= CP_R_CV;
            if (cp_may(si, cn, x, dr, 0u) != 0u) flags |= CP_R_BG1_MAIN;
            if (cp_may(si, cn, x, dr, 1u) != 0u) flags |= CP_R_BG1_SUB;
            cp_r0 = (cp_main & 0x7FFFu) | (cp_sub & 0x7FFFu) << 16;
            cp_r1 = ((cp_main >> 16) & 31u) | ((cp_sub >> 16) & 31u) << 8 | ((cp_main >> 21) & 7u) << 16 |
                    ((cp_sub >> 21) & 7u) << 24;
            cp_r2 = flags | g << 16;
            cp_r3 = (cp_cvat & 0x1FFFFFFu) | cp_zbg(bgmode, cn, (cp_cvat >> 25) & 1u) << 25 | cn << 29;
            return;
        }
    }
    /* a Mode 7 line (ppu.c pres_is_m7) */
    if (mode != 7u || !(((tm | ts) & 1u) != 0u || ((setini & 0x40u) != 0u && ((tm | ts) & 2u) != 0u)))
        return;
    {
        U32 ob = ((tm | ts) & 0x10u) != 0u ? cp_obj(si, p, x) : 0u, mb, sb;
        mb = cp_m7_base(si, ob, x, dr, 0u);
        sb = cp_m7_base(si, ob, x, dr, 1u);
        if (cp_may(si, 0u, x, dr, 0u) != 0u) flags |= CP_R_BG1_MAIN;
        if (cp_may(si, 0u, x, dr, 1u) != 0u) flags |= CP_R_BG1_SUB;
        if ((setini & 0x40u) != 0u) {
            if (cp_may(si, 1u, x, dr, 0u) != 0u) flags |= CP_R_BG2_MAIN;
            if (cp_may(si, 1u, x, dr, 1u) != 0u) flags |= CP_R_BG2_SUB;
        }
        if ((mosaic >> 4) != 0u && (((flags & (CP_R_BG1_MAIN | CP_R_BG1_SUB)) != 0u && (mosaic & 1u) != 0u) ||
                                    ((flags & (CP_R_BG2_MAIN | CP_R_BG2_SUB)) != 0u && (mosaic & 2u) != 0u))) {
            /* under mosaic: the block's texel (ppu.c m7_mosaic_texel) */
            U32 m7sel = cp_b(si, U_(CP_S_NBA), 2u), msize = (mosaic >> 4) + 1u, t;
            I32 y = I_(my), sx = cp_floor_to(I_(x) - I_(sl), I_(msize)), ox, oy;
            I32 a = cp_s16(cp_h(si, U_(CP_S_M7AB), 0u)), c = cp_s16(cp_h(si, U_(CP_S_M7CD), 0u));
            if ((m7sel & 2u) != 0u)
                y = 255 - y;
            ox = cp_m7_origin(si, y, 0u);
            oy = cp_m7_origin(si, y, 1u);
            if ((m7sel & 1u) != 0u)
                sx = 255 - sx;
            t = cp_m7_texel(si, (ox + a * sx) >> 8, (oy + c * sx) >> 8);
            if ((flags & (CP_R_BG1_MAIN | CP_R_BG1_SUB)) != 0u && (mosaic & 1u) != 0u) {
                flags |= CP_R_BG1_FIX;
                fix |= t;
            }
            if ((flags & (CP_R_BG2_MAIN | CP_R_BG2_SUB)) != 0u && (mosaic & 2u) != 0u) {
                flags |= CP_R_BG2_FIX;
                fix |= t << 8;
            }
        }
        if (cpp == 2u && (setini & 8u) != 0u) {
            /* pseudo-hires (Mode 7 has no other) */
            if (X == 0u)
                flags |= CP_R_BLACK;
            else if ((X & 1u) == 0u)
                flags |= CP_R_SWAP;
        }
        cp_r0 = (mb & 0x7FFFu) | (sb & 0x7FFFu) << 16;
        cp_r1 = ((mb >> 16) & 31u) | ((sb >> 16) & 31u) << 8 | ((mb >> 21) & 7u) << 16 | ((sb >> 21) & 7u) << 24;
        cp_r2 = flags | g << 16;
        cp_r3 = fix;
    }
}
