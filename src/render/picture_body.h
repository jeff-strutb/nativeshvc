/*
 * picture_body.h: the SNES's picture rules for one pixel, in one source for
 * the C-like shading languages. Each backend's prelude gives:
 *
 *   PB_FN        a function's qualifiers
 *   PB_VAR       a variable private to the pixel being drawn
 *   U32, I32     32-bit unsigned and signed integers
 *   U_(x), I_(x) conversions to them
 *   D_(i)        word i of this picture's description
 *   T_(i)        word i of the store of tiles, maps and images on the GPU
 *
 * The description, in 32-bit words (picture.c writes it):
 *   header   PB_* below
 *   rows     for each row: first part, parts, first sprite entry, entries
 *   parts    for each part of a row: its first column, its state
 *   states   PB_ST words each
 *   palettes 256 colours each: 15-bit BGR, or PB_RGB | 0xRRGGBB
 *   sprites  PB_SP words each
 *   entries  for each row, the sprites on it in priority order: the sprite |
 *            tiles kept + 1 << 24 (0 none: no limit)
 */
#define PB_W 0
#define PB_H 1
#define PB_OX 2          /* the original's 256 x 224 on the screen */
#define PB_OY 3
#define PB_ROWS 4
#define PB_PARTS 5
#define PB_STATES 6
#define PB_SPRITES 7
#define PB_ENTRIES 8
#define PB_CLIP 9        /* bits 0-4: BG1-4, sprites kept to the original's 256 x 224; bit 8: sprites wrap at 256 rows */
#define PB_LINES 10      /* the original's rows: 224, or 239 with overscan */
#define PB_PW 11         /* the picture's width: W, or twice W with a hires lineno */
#define PB_STEPU 12      /* one window pixel, in 1/1024 of a picture pixel across (of a console pixel) */
#define PB_STEPV 13
#define PB_WW 14         /* the window's pixels */
#define PB_WH 15
#define PB_DU 16         /* the supersample offsets: 4, then 8, then 16 (rr_samples) */
#define PB_DV (16 + 28)
#define PB_OVERLAYS (16 + 56)
#define PB_OVCOUNT (17 + 56)
#define PB_FIELD (18 + 56) /* 0 not interlaced; 1, 2: the field drawn, rows 0, 2.. or 1, 3.. of a picture twice as tall */
#define PB_HEADER (19 + 56)

#define PB_RGB 0x1000000u

/* a state's words */
#define PB_S_MODE 0      /* mode | bg3 priority << 3 | hires << 4 | brightness << 8 | blank << 12 | sprites on main << 13, on sub << 14 */
#define PB_S_BLEND 1     /* fixed | subtract << 16 | half << 17 | use_fixed << 18 | layers << 20
                            | clip << 26 | prevent << 28 */
#define PB_S_W 2         /* window 1's left, right, window 2's left, right (signed) */
#define PB_S_USE 6       /* layers' window use: BG1-4, a byte each */
#define PB_S_USE2 7      /* sprites' | colour's << 8 | masked main << 16 | masked sub << 24 */
#define PB_S_M7 8        /* a | b << 16; c | d << 16; cx | cy << 16; sx | sy << 16 */
#define PB_S_M7F 12      /* outside | flip_x << 2 | flip_y << 3 | extbg << 4 */
#define PB_S_M7T 13      /* Mode 7's tiles in the store */
#define PB_S_M7M 14      /* its map in the store */
#define PB_S_PAL 15      /* the palette's first word */
#define PB_S_L 16        /* the four layers, PB_L words each */
#define PB_L 8
#define PB_ST (16 + 4 * 8)

/* a layer's words */
#define PB_L_F 0         /* main | sub << 1 | big tiles << 2 | mosaic - 1 << 4 | wrap << 9 | direct << 10
                            | kind << 11 (0 none, 1 tiles, 2 image, 3 draw list) | bpp << 13 */
#define PB_L_T 1         /* its tiles in the store, or its image */
#define PB_L_A 2         /* texels a pixel (1) | the row its mosaic starts at << 16 (signed) */
#define PB_L_M 3         /* its map in the store, or its image's width | height << 16 */
#define PB_L_S 4         /* the map's width | height << 16, in tiles */
#define PB_L_X 5
#define PB_L_Y 6
#define PB_L_O 7         /* its offset-per-tile map, or 0 */

/* a sprite's words */
#define PB_SP 5
#define PB_P_X 0
#define PB_P_Y 1
#define PB_P_S 2         /* width | height << 16, in tiles */
#define PB_P_T 3         /* its tiles in the store */
#define PB_P_N 4         /* tile | palette << 16 | priority << 19 | hflip << 21 | vflip << 22 */

/* an overlay's words: 0xAARRGGBB texels from the store, placed by the window
   pixel's texel = M * (x, y, 1); floats by their bits */
#define PB_OV 9
#define PB_V_T 0
#define PB_V_S 1         /* width | height << 16 */
#define PB_V_M 2         /* six floats */
#define PB_V_A 8         /* alpha */

/* a map entry in the store: tile | palette << 16 | priority << 19 | hflip << 20 | vflip << 21 */

PB_VAR I32 pb_wx, pb_wy; /* the window pixel a composition is for; -1: the picture's own */
PB_VAR U32 pb_s;         /* this pixel's state's first word */
PB_VAR U32 pb_c;         /* a layer's or the sprites' colour */
PB_VAR U32 pb_main, pb_sub, pb_mz, pb_sz, pb_ml, pb_sl;

#define S_(k) D_(pb_s + U_(k))

PB_FN I32 pb_floor(I32 x, I32 m) { return x >= 0 ? x - x % m : x - (m - 1 - (-x - 1) % m); }
PB_FN I32 pb_mod(I32 x, I32 m) { return x - pb_floor(x, m); }
PB_FN I32 pb_lo(U32 v) { return I_(v & 0xFFFFu) - ((v & 0x8000u) != 0u ? 0x10000 : 0); }
PB_FN I32 pb_hi(U32 v) { return pb_lo(v >> 16); }

PB_FN U32 pb_pal(U32 i) { return D_(S_(PB_S_PAL) + i); }

/* byte i of the store from word base */
PB_FN U32 pb_byte(U32 base, U32 i) { return (T_(base + (i >> 2)) >> (8u * (i & 3u))) & 0xFFu; }

PB_FN U32 pb_direct(U32 c, U32 pal)
{
    return ((c & 7u) << 2 | (pal & 1u) << 1) | ((c & 0x38u) >> 1 | (pal & 2u)) << 5 | ((c & 0xC0u) >> 3 | (pal & 4u)) << 10;
}

/* colour of pixel value c at a layer's depth */
PB_FN U32 pb_tile_colour(U32 c, U32 bpp, U32 pal, U32 n, U32 mode, U32 direct)
{
    if (bpp == 8u)
        return direct != 0u ? pb_direct(c, pal) : pb_pal(c);
    if (bpp == 4u)
        return pb_pal(pal * 16u + c);
    return pb_pal((mode == 0u ? n * 32u : 0u) + pal * 4u + c);
}

/* layer n's pixel at the screen's column x (twice as many on a hires lineno)
   and row r: pb_c its colour; opaque << 1 | priority */
PB_FN U32 pb_bg(U32 n, U32 x, I32 r)
{
    U32 l = PB_S_L + n * PB_L, f = S_(l + PB_L_F), kind = (f >> 11) & 3u;
    U32 mode = S_(PB_S_MODE) & 7u, hires = mode == 5u || mode == 6u ? 1u : 0u;
    U32 big = (f >> 2) & 1u, bpp = (f >> 13) & 15u, msize = ((f >> 4) & 31u) + 1u;
    I32 ox = I_(D_(PB_OX)), oy = I_(D_(PB_OY));
    I32 hx = I_(x) - (hires != 0u ? 2 * ox : ox), y = r - oy, xx, yy;
    I32 sx = I_(S_(l + PB_L_X)), sy = I_(S_(l + PB_L_Y));
    U32 mw = S_(l + PB_L_S) & 0xFFFFu, mh = S_(l + PB_L_S) >> 16;
    U32 xs = hires != 0u || big != 0u ? 4u : 3u, ys = big != 0u ? 4u : 3u;
    U32 tpp = S_(l + PB_L_A) & 0xFFFFu, opt = S_(l + PB_L_O), e, ch, pal, px, py, i, c, tw, th;
    if (kind == 0u)
        return 0u;
    if (msize > 1u) {
        I32 anchor = pb_hi(S_(l + PB_L_A));
        hx = pb_floor(hx, I_(hires != 0u ? 2u * msize : msize));
        y = anchor + pb_floor(y - anchor, I_(msize));
    }
    if (kind == 3u) {
        /* a draw list: at the window's pixels when composed for one */
        U32 lw = S_(l + PB_L_M) & 0xFFFFu, lh = S_(l + PB_L_M) >> 16, v, ix, iy;
        if (pb_wx >= 0) {
            ix = U_(pb_wx) * lw / D_(PB_WW);
            iy = U_(pb_wy) * lh / D_(PB_WH);
        } else {
            ix = (hires != 0u ? x >> 1 : x) * lw / D_(PB_W);
            iy = U_(r) * lh / D_(PB_H);
        }
        v = L_(n, ix, iy);
        if (v == 0u)
            return 0u;
        pb_c = v >= PB_RGB ? PB_RGB | (v & 0xFFFFFFu) : pb_pal(v & 0xFFu);
        return 2u;
    }
    if (kind == 2u) {
        /* an image over the screen from its top left; 0 clear */
        U32 iw = S_(l + PB_L_M) & 0xFFFFu, ih = S_(l + PB_L_M) >> 16, ix = x * iw / D_(PB_W), iy = U_(r) * ih / D_(PB_H), v;
        if (hires != 0u)
            ix = (x >> 1) * iw / D_(PB_W);
        v = T_(S_(l + PB_L_T) + iy * iw + ix);
        if (v == 0u)
            return 0u;
        pb_c = v >= PB_RGB ? PB_RGB | (v & 0xFFFFFFu) : pb_pal(v & 0xFFu);
        return 2u;
    }
    if (opt != 0u && n < 2u) {
        /* offset-per-tile: the scheme of modes 2, 4 and 6 */
        I32 col = hires != 0u ? (hx + (sx << 1 & 7)) >> 4 : (hx + (sx & 7)) >> 3;
        if (col > 0) {
            U32 l3 = PB_S_L + 2u * PB_L, w = S_(l3 + PB_L_S) & 0xFFFFu, h = S_(l3 + PB_L_S) >> 16;
            I32 x3 = I_(S_(l3 + PB_L_X)), y3 = I_(S_(l3 + PB_L_Y));
            U32 bx = U_(pb_mod(((col - 1) * 8 + (x3 & ~7)) >> 3, I_(w)));
            U32 hv = T_(opt + U_(pb_mod(y3 >> 3, I_(h))) * w + bx);
            U32 on = n == 0u ? (hv >> 19) & 1u : (hv >> 20) & 1u, bit15 = (hv >> 21) & 1u, v = hv & 0x3FFu;
            if (mode == 4u) {
                if (on != 0u) {
                    if (bit15 != 0u)
                        sy = I_(v);
                    else
                        sx = (sx & 7) | I_(v & ~7u);
                }
            } else {
                U32 vv = T_(opt + U_(pb_mod((y3 + 8) >> 3, I_(h))) * w + bx);
                if (on != 0u)
                    sx = (sx & 7) | I_(v & ~7u);
                if ((n == 0u ? (vv >> 19) & 1u : (vv >> 20) & 1u) != 0u)
                    sy = I_(vv & 0x3FFu);
            }
        }
    }
    xx = hx + (hires != 0u ? sx << 1 : sx);
    yy = y + 1;
    if (hires != 0u && D_(PB_FIELD) != 0u)
        yy = 2 * yy + (D_(PB_FIELD) == 2u && msize == 1u ? 1 : 0);
    yy += sy;
    tw = mw << xs;
    th = mh << ys;
    if (((f >> 9) & 1u) != 0u) {
        xx = pb_mod(xx, I_(tw));
        yy = pb_mod(yy, I_(th));
    } else if (xx < 0 || yy < 0 || U_(xx) >= tw || U_(yy) >= th) {
        return 0u;
    }
    e = T_(S_(l + PB_L_M) + (U_(yy) >> ys) * mw + (U_(xx) >> xs));
    px = U_(xx) & ((1u << xs) - 1u);
    py = U_(yy) & ((1u << ys) - 1u);
    if (((e >> 20) & 1u) != 0u)
        px = (1u << xs) - 1u - px;
    if (((e >> 21) & 1u) != 0u)
        py = (1u << ys) - 1u - py;
    ch = e & 0xFFFFu;
    if (px >= 8u)
        ch += 1u;
    if (py >= 8u)
        ch += 16u;
    i = ch * 64u * tpp * tpp + ((py & 7u) * tpp + (tpp >> 1)) * 8u * tpp + (px & 7u) * tpp + (tpp >> 1);
    c = pb_byte(S_(l + PB_L_T), i);
    pal = (e >> 16) & 7u;
    if (c == 0u)
        return (e >> 19) & 1u;
    pb_c = pb_tile_colour(c, bpp, pal, n, mode, (f >> 10) & 1u);
    return 2u | ((e >> 19) & 1u);
}

/* the texel byte of Mode 7's plane at (tx, ty); 0x100 clear */
PB_FN U32 pb_m7_texel(I32 tx, I32 ty)
{
    U32 outside = S_(PB_S_M7F) & 3u, tile;
    if (((tx | ty) & ~0x3FF) != 0) {
        if (outside == 1u)
            return 0x100u;
        if (outside == 2u)
            return pb_byte(S_(PB_S_M7T), U_(ty & 7) * 8u + U_(tx & 7));
    }
    tile = T_(S_(PB_S_M7M) + U_((ty >> 3) & 127) * 128u + U_((tx >> 3) & 127)) & 0xFFu;
    return pb_byte(S_(PB_S_M7T), tile * 64u + U_(ty & 7) * 8u + U_(tx & 7));
}

/* Mode 7's lookup: u in 1/1024 of a pixel from the original's column 0, e the
   same from row r's centre: pb_qx, pb_qy in 1/256 of a texel */
PB_VAR I32 pb_qx, pb_qy;

PB_FN void pb_m7_pos(I32 u, I32 r, I32 e)
{
    I32 a = pb_lo(S_(PB_S_M7)), b = pb_hi(S_(PB_S_M7)), c = pb_lo(S_(PB_S_M7 + 1)), d = pb_hi(S_(PB_S_M7 + 1));
    I32 cx = pb_lo(S_(PB_S_M7 + 2)), cy = pb_hi(S_(PB_S_M7 + 2)), hx = pb_lo(S_(PB_S_M7 + 3)), hy = pb_hi(S_(PB_S_M7 + 3));
    I32 x0 = hx - cx, y0 = hy - cy, f = I_(S_(PB_S_M7F)), lineno = r + 1, hu, lu, ox, oy;
    x0 = (x0 & 0x2000) != 0 ? x0 | ~0x3FF : x0 & 0x3FF;
    y0 = (y0 & 0x2000) != 0 ? y0 | ~0x3FF : y0 & 0x3FF;
    if ((f & 4) != 0)
        u = 255 * 1024 - u;
    if ((f & 8) != 0) {
        lineno = 255 - lineno;
        e = -e;
    }
    hu = u >> 10;
    lu = u & 1023;
    /* a lineno's origin is the console's sum, each product kept to 1/4 texel */
    ox = ((a * x0) & ~63) + ((b * lineno) & ~63) + ((b * y0) & ~63) + cx * 256;
    oy = ((c * x0) & ~63) + ((d * lineno) & ~63) + ((d * y0) & ~63) + cy * 256;
    pb_qx = ox + a * hu + ((a * lu + b * e) >> 10);
    pb_qy = oy + c * hu + ((c * lu + d * e) >> 10);
}

PB_FN U32 pb_m7_at(I32 u, I32 r, I32 e)
{
    pb_m7_pos(u, r, e);
    return pb_m7_texel(pb_qx >> 8, pb_qy >> 8);
}

/* the sprites' pixel at column x of row r: pb_c its colour; opaque << 3 |
   maths << 2 | priority */
PB_FN U32 pb_obj(U32 at, U32 count, U32 x, I32 r)
{
    U32 i;
    for (i = 0u; i < count; i++) {
        U32 en = D_(D_(PB_ENTRIES) + at + i), keep = en >> 24, sp = D_(PB_SPRITES) + (en & 0xFFFFFFu) * PB_SP;
        I32 dx = I_(x) - I_(D_(sp + PB_P_X)), dy = r - I_(D_(sp + PB_P_Y));
        U32 w = D_(sp + PB_P_S) & 0xFFFFu, h = D_(sp + PB_P_S) >> 16, n = D_(sp + PB_P_N), col, row, ix, iy, t, c, first;
        if (((D_(PB_CLIP) >> 8) & 1u) != 0u)
            dy = pb_mod(dy, 256);
        if (dx < 0 || dy < 0 || U_(dx) >= w * 8u || U_(dy) >= h * 8u)
            continue;
        if (keep != 0u) {
            /* tiles off the original's left edge don't count */
            I32 sx = I_(D_(sp + PB_P_X)) - I_(D_(PB_OX));
            first = sx > -8 ? 0u : U_((-sx - 8) / 8 + 1);
            if ((U_(dx) >> 3) >= first + keep - 1u)
                continue;
        }
        col = U_(dx) >> 3;
        row = U_(dy) >> 3;
        ix = U_(dx) & 7u;
        iy = U_(dy) & 7u;
        if (((n >> 21) & 1u) != 0u) {
            col = w - 1u - col;
            ix = 7u - ix;
        }
        if (((n >> 22) & 1u) != 0u) {
            row = h - 1u - row;
            iy = 7u - iy;
        }
        t = (n & 0xFF00u) | (((n & 0xF0u) + row * 16u) & 0xF0u) | (((n & 0xFFu) + col) & 0x0Fu);
        c = pb_byte(D_(sp + PB_P_T), t * 64u + iy * 8u + ix);
        if (c == 0u)
            continue;
        c = 128u + ((n >> 16) & 7u) * 16u + (c & 15u);
        pb_c = pb_pal(c);
        return 8u | (((n >> 16) & 7u) >= 4u ? 4u : 0u) | ((n >> 19) & 3u);
    }
    return 0u;
}

PB_FN U32 pb_use(U32 layer)
{
    if (layer < 4u)
        return (S_(PB_S_USE) >> (8u * layer)) & 0xFFu;
    return (S_(PB_S_USE2) >> (8u * (layer - 4u))) & 0xFFu;
}

/* whether screen column x is in the window `layer` uses: 1 in */
PB_FN U32 pb_win(U32 layer, I32 x)
{
    U32 use = pb_use(layer), e1 = use & 1u, e2 = use & 4u, logic = (use >> 4) & 3u, w1, w2;
    if (e1 == 0u && e2 == 0u)
        return 0u;
    w1 = x >= I_(S_(PB_S_W)) && x <= I_(S_(PB_S_W + 1)) ? 1u : 0u;
    w2 = x >= I_(S_(PB_S_W + 2)) && x <= I_(S_(PB_S_W + 3)) ? 1u : 0u;
    if ((use & 2u) != 0u)
        w1 ^= 1u;
    if ((use & 8u) != 0u)
        w2 ^= 1u;
    if (e1 != 0u && e2 == 0u)
        return w1;
    if (e2 != 0u && e1 == 0u)
        return w2;
    if (logic == 0u)
        return w1 | w2;
    if (logic == 1u)
        return w1 & w2;
    if (logic == 2u)
        return w1 ^ w2;
    return (w1 ^ w2) ^ 1u;
}

/* whether `layer` is kept off screen `which` (0 main, 1 sub) at (x, r) */
PB_FN U32 pb_off(U32 layer, U32 which, U32 x, I32 r)
{
    I32 ox = I_(D_(PB_OX)), oy = I_(D_(PB_OY));
    if (((D_(PB_CLIP) >> layer) & 1u) != 0u &&
        (I_(x) < ox || I_(x) >= ox + 256 || r < oy || r >= oy + I_(D_(PB_LINES))))
        return 1u;
    if (((S_(PB_S_USE2) >> (which != 0u ? 24u : 16u)) >> layer & 1u) != 0u && pb_win(layer, I_(x)) != 0u)
        return 1u;
    return 0u;
}

PB_FN U32 pb_zbg(U32 ms, U32 n, U32 prio)
{
    U32 mode = ms & 7u;
    if (mode == 0u)
        return n == 0u ? (prio != 0u ? 11u : 8u) : n == 1u ? (prio != 0u ? 10u : 7u) : n == 2u ? (prio != 0u ? 5u : 2u) : (prio != 0u ? 4u : 1u);
    if (mode == 1u) {
        if (n == 0u)
            return prio != 0u ? 9u : 6u;
        if (n == 1u)
            return prio != 0u ? 8u : 5u;
        return prio != 0u ? ((ms & 8u) != 0u ? 11u : 3u) : 1u;
    }
    if (mode == 7u)
        return n == 0u ? 3u : (prio != 0u ? 5u : 1u);
    return n == 0u ? (prio != 0u ? 7u : 3u) : (prio != 0u ? 5u : 1u);
}

PB_FN U32 pb_zobj(U32 mode, U32 prio)
{
    if (mode == 0u)
        return 3u + 3u * prio;
    if (mode == 1u)
        return prio == 0u ? 2u : prio == 1u ? 4u : prio == 2u ? 7u : 10u;
    if (mode == 7u)
        return prio == 0u ? 2u : prio == 1u ? 4u : prio == 2u ? 6u : 7u;
    return 2u + 2u * prio;
}

/* a layer's colour c at depth z onto the screens, where it may show there */
PB_FN void pb_put(U32 layer, U32 show, U32 c, U32 z, U32 which, U32 x, I32 r)
{
    if (which == 0u) {
        if (show != 0u && z > pb_mz && pb_off(layer, 0u, x, r) == 0u) {
            pb_main = c;
            pb_mz = z;
            pb_ml = layer;
        }
    } else {
        if (show != 0u && z > pb_sz && pb_off(layer, 1u, x, r) == 0u) {
            pb_sub = c;
            pb_sz = z;
            pb_sl = layer;
        }
    }
}

/* the screens at column x of row r, Mode 7's layers from byte pix (0x200:
   looked up at the pixel's own position) */
PB_FN void pb_screens(U32 sprites_at, U32 sprites, U32 x, I32 r, U32 pix)
{
    U32 ms = S_(PB_S_MODE), mode = ms & 7u, hires = mode == 5u || mode == 6u ? 1u : 0u, n, o;
    pb_main = pb_pal(0u);
    pb_mz = 0u;
    pb_ml = 5u;
    pb_sub = S_(PB_S_BLEND) & 0xFFFFu;
    pb_sz = 0u;
    pb_sl = 5u;
    if (mode == 7u) {
        U32 f0 = S_(PB_S_L + PB_L_F), f1 = S_(PB_S_L + PB_L + PB_L_F), p, m0 = f0 & 1u, s0 = (f0 >> 1) & 1u, ext = (S_(PB_S_M7F) >> 4) & 1u;
        if (pix == 0x200u) {
            /* under mosaic, the block's texel */
            U32 ms0 = ((f0 >> 4) & 31u) + 1u;
            I32 hx = I_(x) - I_(D_(PB_OX)), vy = r - I_(D_(PB_OY));
            if (ms0 > 1u) {
                I32 anchor = pb_hi(S_(PB_S_L + PB_L_A));
                hx = pb_floor(hx, I_(ms0));
                vy = anchor + pb_floor(vy - anchor, I_(ms0));
            }
            pix = pb_m7_at(hx * 1024, vy, 0);
        }
        p = pix & 0xFFu;
        if (pix < 0x100u && p != 0u) {
            U32 c = (S_(PB_S_L + PB_L_F) >> 10 & 1u) != 0u ? pb_direct(p, 0u) : pb_pal(p);
            pb_put(0u, m0, c, 3u, 0u, x, r);
            pb_put(0u, s0, c, 3u, 1u, x, r);
        }
        if (ext != 0u && pix < 0x100u && (p & 0x7Fu) != 0u) {
            U32 c = pb_pal(p & 0x7Fu), z = (p & 0x80u) != 0u ? 5u : 1u;
            pb_put(1u, f1 & 1u, c, z, 0u, x, r);
            pb_put(1u, (f1 >> 1) & 1u, c, z, 1u, x, r);
        }
    } else {
        for (n = 0u; n < 4u; n++) {
            U32 l = PB_S_L + n * PB_L, f = S_(l + PB_L_F), lp, lps, pc, psc;
            if (((f >> 11) & 3u) == 0u || (f & 3u) == 0u)
                continue;
            lp = pb_bg(n, hires != 0u ? 2u * x + 1u : x, r);
            pc = pb_c;
            if (hires != 0u) {
                lps = pb_bg(n, 2u * x, r);
                psc = pb_c;
            } else {
                lps = lp;
                psc = pc;
            }
            if ((lp & 2u) != 0u)
                pb_put(n, f & 1u, pc, pb_zbg(ms, n, lp & 1u), 0u, x, r);
            if ((lps & 2u) != 0u)
                pb_put(n, (f >> 1) & 1u, psc, pb_zbg(ms, n, lps & 1u), 1u, x, r);
        }
    }
    o = pb_obj(sprites_at, sprites, x, r);
    if ((o & 8u) != 0u) {
        U32 z = pb_zobj(mode, o & 3u), layer = (o & 4u) != 0u ? 4u : 6u, c = pb_c;
        if (((ms >> 13) & 1u) != 0u && z > pb_mz && pb_off(4u, 0u, x, r) == 0u) {
            pb_main = c;
            pb_mz = z;
            pb_ml = layer;
        }
        if (((ms >> 14) & 1u) != 0u && z > pb_sz && pb_off(4u, 1u, x, r) == 0u) {
            pb_sub = c;
            pb_sz = z;
            pb_sl = layer;
        }
    }
}

/* channel k of colour c in eighths of a console step */
PB_FN U32 pb_eighths(U32 c, U32 k)
{
    U32 v;
    if ((c & PB_RGB) == 0u)
        return ((c >> (5u * k)) & 31u) << 3;
    v = (c >> (16u - 8u * k)) & 0xFFu;
    return v - (v >> 5);
}

PB_FN U32 pb_region(U32 mode, U32 inside)
{
    mode &= 3u;
    if (mode == 0u)
        return 0u;
    if (mode == 1u)
        return inside ^ 1u;
    if (mode == 2u)
        return inside;
    return 1u;
}

/* one pixel's colour out of screen colours m, o (m's layer ml, o's ol) at x */
PB_FN U32 pb_finish(U32 m, U32 ml, U32 o, U32 ol, I32 x)
{
    U32 b = S_(PB_S_BLEND), fixc = b & 0xFFFFu, bright = (S_(PB_S_MODE) >> 8) & 15u;
    U32 w = pb_win(5u, x), clip = pb_region(b >> 26, w), prevent = pb_region(b >> 28, w);
    U32 bit = ml == 6u ? 8u : ml, c = clip != 0u ? 0u : m, math, halve = 0u, oc = 0u, res = 0xFF000000u, k;
    math = bit < 6u && ((b >> (20u + bit)) & 1u) != 0u && prevent == 0u ? 1u : 0u;
    if (math != 0u) {
        halve = ((b >> 17) & 1u) != 0u && clip == 0u ? 1u : 0u;
        if (((b >> 18) & 1u) == 0u) {
            if (ol == 5u) {
                oc = fixc;
                halve = 0u;
            } else {
                oc = o;
            }
        } else {
            oc = fixc;
        }
    }
    if (((c | oc) & PB_RGB) != 0u) {
        for (k = 0u; k < 3u; k++) {
            U32 sh = 16u - 8u * k, z, y;
            if (math == 0u && bright == 15u && (c & PB_RGB) != 0u) {
                res |= c & (0xFFu << sh);
            } else {
                z = pb_eighths(c, k);
                if (math != 0u) {
                    y = pb_eighths(oc, k);
                    z = ((b >> 16) & 1u) != 0u ? (z > y ? z - y : 0u) : z + y;
                    if (halve != 0u)
                        z = (z >> 4) << 3;
                    if (z > 248u)
                        z = 248u;
                }
                if (bright != 15u)
                    z = ((((z >> 3) * bright + 7u) * 4370u) >> 16) << 3;
                res |= (z + (z >> 5)) << sh;
            }
        }
        return res;
    }
    for (k = 0u; k < 3u; k++) {
        U32 v = (c >> (5u * k)) & 31u;
        if (math != 0u) {
            U32 y = (oc >> (5u * k)) & 31u;
            if (((b >> 16) & 1u) != 0u)
                v = v > y ? v - y : 0u;
            else
                v += y;
            if (halve != 0u)
                v >>= 1;
            if (v > 31u)
                v = 31u;
        }
        if (bright != 15u)
            v = (v * bright + 7u) / 15u;
        res |= (v << 3 | v >> 2) << (k == 0u ? 16u : k == 1u ? 8u : 0u);
    }
    return res;
}

/* this row's state at picture column X, pb_s set */
PB_FN void pb_state(U32 X, U32 R)
{
    U32 ri = D_(PB_ROWS) + 4u * R, p0 = D_(ri), np = D_(ri + 1u), p = p0, k, scale = D_(PB_PW) == D_(PB_W) ? 1u : 2u;
    for (k = 1u; k < np; k++)
        if (D_(D_(PB_PARTS) + 2u * (p0 + k)) * scale <= X)
            p = p0 + k;
    pb_s = D_(PB_STATES) + D_(D_(PB_PARTS) + 2u * p + 1u) * PB_ST;
}

/* a composed colour at picture column X, Mode 7's byte pix (0x200 its own) */
PB_FN U32 pb_compose(U32 X, U32 R, U32 pix)
{
    U32 ri = D_(PB_ROWS) + 4u * R, scale = D_(PB_PW) == D_(PB_W) ? 1u : 2u, x = X >> (scale - 1u), ms;
    pb_state(X, R);
    ms = S_(PB_S_MODE);
    if (((ms >> 12) & 1u) != 0u)
        return 0xFF000000u;
    pb_screens(D_(ri + 2u), D_(ri + 3u), x, I_(R), pix);
    if (scale == 2u && (((ms & 7u) == 5u) || ((ms & 7u) == 6u) || ((ms >> 4) & 1u) != 0u)) {
        /* a hires lineno: the sub screen's pixel, then the main's */
        if (X == 0u)
            return 0xFF000000u;
        if ((X & 1u) != 0u)
            return pb_finish(pb_main, pb_ml, pb_sub, pb_sl, I_(x));
        {
            U32 sb = pb_sl == 5u ? pb_pal(0u) : pb_sub;
            return pb_finish(sb, pb_sl, pb_main, pb_ml, I_(x));
        }
    }
    return pb_finish(pb_main, pb_ml, pb_sub, pb_sl, I_(x));
}

/* the picture's pixel */
PB_FN U32 pb_pixel(U32 X, U32 Y)
{
    pb_wx = -1;
    pb_wy = -1;
    return pb_compose(X, D_(PB_FIELD) != 0u ? Y >> 1 : Y, 0x200u);
}

/* whether target row Y is this picture's: an interlaced picture keeps the
   other field's rows from the picture before */
PB_FN U32 pb_row_kept(U32 Y) { return D_(PB_FIELD) == 0u || (Y & 1u) == D_(PB_FIELD) - 1u ? 0u : 1u; }

/* ---- at the window's pixels ---- */

PB_FN U32 pb_mix(U32 a, U32 b, F32 t)
{
    U32 o = 0xFF000000u, s;
    F32 w = floor(t * 256.0 + 0.5);
    for (s = 0u; s < 24u; s += 8u) {
        F32 ca = F_((a >> s) & 0xFFu), cb = F_((b >> s) & 0xFFu);
        o |= U_(I_(ca + floor(((cb - ca) * w + 128.0) / 256.0))) << s;
    }
    return o;
}

PB_FN I32 pb_fdiv(I32 a, I32 b) { return (a - pb_mod(a, b)) / b; }

/* the next texel's part of the footprint past its edge */
PB_FN I32 pb_edge(I32 f, I32 sa, I32 sb)
{
    I32 A = sa > sb ? sa : sb, B = sa > sb ? sb : sa, T = 2 * (128 - f), cov;
    if (A == 0)
        return T < 0 ? 256 : T > 0 ? 0 : 128;
    if (T <= -(A + B))
        cov = 0;
    else if (B > 0 && T <= -(A - B))
        cov = pb_fdiv(256 * (T + A + B) * (T + A + B), 8 * A * B);
    else if (T < A - B || B == 0)
        cov = 128 + pb_fdiv(128 * T, A);
    else if (T < A + B)
        cov = 256 - pb_fdiv(256 * (A + B - T) * (A + B - T), 8 * A * B);
    else
        cov = 256;
    cov = cov < 0 ? 0 : cov > 256 ? 256 : cov;
    return 256 - cov;
}

PB_FN I32 pb_abs(I32 x) { return x < 0 ? -x : x; }

/* picture pixel (i, r) as the window pixel centred at u, v shows it */
PB_FN U32 pb_q(I32 i, I32 r, I32 u, I32 v)
{
    I32 pw = I_(D_(PB_PW)), ph = I_(D_(PB_H) << (D_(PB_FIELD) != 0u ? 1u : 0u));
    U32 ms, f0, f1, X, R;
    i = i < 0 ? 0 : i >= pw ? pw - 1 : i;
    r = r < 0 ? 0 : r >= ph ? ph - 1 : r;
    X = U_(i);
    R = U_(r);
    if (D_(PB_FIELD) != 0u)
        return P_(X, R);
    pb_state(X, R);
    ms = S_(PB_S_MODE);
    f0 = S_(PB_S_L + PB_L_F);
    f1 = S_(PB_S_L + PB_L + PB_L_F);
    /* an interlaced picture is its woven rows */
    if (D_(PB_FIELD) != 0u)
        return P_(X, R);
    /* a draw list's layer is composed at the window's pixels */
    if (((ms >> 12) & 1u) == 0u && (((S_(PB_S_L + PB_L_F) >> 11) & 3u) == 3u || ((S_(PB_S_L + PB_L + PB_L_F) >> 11) & 3u) == 3u ||
                                    ((S_(PB_S_L + 2u * PB_L + PB_L_F) >> 11) & 3u) == 3u || ((S_(PB_S_L + 3u * PB_L + PB_L_F) >> 11) & 3u) == 3u))
        return pb_compose(X, R, 0x200u);
    /* Mode 7 is sampled at the window's pixels; all else is the picture's */
    if ((ms & 7u) != 7u || ((ms >> 12) & 1u) != 0u || ((f0 & 3u) == 0u && ((S_(PB_S_M7F) >> 4) & 1u) == 0u) ||
        ((f0 >> 4) & 31u) != 0u || ((f1 >> 4) & 31u) != 0u)
        return P_(X, R);
    {
        I32 oy = I_(D_(PB_OY)), a = pb_lo(S_(PB_S_M7)), b = pb_hi(S_(PB_S_M7)), c = pb_lo(S_(PB_S_M7 + 1)), d = pb_hi(S_(PB_S_M7 + 1));
        I32 su = I_(D_(PB_STEPU)), sv = I_(D_(PB_STEPV)), lineno = r - oy, e = v - 1024 * r;
        I32 ax = (pb_abs(a) * su) >> 10, bx = (pb_abs(b) * sv) >> 10, ay = (pb_abs(c) * su) >> 10, by = (pb_abs(d) * sv) >> 10;
        I32 w = ax + bx > ay + by ? ax + bx : ay + by, n, base, ix = 0, iy = 0, ex = 0, ey = 0, k;
        U32 last = 0x300u, col = 0u, cur = 0u, top = 0u, sr = 0u, sg = 0u, sb = 0u, sh, rnd;
        F32 fx = 0.0, fy = 0.0;
        if (w <= 256) {
            I32 qx, qy;
            pb_m7_pos(u, lineno, e);
            qx = pb_qx;
            qy = pb_qy;
            ix = qx >> 8;
            iy = qy >> 8;
            ex = pb_edge(qx & 255, ax, bx);
            ey = pb_edge(qy & 255, ay, by);
            if (ex == 256) {
                ix += 1;
                ex = 0;
            }
            if (ey == 256) {
                iy += 1;
                ey = 0;
            }
            fx = F_(ex) / 256.0;
            fy = F_(ey) / 256.0;
            n = (ex != 0 ? 2 : 1) * (ey != 0 ? 2 : 1);
            base = -1;
        } else {
            n = w <= 512 ? 4 : w <= 1024 ? 8 : 16;
            base = n == 4 ? 0 : n == 8 ? 4 : 12;
        }
        for (k = 0; k < n; k++) {
            I32 ci = 0, cj = 0;
            U32 pix;
            if (base < 0) {
                ci = ex != 0 ? (k & 1) : 0;
                cj = ex != 0 ? (k >> 1) : k;
                pix = pb_m7_texel(ix + ci, iy + cj);
            } else {
                pix = pb_m7_at(u + I_(D_(PB_DU + U_(base + k))), lineno, e + I_(D_(PB_DV + U_(base + k))));
            }
            if (pix != last) {
                col = pb_compose(X, R, pix);
                pb_state(X, R);
                last = pix;
            }
            if (base >= 0) {
                sb += col & 0xFFu;
                sg += (col >> 8) & 0xFFu;
                sr += (col >> 16) & 0xFFu;
            } else {
                cur = ci != 0 ? pb_mix(cur, col, fx) : col;
                if (ex == 0 || ci != 0)
                    top = cj != 0 ? pb_mix(top, cur, fy) : cur;
            }
        }
        if (base < 0)
            return top;
        sh = n == 4 ? 2u : n == 8 ? 3u : 4u;
        rnd = U_(n) / 2u;
        return 0xFF000000u | ((sr + rnd) >> sh) << 16 | ((sg + rnd) >> sh) << 8 | ((sb + rnd) >> sh);
    }
}

/* the overlays over colour c at window pixel (X, Y): bilinear, straight alpha */
PB_FN U32 pb_overlays(U32 c, U32 X, U32 Y)
{
    U32 n = D_(PB_OVCOUNT), i, at = D_(PB_OVERLAYS);
    F32 xc = F_(X) + 0.5, yc = F_(Y) + 0.5;
    for (i = 0u; i < n; i++, at += PB_OV) {
        U32 base = D_(at + PB_V_T), w = D_(at + PB_V_S) & 0xFFFFu, h = D_(at + PB_V_S) >> 16;
        F32 u = F_B(D_(at + PB_V_M)) * xc + F_B(D_(at + PB_V_M + 1u)) * yc + F_B(D_(at + PB_V_M + 2u));
        F32 v = F_B(D_(at + PB_V_M + 3u)) * xc + F_B(D_(at + PB_V_M + 4u)) * yc + F_B(D_(at + PB_V_M + 5u));
        F32 alpha = F_B(D_(at + PB_V_A)), a_sum = 0.0, r_sum = 0.0, g_sum = 0.0, b_sum = 0.0, fu, fv, iu, iv;
        I32 k;
        if (u < 0.0 || v < 0.0 || u >= F_(w) || v >= F_(h))
            continue;
        iu = floor(u - 0.5);
        iv = floor(v - 0.5);
        fu = u - 0.5 - iu;
        fv = v - 0.5 - iv;
        for (k = 0; k < 4; k++) {
            I32 tx = I_(iu) + (k & 1), ty = I_(iv) + (k >> 1);
            U32 t;
            F32 wt = ((k & 1) != 0 ? fu : 1.0 - fu) * ((k >> 1) != 0 ? fv : 1.0 - fv), a;
            tx = tx < 0 ? 0 : tx >= I_(w) ? I_(w) - 1 : tx;
            ty = ty < 0 ? 0 : ty >= I_(h) ? I_(h) - 1 : ty;
            t = T_(base + U_(ty) * w + U_(tx));
            a = F_(t >> 24) * wt;
            a_sum += a;
            r_sum += F_((t >> 16) & 255u) * a;
            g_sum += F_((t >> 8) & 255u) * a;
            b_sum += F_(t & 255u) * a;
        }
        if (a_sum > 0.0) {
            F32 oa = a_sum / 255.0 * alpha;
            F32 r = F_((c >> 16) & 255u) * (1.0 - oa) + r_sum / a_sum * oa;
            F32 g = F_((c >> 8) & 255u) * (1.0 - oa) + g_sum / a_sum * oa;
            F32 b = F_(c & 255u) * (1.0 - oa) + b_sum / a_sum * oa;
            c = 0xFF000000u | U_(I_(r + 0.5)) << 16 | U_(I_(g + 0.5)) << 8 | U_(I_(b + 0.5));
        }
    }
    return c;
}

/* c * fx / n's quotient and rounding in 32 bits: 1/1024 of a unit, to the nearest */
PB_FN I32 pb_scale1024(U32 a, U32 d)
{
    U32 q = a / d, r = a - q * d;
    return I_(q * 1024u + (r * 1024u + d / 2u) / d);
}

/* the window's pixel (X, Y): sharp bilinear over the picture, whole texels
   blended across the one window pixel an edge falls in */
PB_FN U32 pb_window(U32 X, U32 Y)
{
    U32 pw = D_(PB_PW), ph = D_(PB_H) << (D_(PB_FIELD) != 0u ? 1u : 0u), ww = D_(PB_WW), wh = D_(PB_WH), cpp = pw == D_(PB_W) ? 1u : 2u;
    F32 dx = F_(pw) / F_(ww), dy = F_(ph) / F_(wh);
    F32 px = (F_(X) + 0.5) * dx - 0.5, py = (F_(Y) + 0.5) * dy - 0.5;
    F32 ix = floor(px), iy = floor(py), fx = (px - ix - 0.5) / dx + 0.5, fy = (py - iy - 0.5) / dy + 0.5;
    I32 tx = I_(ix), ty = I_(iy), nr, nc, j, k;
    I32 u = pb_scale1024((2u * X + 1u) * pw, 2u * ww * cpp) - 512 - 1024 * I_(D_(PB_OX));
    I32 v = pb_scale1024((2u * Y + 1u) * ph, 2u * wh) - 512;
    U32 top = 0u;
    pb_wx = I_(X);
    pb_wy = I_(Y);
    fx = fx < 0.0 ? 0.0 : fx > 1.0 ? 1.0 : fx;
    fy = fy < 0.0 ? 0.0 : fy > 1.0 ? 1.0 : fy;
    nc = fx > 0.0 ? 2 : 1;
    nr = fy > 0.0 ? 2 : 1;
    for (j = 0; j < nr; j++) {
        U32 cur = 0u;
        for (k = 0; k < nc; k++) {
            U32 c = pb_q(tx + k, ty + j, u, v);
            cur = k != 0 ? pb_mix(cur, c, fx) : c;
        }
        top = j != 0 ? pb_mix(top, cur, fy) : cur;
    }
    return pb_overlays(top, X, Y);
}
