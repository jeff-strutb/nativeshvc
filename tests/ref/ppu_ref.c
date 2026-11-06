/*
 * ppu_ref.c: the frame drawn on the CPU from its record (ppu.h PpuFrame), and
 * from it the presentation at the window's resolution: the reference the GPU
 * compositors are held to, for the checks only (ppu_ref.h). It reads the
 * console through ppu.h alone and has no part in a game's run.
 */
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "ppu.h"
#include "rr_ref_mode7.h"
#include "ppu_ref.h"

/* The pictures, each as large as the screen asks for (nshvc_ext_screen),
   kept at the largest it has been: the frame drawn a line at a time
   (ppu_ref_line), the frame shown, and the interlaced picture, each field's
   lines on its own rows (the hardware's line y on row 2(y - 1) + field) over
   the rows of the frame before */
static uint32_t *picture, *shown, *woven;
static size_t picture_cap, shown_cap, woven_cap;
static uint32_t *field_pic;
static size_t field_cap;
static unsigned long shown_of, woven_built;     /* the frame shown[] holds; the one woven[] holds */

/* the screen being drawn: sw pixels a line, the hardware's column 0 at sl */
static int sw = PPU_WIDTH, sl = 0;
/* the row being drawn, from the hardware's line 1 (0): below 0 and past the
   hardware's last line on a taller screen */
static int draw_row;
/* the hardware's lines in the frame being drawn (rows past them are the
   taller screen's own) */
static int draw_lines_of = PPU_MAX_HEIGHT;
static unsigned mosaic_y;
static const PpuScreen *s;                      /* the state being drawn */
/* what the drawing reads */
static const uint16_t *d_vram, *d_cgram;
static const uint8_t *d_oam;
static int d_field;                             /* the field of the frame being drawn */
static const PpuFrame *d_frame;                 /* the frame, whose canvas its layer shows */
static uint8_t range_over, time_over;           /* (the drawing's own: the console's are ppu.c's) */

/* *p at least n elements of `size` bytes (*cap of them now), its contents kept */
static void *grow(void *p, size_t *cap, size_t n, size_t size)
{
    void *q;
    if (n <= *cap)
        return p;
    q = realloc(p, n * size);
    if (!q)
        nshvc_fatal("no memory for a picture of %lu pixels", (unsigned long)n);
    *cap = n;
    return q;
}

/* What one screen of one line holds at each pixel: a colour (15-bit BGR,
   or a full-colour canvas's, rr_mode7.h RR_RGB),
   its depth (higher in front; 0 the backdrop) and the layer it came from
   (0-3 BG1-4, 4 sprites with palettes 4-7, 5 the backdrop, 6 sprites with
   palettes 0-3, which colour maths never takes). */
typedef struct {
    uint32_t color;
    uint8_t z, layer;
    uint8_t ci;               /* the CGRAM entry its colour came from */
} Pixel;

static Pixel main_px[PPU_EXT_WIDTH], sub_px[PPU_EXT_WIDTH];

/* a layer's pixels for one line: colour, palette-0 (transparent) and
   priority */
typedef struct {
    uint32_t color;
    uint8_t opaque, prio, ci;
} LayerPx;

/* 512 of them on a hires line (modes 5 and 6): the main screen takes the
   odd ones, the sub screen the even */
static LayerPx lp[2 * PPU_EXT_WIDTH];

static uint16_t direct_color(uint8_t c, uint8_t pal)
{
    unsigned r = (c & 7u) << 2 | (pal & 1u) << 1;
    unsigned g = (c & 0x38u) >> 1 | (pal & 2u);
    unsigned b = (c & 0xC0u) >> 3 | (pal & 4u);
    return (uint16_t)(r | g << 5 | b << 10);
}

/* bits per pixel of each background in each mode (0: none) */
static const uint8_t mode_bpp[8][4] = {
    { 2, 2, 2, 2 }, { 4, 4, 2, 0 }, { 4, 4, 0, 0 }, { 8, 4, 0, 0 },
    { 8, 2, 0, 0 }, { 4, 2, 0, 0 }, { 4, 0, 0, 0 }, { 8, 0, 0, 0 },
};

/* background n's map entry for tile (tx, ty) from its map outside the
   console's picture (nshvc_ext_layer_outside), laid out as BGnSC's */
static uint16_t out_entry(int n, unsigned tx, unsigned ty)
{
    unsigned sc = d_frame->out_size[n], off = (ty & 31) << 5 | (tx & 31);
    if (sc >= 4)
        /* 128 wide: rows of 128 entries, 32 or 64 of them */
        return d_frame->out_map[n][(ty & (sc == 4 ? 31u : 63u)) << 7 | (tx & 127)];
    if ((tx & 32) && (sc & 1))
        off += 0x400;
    if ((ty & 32) && (sc & 2))
        off += (sc & 1) ? 0x800 : 0x400;
    return d_frame->out_map[n][off & 0xFFF];
}

static uint16_t map_entry(int n, unsigned tx, unsigned ty)
{
    uint8_t sc = s->bgsc[n];
    uint16_t base = (uint16_t)((sc & 0xFC) << 8);
    uint16_t off = (uint16_t)((ty & 31) << 5 | (tx & 31));
    if ((tx & 32) && (sc & 1))
        off += 0x400;
    if ((ty & 32) && (sc & 2))
        off += (sc & 1) ? 0x800 : 0x400;
    return d_vram[(base + off) & 0x7FFF];
}

/* one pixel's colour index (0 transparent) from a tile row: char address
   `ca` (words) of the 8x8 tile, row y, column x (already flipped) */
static uint8_t tile_pixel(uint16_t ca, unsigned y, unsigned x, unsigned bpp)
{
    unsigned bit = 7 - x, c = 0, p;
    for (p = 0; p < bpp; p += 2) {
        uint16_t w = d_vram[(ca + p * 4 + y) & 0x7FFF];
        c |= ((w >> bit) & 1u) << p;
        c |= ((w >> (bit + 8)) & 1u) << (p + 1);
    }
    return (uint8_t)c;
}

/* Each pixel's place in the canvas, as draw_bg found it while cv_capture
   is set (the presentation's records, ppu_mode7): its character's index
   in the canvas plus 1 (0: its tile is VRAM's), the pixel in the 8 x 8
   character (flipped), the flips (bit 0 H, bit 1 V), palette and
   priority. */
typedef struct {
    uint16_t ci1;
    uint8_t px, py, flip, pal, prio;
} CvAt;
static int cv_capture;
static CvAt cv_at[PPU_EXT_WIDTH];

/* the canvas's character for char address ca of layer n at bpp, or -1:
   d_frame's canvas covers the layer's characters whose data starts in its
   range (n of them at the layer's colour depth); a range from $8000 is in
   VRAM's whole space, the character's address its page's (page << 15) */
static long cv_char(int n, unsigned ca, unsigned bpp)
{
    unsigned wpc = bpp * 4u;
    unsigned long a = ca & 0x7FFFu;
    long rel;
    if (!d_frame || !d_frame->cv_on || d_frame->cv_screen || d_frame->cv_layer != n)
        return -1;
    if (d_frame->cv_addr >= 0x8000u)
        a |= (unsigned long)s->page[n] << 15;
    rel = (long)a - (long)d_frame->cv_addr;
    if (rel < 0 || rel % (long)wpc || (unsigned long)(rel / (long)wpc) >= d_frame->cv_count)
        return -1;
    return rel / (long)wpc;
}

/* the canvas's texel for a console pixel's centre: character ci, its pixel
   (px, py) in the character, the flips (bit 0 H, bit 1 V); the texel at the
   pixel's centre (k / 2 across and down, mirrored where flipped: rr_mode7.h
   rr_cv_q at the centre) */
static unsigned cv_centre(const PpuFrame *f, long ci, unsigned px, unsigned py, unsigned flip)
{
    unsigned k = (unsigned)f->cv_k, sx = k >> 1, sy = k >> 1;
    size_t i;
    if (flip & 1)
        sx = k - 1 - sx;
    if (flip & 2)
        sy = k - 1 - sy;
    i = (size_t)ci * 64u * k * k + ((size_t)py * k + sy) * 8u * k + (size_t)px * k + sx;
    return i < f->cv_len ? f->cv[i] : 0u;
}

/* x rounded down to a multiple of m (mosaic blocks counted from the
   hardware's column 0, either way) */
static int floor_to(int x, int m) { return x - (((x % m) + m) % m); }

/* A background's line into lp: sw pixels, or on a hires line (modes 5
   and 6) twice that, its tiles then 16 pixels wide whatever their size
   bit. */
static void draw_bg(int n, unsigned line, unsigned mode)
{
    unsigned bpp = mode_bpp[mode][n], big = (s->bgmode >> (4 + n)) & 1;
    int hires = mode == 5 || mode == 6;
    unsigned xshift = hires || big ? 4 : 3, yshift = big ? 4 : 3, x, count = (unsigned)(hires ? 2 * sw : sw);
    unsigned msize = (s->mosaic & (1u << n)) ? ((s->mosaic >> 4) + 1u) : 1u;
    int y = msize > 1 ? (int)mosaic_y : draw_row + 1;
    uint16_t nba = (uint16_t)(((n < 2 ? s->bgnba[0] : s->bgnba[1]) >> ((n & 1) * 4) & 0x0F) << 12);
    int opt = mode == 2 || mode == 4 || mode == 6;
    /* an interlaced hires line: two rows of the map to each line, the
       field's (the first with the layer's mosaic on) */
    if (hires && (s->setini & 0x01))
        y = 2 * y + (d_field && !(s->mosaic & (1u << n)));
    if (!hires && msize == 1 && d_frame && d_frame->cv_on && d_frame->cv_screen && d_frame->cv_layer == n) {
        /* a screen canvas: each pixel the texel under its centre, the
           layer's priority and palette (no map, no characters) */
        const PpuFrame *f = d_frame;
        int k = f->cv_k, R = draw_row + f->top;
        unsigned pal = (unsigned)f->cv_pal, prio = (unsigned)f->cv_prio, c;
        for (x = 0; x < count; x++) {
            long tx = (long)x * k + k / 2, ty = (long)R * k + k / 2;
            uint32_t t = 0;
            if (f->cv_fx && R >= 0) {
                /* (over the whole screen: the texel under the pixel's centre) */
                tx = (long)rr_cv_fit(x, 512u, (uint32_t)f->cv_fx, (uint32_t)f->width);
                ty = (long)rr_cv_fit((uint32_t)R, 512u, (uint32_t)f->cv_fy, (uint32_t)f->screen_height);
            }
            if (R >= 0 && tx < f->cv_w && ty < f->cv_h) {
                size_t at = (size_t)ty * (size_t)f->cv_w + (size_t)tx;
                if (f->cv_rgb)
                    memcpy(&t, f->cv + 4 * at, 4);
                else
                    t = f->cv[at];
            }
            if (cv_capture) {
                cv_at[x].ci1 = 1;
                cv_at[x].px = cv_at[x].py = cv_at[x].flip = 0;
                cv_at[x].pal = (uint8_t)pal;
                cv_at[x].prio = (uint8_t)prio;
            }
            if (t >= RR_RGB) {
                /* a full colour (nshvc_ext_canvas_screen32) */
                lp[x].opaque = 1;
                lp[x].prio = (uint8_t)prio;
                lp[x].color = RR_RGB | (t & 0xFFFFFFu);
                lp[x].ci = 0;
                continue;
            }
            c = t & ((1u << bpp) - 1u);
            lp[x].opaque = c != 0;
            lp[x].prio = (uint8_t)prio;
            if (!c)
                continue;
            if (bpp == 8)
                lp[x].color = (s->cgwsel & 1) ? direct_color((uint8_t)c, (uint8_t)pal) : d_cgram[c], lp[x].ci = (uint8_t)c;
            else if (bpp == 4)
                lp[x].ci = (uint8_t)(pal * 16 + c), lp[x].color = d_cgram[lp[x].ci];
            else
                lp[x].ci = (uint8_t)((mode == 0 ? n * 32 : 0) + pal * 4 + c), lp[x].color = d_cgram[lp[x].ci];
        }
        return;
    }
    for (x = 0; x < count; x++) {
        int hx = (int)x - (hires ? 2 * sl : sl);
        int sx = msize > 1 ? floor_to(hx, (int)(hires ? 2 * msize : msize)) : hx;
        unsigned hofs = s->bghofs[n], vofs = s->bgvofs[n];
        unsigned xx, yy, tx, ty, px, py, c;
        int opt_h = 0;   /* the column's coarse scroll from BG3's entry */
        uint16_t e, ch, ca;
        uint8_t pal;
        if (opt && n < 2) {
            /* the screen column the entry is for: 8 pixels, or on a hires
               line 16 half pixels, counted from the scroll's fine part (in
               half pixels: its low 3 bits doubled, then the low 3 of that) */
            int col = hires ? (sx + (int)((hofs << 1) & 7)) >> 4 : (sx + (int)(hofs & 7)) >> 3;
            if (col > 0) {
                /* BG3's map holds the offsets, read in BG3's own tiles: its
                   size bit sets their height, and their width but in mode 6,
                   where the columns are 8 pixels apart whatever the size
                   (measured against the reference) */
                unsigned big3 = s->bgmode >> 6 & 1;
                unsigned sx3 = big3 && !hires ? 4 : 3, sy3 = big3 ? 4 : 3;
                unsigned bx = ((unsigned)(col - 1) * 8 + (s->bghofs[2] & ~7u)) >> sx3;
                unsigned by = s->bgvofs[2] >> sy3;
                uint16_t hv = map_entry(2, bx, by), vv;
                if (mode == 4) {
                    if (hv & 0x8000) {
                        if (hv & (0x2000u << n))
                            vofs = hv & 0x3FF;
                    } else if (hv & (0x2000u << n)) {
                        hofs = (hofs & 7) | (hv & 0x3F8);
                        opt_h = 1;
                    }
                } else {
                    vv = map_entry(2, bx, (s->bgvofs[2] + 8) >> sy3);
                    if (hv & (0x2000u << n)) {
                        hofs = (hofs & 7) | (hv & 0x3F8);
                        opt_h = 1;
                    }
                    if (vv & (0x2000u << n))
                        vofs = vv & 0x3FF;
                }
            }
        }
        /* hires: the scroll in half pixels (the register's doubled); in a
           column an offset-per-tile entry moves, the fine part's low 3 bits
           in half pixels and the entry's coarse part as it stands
           (measured against the reference) */
        if (hires)
            xx = ((unsigned)sx + (opt_h ? ((hofs << 1) & 7) + (hofs & ~7u) : hofs << 1)) & 0x7FF;
        else
            xx = ((unsigned)sx + hofs) & 0x3FF;
        yy = ((unsigned)y + vofs) & 0x3FF;
        tx = xx >> xshift;
        ty = yy >> yshift;
        {
            /* outside the console's picture, the layer's own map there
               when it has one */
            int col = hires ? (int)x >> 1 : (int)x;
            if (d_frame && (d_frame->out_on >> n & 1) &&
                (col < sl || col >= sl + PPU_WIDTH || draw_row < 0 || draw_row >= draw_lines_of))
                e = out_entry(n, tx, ty);
            else
                e = map_entry(n, tx, ty);
        }
        px = xx & ((1u << xshift) - 1);
        py = yy & ((1u << yshift) - 1);
        if (e & 0x4000)
            px = (1u << xshift) - 1 - px;
        if (e & 0x8000)
            py = (1u << yshift) - 1 - py;
        ch = e & 0x3FF;
        if (px >= 8) ch += 1;
        if (py >= 8) ch += 16;
        ca = (uint16_t)(nba + (ch & 0x3FF) * (bpp * 4));
        pal = (uint8_t)((e >> 10) & 7);
        {
            /* a character the canvas covers (not on a hires line, not
               under mosaic): its texel at the pixel's centre */
            long ci = hires || msize > 1 ? -1 : cv_char(n, ca, bpp);
            unsigned flip = (e & 0x4000 ? 1u : 0u) | (e & 0x8000 ? 2u : 0u);
            if (ci >= 0) {
                c = cv_centre(d_frame, ci, px & 7, py & 7, flip) & ((1u << bpp) - 1u);
                if (cv_capture) {
                    cv_at[x].ci1 = (uint16_t)(ci + 1);
                    cv_at[x].px = (uint8_t)(px & 7);
                    cv_at[x].py = (uint8_t)(py & 7);
                    cv_at[x].flip = (uint8_t)flip;
                    cv_at[x].pal = pal;
                    cv_at[x].prio = (uint8_t)((e >> 13) & 1);
                }
            } else {
                c = tile_pixel(ca, py & 7, px & 7, bpp);
                if (cv_capture && x < PPU_EXT_WIDTH)
                    cv_at[x].ci1 = 0;
            }
        }
        lp[x].opaque = c != 0;
        lp[x].prio = (uint8_t)((e >> 13) & 1);
        if (!c)
            continue;
        if (bpp == 8)
            lp[x].color = (s->cgwsel & 1) ? direct_color((uint8_t)c, pal) : d_cgram[c], lp[x].ci = (uint8_t)c;
        else if (bpp == 4)
            lp[x].ci = (uint8_t)(pal * 16 + c), lp[x].color = d_cgram[lp[x].ci];
        else
            lp[x].ci = (uint8_t)((mode == 0 ? n * 32 : 0) + pal * 4 + c), lp[x].color = d_cgram[lp[x].ci];
    }
}

static int sext13(int v) { return (v & 0x1000) ? v | ~0x1FFF : v & 0x1FFF; }
static int clip10(int v) { return (v & 0x2000) ? (v | ~0x3FF) : (v & 0x3FF); }

/* Mode 7's texel at texel coordinates (px, py) (integers, 1024 a side
   inside the map) into o: the wrap modes past the map, the tile and its
   pixel, BG2's bit 7 as priority (EXTBG) */
static void m7_lookup(int n, int px, int py, LayerPx *o)
{
    int tx, ty;
    uint8_t tile, pix;
    if ((px | py) & ~0x3FF) {
        if ((s->m7sel & 0xC0) == 0x80) {
            o->opaque = 0;
            return;
        }
        if ((s->m7sel & 0xC0) == 0xC0)
            tile = 0;
        else
            tile = (uint8_t)d_vram[((py >> 3 & 127) << 7 | (px >> 3 & 127)) & 0x7FFF];
    } else {
        tile = (uint8_t)d_vram[((py >> 3) << 7 | (px >> 3)) & 0x7FFF];
    }
    tx = px & 7;
    ty = py & 7;
    pix = (uint8_t)(d_vram[(tile * 64 + ty * 8 + tx) & 0x7FFF] >> 8);
    if (n == 1) {
        o->prio = pix >> 7;
        pix &= 0x7F;
    } else {
        o->prio = 0;
    }
    o->opaque = pix != 0;
    if (pix)
        o->color = (s->cgwsel & 1) && n == 0 ? direct_color(pix, 0) : d_cgram[pix], o->ci = pix;
}

/* Mode 7's matrix applied to the line: the texel coordinates of screen
   column sx (the hardware's, flipped) are ((ox + a * sx) >> 8, (oy + c * sx)
   >> 8), in 1/256 texels; y the line (mosaic and flip applied) */
static void m7_origin(int y, int *ox, int *oy)
{
    int a = s->m7a, b = s->m7b, c = s->m7c, d = s->m7d;
    int cx = sext13(s->m7x), cy = sext13(s->m7y);
    int hofs = sext13(s->m7hofs), vofs = sext13(s->m7vofs);
    int x0 = clip10(hofs - cx), y0 = clip10(vofs - cy);
    *ox = ((a * x0) & ~63) + ((b * y) & ~63) + ((b * y0) & ~63) + (cx << 8);
    *oy = ((c * x0) & ~63) + ((d * y) & ~63) + ((d * y0) & ~63) + (cy << 8);
}

/* Mode 7: BG1 (or BG2, EXTBG, with bit 7 as priority) through the matrix */
static void draw_m7(int n, unsigned line)
{
    int a = s->m7a, c = s->m7c;
    unsigned msize = (s->mosaic & (1u << n)) ? ((s->mosaic >> 4) + 1u) : 1u;
    int y = msize > 1 ? (int)mosaic_y : draw_row + 1;
    int ox, oy;
    unsigned x;
    (void)line;
    if (s->m7sel & 2)
        y = 255 - y;
    m7_origin(y, &ox, &oy);
    for (x = 0; x < (unsigned)sw; x++) {
        int hx = (int)x - sl, sx = msize > 1 ? floor_to(hx, (int)msize) : hx;
        if (s->m7sel & 1)
            sx = 255 - sx;
        m7_lookup(n, (ox + a * sx) >> 8, (oy + c * sx) >> 8, &lp[x]);
    }
}

/* ---- sprites ---- */
static const uint8_t obj_w[8][2] = { { 8, 16 }, { 8, 32 }, { 8, 64 }, { 16, 32 }, { 16, 64 }, { 32, 64 }, { 16, 32 }, { 16, 32 } };
static const uint8_t obj_h[8][2] = { { 8, 16 }, { 8, 32 }, { 8, 64 }, { 16, 32 }, { 16, 64 }, { 32, 64 }, { 32, 64 }, { 32, 32 } };

typedef struct {
    uint16_t color;
    uint8_t opaque, prio, math, ci;
} ObjPx;

static ObjPx op[PPU_EXT_WIDTH];

/* a sprite's X: the table's 9 bits as signed, and 512 more for each of
   its X pages (nshvc_ext_sprite_page) */
static int obj_x(unsigned s, uint8_t hi)
{
    int x = d_oam[s * 4] | ((hi & 1) << 8);
    if (x >= 256)
        x -= 512;
    return x + 512 * (int8_t)d_oam[544 + s];
}

/* Whether sprite s (y its table's Y, h its height) covers row r of the
   screen (0 the hardware's line 1), and its row there. On Y page 0 the
   hardware's Y wraps in 256 rows, and a taller screen's rows past those
   256 show none of those sprites; a sprite on another Y page
   (nshvc_ext_sprite_page) is at its Y and 256 rows more for each page,
   without wrapping. */
static int obj_row(unsigned s, unsigned y, unsigned h, int r, unsigned *row)
{
    int d, page = (int8_t)d_oam[672 + s];
    if (page) {
        d = r - ((int)y + 256 * page);
        if (d < 0 || d >= (int)h)
            return 0;
        *row = (unsigned)d;
        return 1;
    }
    if (r >= 256 || (r < 0 && !ppu_ext_wrap_on()))
        return 0;
    d = (r - (int)y) & 0xFF;
    if (d >= (int)h)
        return 0;
    *row = (unsigned)d;
    return 1;
}

/* The console's line limits on the row being drawn: for each sprite,
   its tiles the console draws (-1 none). The first 32 sprites in range of
   its 256 columns (range over past them), then their tiles in those
   columns from the last in the list, up to 34 (time over past them). */
static unsigned obj_items, obj_tiles;      /* obj_eval's last: the sprites in range (at most 32), tiles kept */

static void obj_eval(int keep[128])
{
    unsigned first = s->oam_prio ? (s->oamaddr >> 1) & 0x7F : 0;
    unsigned i, n = 0, tiles = 0, k;
    unsigned list[32];
    int row = draw_row;
    unsigned sz = s->obsel >> 5;
    unsigned oi = (s->setini >> 1) & 1;
    for (i = 0; i < 128; i++)
        keep[i] = -1;
    for (i = 0; i < 128; i++) {
        unsigned sp = (first + i) & 0x7F;
        uint8_t hi = (uint8_t)(d_oam[0x200 + (sp >> 2)] >> ((sp & 3) * 2));
        unsigned big = (hi >> 1) & 1, h = obj_h[sz][big], w = obj_w[sz][big], r;
        int x = obj_x(sp, hi);
        /* X 256 is in range, whatever its width (its tiles off the screen) */
        int at256 = (d_oam[sp * 4] | (hi & 1) << 8) == 256;
        if (!obj_row(sp, d_oam[sp * 4 + 1], h >> oi, row, &r))
            continue;
        if (!at256 && (x <= -(int)w || x >= PPU_WIDTH))
            continue;
        if (n == 32) {
            range_over = 1;
            break;
        }
        list[n++] = sp;
    }
    obj_items = n;
    obj_tiles = 0;
    for (k = n; k-- > 0;) {
        unsigned sp = list[k];
        uint8_t hi = (uint8_t)(d_oam[0x200 + (sp >> 2)] >> ((sp & 3) * 2));
        unsigned big = (hi >> 1) & 1, w = obj_w[sz][big], c, cols = w >> 3, on = 0;
        int x = obj_x(sp, hi);
        /* at X 256 every tile is fetched */
        int at256 = (d_oam[sp * 4] | (hi & 1) << 8) == 256;
        for (c = 0; c < cols; c++) {
            int tx = x + (int)c * 8;
            if (at256 || (tx > -8 && tx < PPU_WIDTH))
                on++;
        }
        if (tiles + on > 34) {
            time_over = 1;
            keep[sp] = (int)(34 - tiles);
            obj_tiles = 34;
            break;
        }
        tiles += on;
        obj_tiles = tiles;
        keep[sp] = (int)on;
    }
}

static void draw_obj(unsigned line)
{
    unsigned first = s->oam_prio ? (s->oamaddr >> 1) & 0x7F : 0;
    unsigned i;
    int keep[128];                 /* by sprite: its tiles the console draws on the line (-1 none) */
    int row = draw_row;
    uint16_t base = (uint16_t)((s->obsel & 7) << 13);
    uint16_t gap = (uint16_t)((((s->obsel >> 3) & 3) + 1) << 12);
    unsigned sz = s->obsel >> 5;
    /* OBJ interlace (SETINI bit 1): each sprite half as many lines, every
       other row of it, the field's */
    unsigned oi = (s->setini >> 1) & 1;
    (void)line;
    memset(op, 0, sizeof op);
    obj_eval(keep);
    /* Drawn from the last in priority order, so the first ends on top:
       in the console's columns the tiles it keeps; in a larger screen's
       columns past them every sprite's, with no limit. */
    for (i = 128; i-- > 0;) {
        unsigned sp = (first + i) & 0x7F;
        uint8_t hi = (uint8_t)(d_oam[0x200 + (sp >> 2)] >> ((sp & 3) * 2));
        unsigned big = (hi >> 1) & 1, h = obj_h[sz][big], w = obj_w[sz][big];
        int x = obj_x(sp, hi);
        unsigned y = d_oam[sp * 4 + 1];
        uint16_t name = (uint16_t)(d_oam[sp * 4 + 2] | (d_oam[sp * 4 + 3] & 1) << 8);
        uint8_t attr = d_oam[sp * 4 + 3];
        unsigned pal = (attr >> 1) & 7, prio = (attr >> 4) & 3;
        unsigned r = 0, c, cols = w >> 3, drawn = 0;
        if (!obj_row(sp, y, h >> oi, row, &r))
            continue;
        r <<= oi;
        if (x + (int)w <= -sl || x >= sw - sl)
            continue;
        /* a vertical flip turns the sprite over in squares of its
           width: a sprite taller than it is wide (sizes 6 and 7's
           16x32 and 32x64) flips each square in place */
        if (attr & 0x80)
            r = h > w ? (r & ~(w - 1)) | (w - 1 - (r & (w - 1))) : h - 1 - r;
        if (oi)
            r = ((attr & 0x80) ? r - (unsigned)d_field : r + (unsigned)d_field) & 0xFF;
        for (c = 0; c < cols; c++) {
            unsigned col = (attr & 0x40) ? cols - 1 - c : c;
            int tx = x + (int)c * 8, kept;
            unsigned t, px;
            uint16_t ca;
            if (!(tx > -8 - sl && tx < sw - sl))
                continue;
            /* the console's tiles: in its columns, those it keeps (every
               one with the limits lifted) */
            kept = !ppu_ext_limits_on();
            if (tx > -8 && tx < PPU_WIDTH) {
                if (ppu_ext_limits_on())
                    kept = keep[sp] >= 0 && (int)drawn < keep[sp];
                drawn++;
            }
            t = (unsigned)(((name & 0xF0) + ((r >> 3) << 4)) & 0xF0) | ((name + col) & 0x0F);
            ca = (uint16_t)(base + t * 16 + ((name & 0x100) ? gap : 0));
            for (px = 0; px < 8; px++) {
                int hx = tx + (int)px, sx = hx + sl;
                unsigned ix = (attr & 0x40) ? 7 - px : px;
                uint8_t ci;
                if (sx < 0 || sx >= sw)
                    continue;
                if (hx >= 0 && hx < PPU_WIDTH && !kept)
                    continue;
                ci = tile_pixel(ca, r & 7, ix, 4);
                if (!ci)
                    continue;
                op[sx].opaque = 1;
                op[sx].prio = (uint8_t)prio;
                op[sx].ci = (uint8_t)(128 + pal * 16 + ci);
                op[sx].color = d_cgram[op[sx].ci];
                op[sx].math = pal >= 4;
            }
        }
    }
}

/* ---- windows ---- */
/* the screen's column console column c (0-256) maps to (nshvc_ext_window_map):
   c * scale / 65536 rounded down, plus the offset, the product in two
   parts so that no part passes 32 bits (compose_body.h cp_wmap's) */
static int wmap_col(int c)
{
    return c * (s->wmap_scale >> 16) + ((c * (s->wmap_scale & 0xFFFF)) >> 16) + s->wmap_offset;
}

/* whether column x (the hardware's, 0-255 its picture's) is inside window
   n (0, 1): at its screen columns (nshvc_ext_window_columns); mapped
   onto the screen (nshvc_ext_window_map), L to R covering the columns
   L maps to up to the one before R + 1's; else past the picture outside
   but that, its edges open (nshvc_ext_window_edges), a window from column
   0 reaches every column left of it and one to column 255 every column
   right */
static int win_in(int n, int x)
{
    int l = s->wh[2 * n], r = s->wh[2 * n + 1];
    if (s->wcols >> n & 1)
        return x + sl >= s->wcol[2 * n] && x + sl <= s->wcol[2 * n + 1];
    if (s->wmap)
        return x + sl >= wmap_col(l) && x + sl < wmap_col(r + 1);
    if (s->wedges && x < 0 && l == 0)
        x = 0;
    else if (s->wedges && x > 255 && r == 255)
        x = 255;
    return x >= l && x <= r;
}

/* whether column x (the hardware's) is inside the window mask */
static uint8_t win_mask(uint8_t sel, uint8_t logic, int x)
{
    int w1 = win_in(0, x), w2 = win_in(1, x);
    int e1 = sel & 2, e2 = sel & 8;
    if (!e1 && !e2)
        return 0;
    if (sel & 1) w1 = !w1;
    if (sel & 4) w2 = !w2;
    if (e1 && !e2) return (uint8_t)w1;
    if (e2 && !e1) return (uint8_t)w2;
    switch (logic & 3) {
    case 0: return (uint8_t)(w1 | w2);
    case 1: return (uint8_t)(w1 & w2);
    case 2: return (uint8_t)(w1 ^ w2);
    }
    return (uint8_t)!(w1 ^ w2);
}

static uint8_t layer_sel(int layer)
{
    switch (layer) {
    case 0: return s->w12sel & 0x0F;
    case 1: return s->w12sel >> 4;
    case 2: return s->w34sel & 0x0F;
    case 3: return s->w34sel >> 4;
    case 4: return s->wobjsel & 0x0F;
    }
    return s->wobjsel >> 4;
}

static uint8_t layer_logic(int layer)
{
    if (layer < 4)
        return (uint8_t)(s->wbglog >> (layer * 2));
    if (layer == 4)
        return s->wobjlog;
    return (uint8_t)(s->wobjlog >> 2);
}

/* whether layer n (4 the sprites) is kept out of the screen's column x
   on the row being drawn: kept to the console's picture
   (nshvc_ext_layer_clip), and x or the row past it */
static int clipped(int n, int x)
{
    return (s->clip >> n & 1) && (x < sl || x >= sl + PPU_WIDTH || draw_row < 0 || draw_row >= draw_lines_of);
}

/* put a layer's line onto the main and sub screens at its depths; a
   hires layer's 512 pixels give the main screen the odd ones and the sub
   screen the even (the windows, 256 wide, cover a pair) */
static void put_layer(int layer, const uint8_t z[2], int hires)
{
    unsigned x;
    int on_main = (s->tm >> layer) & 1, on_sub = (s->ts >> layer) & 1;
    uint8_t sel = layer_sel(layer), logic = layer_logic(layer);
    for (x = 0; x < (unsigned)sw; x++) {
        const LayerPx *m = hires ? &lp[2 * x + 1] : &lp[x], *b = hires ? &lp[2 * x] : &lp[x];
        int inside = ((s->tmw | s->tsw) >> layer & 1) ? win_mask(sel, logic, (int)x - sl) : 0;
        uint8_t zz;
        if (clipped(layer, (int)x))
            continue;
        if (on_main && m->opaque && (zz = z[m->prio]) && zz > main_px[x].z && !(inside && (s->tmw >> layer & 1))) {
            main_px[x].color = m->color;
            main_px[x].ci = m->ci;
            main_px[x].z = zz;
            main_px[x].layer = (uint8_t)layer;
        }
        if (on_sub && b->opaque && (zz = z[b->prio]) && zz > sub_px[x].z && !(inside && (s->tsw >> layer & 1))) {
            sub_px[x].color = b->color;
            sub_px[x].ci = b->ci;
            sub_px[x].z = zz;
            sub_px[x].layer = (uint8_t)layer;
        }
    }
}

static void put_obj(const uint8_t z[4])
{
    unsigned x;
    int on_main = (s->tm >> 4) & 1, on_sub = (s->ts >> 4) & 1;
    uint8_t sel = layer_sel(4), logic = layer_logic(4);
    for (x = 0; x < (unsigned)sw; x++) {
        uint8_t zz;
        int inside;
        if (!op[x].opaque || clipped(4, (int)x))
            continue;
        zz = z[op[x].prio];
        inside = ((s->tmw | s->tsw) >> 4 & 1) ? win_mask(sel, logic, (int)x - sl) : 0;
        if (on_main && zz > main_px[x].z && !(inside && (s->tmw >> 4 & 1))) {
            main_px[x].color = op[x].color;
            main_px[x].ci = op[x].ci;
            main_px[x].z = zz;
            main_px[x].layer = op[x].math ? 4 : 6;
        }
        if (on_sub && zz > sub_px[x].z && !(inside && (s->tsw >> 4 & 1))) {
            sub_px[x].color = op[x].color;
            sub_px[x].ci = op[x].ci;
            sub_px[x].z = zz;
            sub_px[x].layer = op[x].math ? 4 : 6;
        }
    }
}

static int region(unsigned mode, int inside)
{
    switch (mode & 3) {
    case 0: return 0;
    case 1: return !inside;
    case 2: return inside;
    }
    return 1;
}

/* One pixel's colour out of its screen and the other one: colour maths
   with the other screen (or the fixed colour), the colour window's clip
   and prevent regions, then brightness; 0xFFRRGGBB. */
static uint32_t finish(const Pixel *m, const Pixel *sp, int x, uint16_t fixed, uint8_t bright)
{
    int cw = win_mask(layer_sel(5), layer_logic(5), x);
    int clip = region(s->cgwsel >> 6, cw);
    int prevent = region(s->cgwsel >> 4, cw);
    unsigned bit = m->layer == 5 ? 5 : m->layer == 6 ? 8 : m->layer;
    uint32_t c = clip ? 0 : m->color;
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    int math = bit < 8 && (s->cgadsub >> bit & 1) && !prevent;
    uint32_t o = 0;
    int halve = 0;
    if (math) {
        halve = (s->cgadsub & 0x40) != 0 && !clip;
        if (s->cgwsel & 2) {
            if (sp->layer == 5) {
                o = fixed;
                halve = 0;
            } else {
                o = sp->color;
            }
        } else {
            o = fixed;
        }
    }
    if ((c | o) & RR_RGB)
        return rr_rgb_finish(c, o, math, (s->cgadsub & 0x80) != 0, halve, bright);
    if (math) {
        if (s->cgadsub & 0x80) {
            int rr = (int)r - (int)(o & 31), gg = (int)g - (int)((o >> 5) & 31), bb = (int)b - (int)((o >> 10) & 31);
            r = rr < 0 ? 0 : (unsigned)rr;
            g = gg < 0 ? 0 : (unsigned)gg;
            b = bb < 0 ? 0 : (unsigned)bb;
            if (halve) {
                r >>= 1;
                g >>= 1;
                b >>= 1;
            }
        } else {
            r += o & 31;
            g += (o >> 5) & 31;
            b += (o >> 10) & 31;
            if (halve) {
                r >>= 1;
                g >>= 1;
                b >>= 1;
            }
            if (r > 31) r = 31;
            if (g > 31) g = 31;
            if (b > 31) b = 31;
        }
    }
    /* brightness: each channel times INIDISP's level over 15, to the
       nearest (measured against the reference at every level and value) */
    if (bright != 15) {
        r = (r * bright + 7u) / 15u;
        g = (g * bright + 7u) / 15u;
        b = (b * bright + 7u) / 15u;
    }
    return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
}

/* One line: width 256, or 512 (a frame with a hires line: each pixel of a
   line that is not hires twice; a hires line's pairs the sub screen's
   pixel, then the main screen's). */
static void draw_line(unsigned line, uint32_t *out, int width)
{
    unsigned mode = s->bgmode & 7, x, n;
    uint8_t bright = s->inidisp & 0x0F;
    uint16_t fixed = (uint16_t)(s->fixed_r | s->fixed_g << 5 | s->fixed_b << 10);
    int hires = ppu_line_hires(s), bg_hires = mode == 5 || mode == 6;
    /* the depths of each layer's two priorities and of the sprites' four */
    static const uint8_t z0[4][2] = { { 8, 11 }, { 7, 10 }, { 2, 5 }, { 1, 4 } }, o0[4] = { 3, 6, 9, 12 };
    static const uint8_t z1[3][2] = { { 6, 9 }, { 5, 8 }, { 1, 3 } }, o1[4] = { 2, 4, 7, 10 };
    static const uint8_t z1p[3][2] = { { 6, 9 }, { 5, 8 }, { 1, 11 } };
    static const uint8_t z2[2][2] = { { 3, 7 }, { 1, 5 } }, o2[4] = { 2, 4, 6, 8 };
    static const uint8_t z7[2][2] = { { 3, 3 }, { 1, 5 } }, o7[4] = { 2, 4, 6, 7 };
    const uint16_t *vbase = d_vram;     /* the VRAM drawn from: each layer reads its page of it */
    if (s->inidisp & 0x80) {
        for (x = 0; x < (unsigned)width; x++)
            out[x] = 0xFF000000u;   /* black, opaque as every pixel drawn */
        return;
    }
    for (x = 0; x < (unsigned)sw; x++) {
        main_px[x].color = d_cgram[0];
        main_px[x].ci = 0;
        sub_px[x].ci = 0;
        main_px[x].z = 0;
        main_px[x].layer = 5;
        sub_px[x].color = fixed;
        sub_px[x].z = 0;
        sub_px[x].layer = 5;
    }
    if (mode == 7) {
        if ((s->tm | s->ts) & 1) {
            d_vram = vbase + ((unsigned)s->page[0] << 15);
            draw_m7(0, line);
            put_layer(0, z7[0], 0);
        }
        if ((s->setini & 0x40) && ((s->tm | s->ts) & 2)) {
            d_vram = vbase + ((unsigned)s->page[0] << 15);
            draw_m7(1, line);
            put_layer(1, z7[1], 0);
        }
    } else {
        for (n = 0; n < 4; n++) {
            const uint8_t *z;
            if (!mode_bpp[mode][n] || !((s->tm | s->ts) >> n & 1))
                continue;
            if (mode == 0)
                z = z0[n];
            else if (mode == 1)
                z = (s->bgmode & 0x08) ? z1p[n] : z1[n];
            else
                z = z2[n];
            d_vram = vbase + ((unsigned)s->page[n] << 15);
            draw_bg((int)n, line, mode);
            put_layer((int)n, z, bg_hires);
        }
    }
    if ((s->tm | s->ts) & 0x10) {
        d_vram = vbase + ((unsigned)s->page[4] << 15);
        draw_obj(line);
        put_obj(mode == 0 ? o0 : mode == 1 ? o1 : mode == 7 ? o7 : o2);
    }
    for (x = 0; x < (unsigned)sw; x++) {
        uint32_t right = finish(&main_px[x], &sub_px[x], (int)x - sl, fixed, bright);
        if (width == sw) {
            out[x] = right;
        } else if (!hires) {
            out[2 * x] = out[2 * x + 1] = right;
        } else {
            /* the sub screen's pixel shown in its own right: its backdrop
               is the main screen's (CGRAM 0), and it takes colour maths with
               the main screen's pixel */
            Pixel b = sub_px[x];
            if (b.layer == 5)
                b.color = d_cgram[0];
            out[2 * x] = finish(&b, &main_px[x], (int)x - sl, fixed, bright);
            out[2 * x + 1] = right;
        }
    }
    /* a hires line's first half pixel is black */
    /* (on a wider screen, its own first) */
    if (hires && width == 2 * sw)
        out[0] = 0xFF000000u;
    d_vram = vbase;
}

/* A recorded line: its segments each drawn with their state and palette,
   each kept from its first pixel to the next's. */
static void draw_recorded(const PpuFrame *f, unsigned line, uint32_t *out, int width)
{
    static uint32_t part[2 * PPU_EXT_WIDTH];
    unsigned k, n = f->segs[line], scale = width == 2 * sw ? 2 : 1;
    draw_row = (int)line - 1;
    s = &f->line[f->st[line]];
    d_cgram = f->palette[f->pl[line]];
    if (!n) {
        draw_line(line, out, width);
        return;
    }
    draw_line(line, out, width);
    for (k = 0; k < n; k++) {
        /* a segment's first pixel is the hardware's: on a wider screen the
           columns left of the hardware's are the first segment's, those
           right of it the last's */
        unsigned x0 = (f->seg_x[line][k] + (unsigned)sl) * scale;
        unsigned x1 = k + 1 < n ? (f->seg_x[line][k + 1] + (unsigned)sl) * scale : (unsigned)width;
        s = &f->seg_line[line][k];
        d_cgram = f->seg_palette[line][k];
        draw_line(line, part, width);
        memcpy(out + x0, part + x0, (x1 - x0) * sizeof *out);
    }
}

void ppu_draw_frame(const PpuFrame *f, uint32_t *out)
{
    int w = ppu_frame_width(f), top = f->top, r;
    sw = f->width;
    d_field = f->field;
    d_frame = f;
    sl = f->left;
    draw_lines_of = f->height;
    for (r = 0; r < f->screen_height; r++) {
        int line = r - top + 1;
        uint32_t *o = out + (size_t)r * (size_t)w;
        if (line >= 1 && line <= f->height) {
            mosaic_y = f->mosaic_row[line];
            d_vram = f->vram[f->version[line]];
            d_oam = f->oam[f->version[line]];
            draw_recorded(f, (unsigned)line, o, w);
            continue;
        }
        /* a row of a taller screen past the hardware's lines: the nearest
           line's state (its first segment), the row its own */
        {
            int edge = line < 1 ? 1 : f->height;
            s = &f->line[f->st[edge]];
            d_cgram = f->palette[f->pl[edge]];
            d_vram = f->vram[f->version[edge]];
            d_oam = f->oam[f->version[edge]];
            draw_row = line - 1;
            mosaic_y = (unsigned)line;
            draw_line((unsigned)edge, o, w);
        }
    }
    d_frame = NULL;
}

void ppu_ref_line(const PpuFrame *f, unsigned line, const uint16_t *live_vram, const uint8_t *live_table)
{
    sw = f->width;
    sl = f->left;
    draw_lines_of = PPU_MAX_HEIGHT;
    mosaic_y = f->mosaic_row[line];
    d_field = f->field;
    d_vram = live_vram;
    d_oam = live_table;
    d_frame = f;
    picture = grow(picture, &picture_cap, (size_t)2 * (size_t)sw * PPU_MAX_HEIGHT, sizeof *picture);
    draw_recorded(f, line, &picture[(size_t)(line - 1) * 2 * (size_t)sw], 2 * sw);
    d_frame = NULL;
}

const uint32_t *ppu_ref_lines(void) { return picture; }

/* the woven picture's pixels, from the finished frame and the one before
   (ppu_picture, when first asked for) */
static void weave_pixels(int woven_w, int woven_h, const int from[2])
{
    static uint32_t *other;
    static size_t other_cap;
    const PpuFrame *finished = ppu_frame(), *before = ppu_frame_before();
    int r, x, p;
    woven = grow(woven, &woven_cap, (size_t)woven_w * (size_t)woven_h, sizeof *woven);
    field_pic = grow(field_pic, &field_cap, (size_t)ppu_frame_width(finished) * (size_t)finished->screen_height,
                     sizeof *field_pic);
    ppu_draw_frame(finished, field_pic);
    if (from[0] || from[1]) {
        other = grow(other, &other_cap, (size_t)ppu_frame_width(before) * (size_t)before->screen_height, sizeof *other);
        ppu_draw_frame(before, other);
    }
    for (p = 0; p < 2; p++) {
        const PpuFrame *f = from[p] ? before : finished;
        const uint32_t *pic = from[p] ? other : field_pic;
        int w = ppu_frame_width(f);
        for (r = 0; 2 * r + p < woven_h; r++) {
            const uint32_t *src = pic + (size_t)r * (size_t)w;
            uint32_t *to = woven + (size_t)(2 * r + p) * (size_t)woven_w;
            if (w == woven_w)
                memcpy(to, src, (size_t)w * sizeof *to);
            else
                for (x = 0; x < w; x++)
                    to[2 * x] = to[2 * x + 1] = src[x];
        }
    }
    woven_built = ppu_frames();
}

const uint32_t *ppu_picture(int *width, int *height)
{
    const PpuFrame *f = ppu_frame();
    unsigned long done = ppu_frames();
    int ww, wh, from[2];
    if (done && shown_of != done) {
        shown = grow(shown, &shown_cap, (size_t)ppu_frame_width(f) * (size_t)f->screen_height, sizeof *shown);
        ppu_draw_frame(f, shown);
        shown_of = done;
    }
    if (done && ppu_woven(&ww, &wh, from)) {
        if (woven_built != done)
            weave_pixels(ww, wh, from);
        if (width)
            *width = ww;
        if (height)
            *height = wh;
        return woven;
    }
    if (!shown) {
        /* before the first frame: a zeroed 256 x 224 */
        shown = grow(shown, &shown_cap, 2 * PPU_WIDTH * PPU_MAX_HEIGHT, sizeof *shown);
        memset(shown, 0, shown_cap * sizeof *shown);
    }
    if (width)
        *width = done ? ppu_frame_width(f) : PPU_WIDTH;
    if (height)
        *height = done ? f->screen_height : 224;
    return shown;
}

/* a console reset: nothing drawn yet */
static void ref_reset(void)
{
    if (picture)
        memset(picture, 0, picture_cap * sizeof *picture);
    shown = grow(shown, &shown_cap, 2 * PPU_WIDTH * PPU_MAX_HEIGHT, sizeof *shown);
    memset(shown, 0, shown_cap * sizeof *shown);
    shown_of = woven_built = 0;
}

__attribute__((constructor)) static void ref_start(void) { ppu_reset_hook = ref_reset; }

/* (ppu_ref.h ppu_mode7, render/rr_mode7.h) Each picture pixel on a Mode 7 line
   is given everything of draw_line's rules that does not depend on where
   the sample falls in it: the sprites' pixel over the backdrop on each
   screen, which Mode 7 layer may show on each (its enable bits and
   windows), the colour window's regions; its line's or segment's state
   once in the segment table. */
static RrM7Px *m7_px;
static size_t m7_px_cap;
/* each picture row's flag, its states and their palettes, for as many
   rows as the screen has */
static uint8_t *m7_row;
static RrM7Seg *m7_seg;
static uint16_t (*m7_pal)[256];
static size_t m7_row_cap, m7_seg_cap, m7_pal_cap;
static uint16_t (*m7_vram)[0x8000];     /* PPU_VERSIONS * PPU_VRAM_PAGES of them, when first asked for */
static const uint16_t *m7_vram_of[PPU_VERSIONS * PPU_VRAM_PAGES];
/* each VRAM's plane (rr_mode7.h rr_m7_plane), made again when the VRAM
   differs from the frame before's at its index */
static uint8_t *m7_plane;
static int m7_planes;                 /* made: indexes 0 to m7_planes - 1 */
static uint32_t m7_plane_gen;

/* whether a line's state shows a Mode 7 layer */
static int pres_is_m7(const PpuScreen *st)
{
    return !(st->inidisp & 0x80) && (st->bgmode & 7) == 7 &&
           (((st->tm | st->ts) & 1) || ((st->setini & 0x40) && ((st->tm | st->ts) & 2)));
}

/* whether a line's state shows frame f's canvas: its layer a background
   of the mode, shown, not on a hires line nor under mosaic */
static int pres_is_cv(const PpuFrame *f, const PpuScreen *st)
{
    unsigned mode = st->bgmode & 7;
    int n = f->cv_layer;
    if (!f->cv_on || (st->inidisp & 0x80) || mode == 7 || !mode_bpp[mode][n] || !(((st->tm | st->ts) >> n) & 1))
        return 0;
    return !ppu_line_hires(st) && !((st->mosaic & (1u << n)) && (st->mosaic >> 4));
}

/* whether background n may show on a screen at column x (its enable bit
   and windows: put_layer's) */
static int cv_may(int n, int x, int sub)
{
    unsigned on = sub ? s->ts : s->tm, wn = sub ? s->tsw : s->tmw;
    int inside;
    if (!(on >> n & 1) || clipped(n, x))
        return 0;
    inside = ((s->tmw | s->tsw) >> n & 1) ? win_mask(layer_sel(n), layer_logic(n), x - sl) : 0;
    return !(inside && (wn >> n & 1));
}

/* background n's depth for priority bit prio in the state's mode
   (draw_line's tables) */
static unsigned cv_depth(int n, unsigned prio)
{
    static const uint8_t z0[4][2] = { { 8, 11 }, { 7, 10 }, { 2, 5 }, { 1, 4 } };
    static const uint8_t z1[3][2] = { { 6, 9 }, { 5, 8 }, { 1, 3 } };
    static const uint8_t z1p[3][2] = { { 6, 9 }, { 5, 8 }, { 1, 11 } };
    static const uint8_t z2[2][2] = { { 3, 7 }, { 1, 5 } };
    unsigned mode = s->bgmode & 7;
    if (mode == 0)
        return z0[n][prio];
    if (mode == 1)
        return (s->bgmode & 0x08) ? z1p[n][prio] : z1[n][prio];
    return z2[n][prio];
}

/* a screen's pixel with the sprites' over the backdrop (put_obj's rule) */
static void m7_base(const ObjPx *o, int x, int sub, uint16_t *c, uint8_t *z, uint8_t *l)
{
    static const uint8_t o7[4] = { 2, 4, 6, 7 };
    int inside;
    *c = sub ? (uint16_t)(s->fixed_r | s->fixed_g << 5 | s->fixed_b << 10) : d_cgram[0];
    *z = 0;
    *l = 5;
    if (!o->opaque || !(((sub ? s->ts : s->tm) >> 4) & 1) || clipped(4, x))
        return;
    inside = ((s->tmw | s->tsw) >> 4 & 1) ? win_mask(layer_sel(4), layer_logic(4), x - sl) : 0;
    if (inside && (((sub ? s->tsw : s->tmw) >> 4) & 1))
        return;
    *c = o->color;
    *z = o7[o->prio];
    *l = o->math ? 4 : 6;
}

/* whether Mode 7 layer n may show on a screen at column x */
static int m7_may(int n, int x, int sub)
{
    unsigned on = sub ? s->ts : s->tm, wn = sub ? s->tsw : s->tmw;
    int inside;
    if (!(on >> n & 1) || (n == 1 && !(s->setini & 0x40)) || clipped(n, x))
        return 0;
    inside = ((s->tmw | s->tsw) >> n & 1) ? win_mask(layer_sel(n), layer_logic(n), x - sl) : 0;
    return !(inside && (wn >> n & 1));
}

/* the line's or segment's state, its palette and VRAM, as an entry of the
   segment table */
static int m7_state(RrMode7 *m, const PpuScreen *st, const uint16_t *pal, const uint16_t *vr, int draw_y)
{
    RrM7Seg *g = &m7_seg[m->segs];
    int y = draw_y + 1, ox, oy, v, pi;
    const uint16_t *vp = vr + ((unsigned)st->page[0] << 15);
    if (st->m7sel & 2)
        y = 255 - y;
    s = st;
    m7_origin(y, &ox, &oy);
    for (v = 0; v < m->vrams && m7_vram_of[v] != vp; v++)
        ;
    if (v == m->vrams) {
        m7_vram_of[v] = vp;
        if (v >= m7_planes || memcmp(m7_vram[v], vp, sizeof m7_vram[v])) {
            if (v >= m7_planes) {
                uint8_t *np = (uint8_t *)realloc(m7_plane, (size_t)(v + 1) * 0x100000u);
                if (!np)
                    nshvc_fatal("no memory for the Mode 7 plane");
                m7_plane = np;
                m7_planes = v + 1;
            }
            memcpy(m7_vram[v], vp, sizeof m7_vram[v]);
            rr_m7_plane(m7_plane + (size_t)v * 0x100000u, m7_vram[v]);
            m7_plane_gen++;
        }
        m->vrams++;
    }
    pi = m->pals - 1;
    if (pi < 0 || memcmp(m7_pal[pi], pal, sizeof m7_pal[pi])) {
        pi = m->pals++;
        memcpy(m7_pal[pi], pal, sizeof m7_pal[pi]);
    }
    g->s[0] = ox;
    g->s[1] = oy;
    g->s[2] = (int32_t)((uint32_t)(uint16_t)st->m7a | (uint32_t)(uint16_t)st->m7b << 16);
    g->s[3] = (int32_t)((uint32_t)(uint16_t)st->m7c | (uint32_t)(uint16_t)st->m7d << 16);
    g->s[4] = (int32_t)((uint32_t)st->m7sel | (uint32_t)(st->cgwsel & 1) << 8 | (uint32_t)st->cgadsub << 16 |
                        (uint32_t)st->cgwsel << 24);
    g->s[5] = (int32_t)((uint32_t)(st->fixed_r | st->fixed_g << 5 | st->fixed_b << 10) |
                        (uint32_t)(st->inidisp & 0x0F) << 16 | (uint32_t)v << 24);
    g->s[6] = pi;
    g->s[7] = 0;
    return m->segs++;
}

/* a canvas line's state, its palette, as an entry of the segment table */
static int cv_state(RrMode7 *m, const PpuScreen *st, const uint16_t *pal, int n)
{
    RrM7Seg *g = &m7_seg[m->segs];
    unsigned mode = st->bgmode & 7;
    int pi = m->pals - 1;
    if (pi < 0 || memcmp(m7_pal[pi], pal, sizeof m7_pal[pi])) {
        pi = m->pals++;
        memcpy(m7_pal[pi], pal, sizeof m7_pal[pi]);
    }
    memset(g, 0, sizeof *g);
    g->s[4] = (int32_t)((uint32_t)(st->cgwsel & 1) << 8 | (uint32_t)st->cgadsub << 16 | (uint32_t)st->cgwsel << 24);
    g->s[5] = (int32_t)((uint32_t)(st->fixed_r | st->fixed_g << 5 | st->fixed_b << 10) | (uint32_t)(st->inidisp & 0x0F) << 16);
    g->s[6] = pi;
    g->s[7] = (int32_t)((uint32_t)mode_bpp[mode][n] | mode << 8);
    return m->segs++;
}

/* Mode 7 layer n's pixel byte at console column x under mosaic: its
   block's (draw_m7's sample) */
static unsigned m7_mosaic_texel(int n, int x, unsigned mosaic_row, const uint16_t *vp)
{
    unsigned msize = (s->mosaic >> 4) + 1u;
    int y = (int)mosaic_row, ox, oy, sx = floor_to(x - sl, (int)msize);
    (void)n;
    if (s->m7sel & 2)
        y = 255 - y;
    m7_origin(y, &ox, &oy);
    if (s->m7sel & 1)
        sx = 255 - sx;
    return rr_m7_texel(vp, s->m7sel, (ox + s->m7a * sx) >> 8, (oy + s->m7c * sx) >> 8);
}

int ppu_mode7(const PpuFrame *f, const uint32_t *pic, RrMode7 *m)
{
    int pw = ppu_frame_width(f), ph = f->screen_height, cpp = pw / f->width, top = f->top, r;
    size_t need = (size_t)pw * (size_t)ph;
    if (need > m7_px_cap) {
        free(m7_px);
        m7_px = (RrM7Px *)malloc(need * sizeof *m7_px);
        m7_px_cap = m7_px ? need : 0;
        if (!m7_px)
            return 0;
    }
    if (!m7_vram) {
        m7_vram = (uint16_t (*)[0x8000])calloc(PPU_VERSIONS * PPU_VRAM_PAGES, sizeof *m7_vram);
        if (!m7_vram)
            return 0;
    }
    m7_row = grow(m7_row, &m7_row_cap, (size_t)ph, sizeof *m7_row);
    m7_seg = grow(m7_seg, &m7_seg_cap, (size_t)ph * (PPU_SEGS + 1), sizeof *m7_seg);
    m7_pal = grow(m7_pal, &m7_pal_cap, (size_t)ph * (PPU_SEGS + 1), sizeof *m7_pal);
    memset(m, 0, sizeof *m);
    m->pic = pic;
    m->pw = pw;
    m->ph = ph;
    m->cpp = cpp;
    m->px = m7_px;
    m->row_m7 = m7_row;
    m->seg = m7_seg;
    m->pal = &m7_pal[0][0];
    m->vram = &m7_vram[0][0];
    sw = f->width;
    sl = f->left;
    m->left = sl;
    draw_lines_of = f->height;
    /* the frame its drawing reads (a canvas, the maps outside the picture) */
    d_frame = f;
    d_field = f->field;
    for (r = 0; r < ph; r++) {
        int line = r - top + 1, edge = line < 1 ? 1 : line > f->height ? f->height : line, k, n, x, i;
        const uint8_t *oamv = f->oam[f->version[edge]];
        const uint16_t *vr = f->vram[f->version[edge]];
        unsigned mosaic = edge == line ? f->mosaic_row[line] : (unsigned)line;
        RrM7Px *row = m7_px + (size_t)r * (size_t)pw;
        n = edge == line ? f->segs[line] : 0;
        m7_row[r] = 0;
        for (k = 0; k <= n; k++) {
            const PpuScreen *st = k ? &f->seg_line[line][k - 1] : &f->line[f->st[edge]];
            const uint16_t *pal = k ? f->seg_palette[line][k - 1] : f->palette[f->pl[edge]];
            int x0 = k ? f->seg_x[line][k - 1] + sl : 0, x1 = k < n ? f->seg_x[line][k] + sl : sw, g, hires;
            if (x0 < 0) x0 = 0;
            if (x1 > sw) x1 = sw;
            if (x0 >= x1)
                continue;
            if (pres_is_cv(f, st)) {
                /* the canvas's layer: the screens drawn without it, then its
                   characters' place in the canvas */
                static uint32_t scratch[2 * PPU_EXT_WIDTH];
                PpuScreen without = *st;
                int cn = f->cv_layer;
                unsigned mode = st->bgmode & 7;
                without.tm = (uint8_t)(without.tm & ~(1u << cn));
                without.ts = (uint8_t)(without.ts & ~(1u << cn));
                g = cv_state(m, st, pal, cn);
                s = &without;
                d_cgram = pal;
                d_oam = oamv;
                d_vram = vr;
                draw_row = line - 1;
                mosaic_y = mosaic;
                draw_line((unsigned)edge, scratch, sw);
                s = st;
                d_vram = vr + ((unsigned)s->page[cn] << 15);
                cv_capture = 1;
                draw_bg(cn, (unsigned)edge, mode);
                cv_capture = 0;
                for (x = x0; x < x1; x++) {
                    const CvAt *a = &cv_at[x];
                    unsigned flags = RR_CV;
                    int cw;
                    if (!a->ci1) {
                        for (i = x * cpp; i < (x + 1) * cpp; i++)
                            row[i].w0 = row[i].w1 = row[i].w3 = 0, row[i].w2 = RR_M7_NOT;
                        continue;
                    }
                    m7_row[r] = 1;
                    cw = win_mask(layer_sel(5), layer_logic(5), x - sl);
                    if (cv_may(cn, x, 0)) flags |= RR_M7_BG1_MAIN;
                    if (cv_may(cn, x, 1)) flags |= RR_M7_BG1_SUB;
                    if (region(s->cgwsel >> 6, cw)) flags |= RR_M7_CLIP;
                    if (region(s->cgwsel >> 4, cw)) flags |= RR_M7_PREVENT;
                    for (i = x * cpp; i < (x + 1) * cpp; i++) {
                        RrM7Px *q = &row[i];
                        q->w0 = (uint32_t)main_px[x].color | (uint32_t)sub_px[x].color << 16;
                        q->w1 = (uint32_t)main_px[x].z | (uint32_t)sub_px[x].z << 8 | (uint32_t)main_px[x].layer << 16 |
                                (uint32_t)sub_px[x].layer << 24;
                        q->w2 = flags | (uint32_t)g << 16;
                        q->w3 = (uint32_t)(a->ci1 - 1) | (uint32_t)a->px << 14 | (uint32_t)a->py << 17 |
                                (uint32_t)(a->flip & 3) << 20 | (uint32_t)a->pal << 22 | cv_depth(cn, a->prio) << 25 |
                                (uint32_t)cn << 29 | (f->cv_screen ? 0x80000000u : 0u);
                    }
                }
                continue;
            }
            if (!pres_is_m7(st)) {
                for (i = x0 * cpp; i < x1 * cpp; i++)
                    row[i].w0 = row[i].w1 = row[i].w3 = 0, row[i].w2 = RR_M7_NOT;
                continue;
            }
            m7_row[r] = 1;
            g = m7_state(m, st, pal, vr, line - 1);
            s = st;
            d_cgram = pal;
            d_oam = oamv;
            draw_row = line - 1;
            if ((s->tm | s->ts) & 0x10) {
                d_vram = vr + ((unsigned)s->page[4] << 15);
                draw_obj((unsigned)edge);
            } else {
                memset(op, 0, sizeof op);
            }
            hires = ppu_line_hires(s);
            for (x = x0; x < x1; x++) {
                uint16_t mc, sc;
                uint8_t mz, sz, ml, sl5;
                unsigned flags = 0, fix = 0;
                int cw = win_mask(layer_sel(5), layer_logic(5), x - sl);
                const uint16_t *vp = vr + ((unsigned)s->page[0] << 15);
                m7_base(&op[x], x, 0, &mc, &mz, &ml);
                m7_base(&op[x], x, 1, &sc, &sz, &sl5);
                if (m7_may(0, x, 0)) flags |= RR_M7_BG1_MAIN;
                if (m7_may(0, x, 1)) flags |= RR_M7_BG1_SUB;
                if (m7_may(1, x, 0)) flags |= RR_M7_BG2_MAIN;
                if (m7_may(1, x, 1)) flags |= RR_M7_BG2_SUB;
                if (region(s->cgwsel >> 6, cw)) flags |= RR_M7_CLIP;
                if (region(s->cgwsel >> 4, cw)) flags |= RR_M7_PREVENT;
                if ((flags & (RR_M7_BG1_MAIN | RR_M7_BG1_SUB)) && (s->mosaic & 1) && (s->mosaic >> 4)) {
                    flags |= RR_M7_BG1_FIX;
                    fix |= m7_mosaic_texel(0, x, mosaic, vp);
                }
                if ((flags & (RR_M7_BG2_MAIN | RR_M7_BG2_SUB)) && (s->mosaic & 2) && (s->mosaic >> 4)) {
                    flags |= RR_M7_BG2_FIX;
                    fix |= m7_mosaic_texel(1, x, mosaic, vp) << 8;
                }
                for (i = x * cpp; i < (x + 1) * cpp; i++) {
                    RrM7Px *q = &row[i];
                    unsigned fl = flags;
                    if (cpp == 2 && hires && i == 0)
                        fl |= RR_M7_BLACK;
                    else if (cpp == 2 && hires && !(i & 1))
                        fl |= RR_M7_SWAP;
                    q->w0 = (uint32_t)mc | (uint32_t)sc << 16;
                    q->w1 = (uint32_t)mz | (uint32_t)sz << 8 | (uint32_t)ml << 16 | (uint32_t)sl5 << 24;
                    q->w2 = fl | (uint32_t)g << 16;
                    q->w3 = fix;
                }
            }
        }
    }
    m->plane = m7_plane;
    m->plane_gen = m7_plane_gen;
    m->cv = f->cv_on ? f->cv : NULL;
    m->cv_len = f->cv_on ? f->cv_len : 0;
    m->cv_k = f->cv_on ? f->cv_k : 1;
    m->cv_w = f->cv_on && f->cv_screen ? f->cv_w : 0;
    m->cv_h = f->cv_on && f->cv_screen ? f->cv_h : 0;
    m->cv_rgb = f->cv_on && f->cv_screen && f->cv_rgb;
    m->cv_fx = f->cv_on && f->cv_screen ? f->cv_fx : 0;
    m->cv_fy = f->cv_on && f->cv_screen ? f->cv_fy : 0;
    d_frame = NULL;
    return 1;
}

void ppu_present(const PpuFrame *f, const uint32_t *pic, uint32_t *out, int ow, int oh)
{
    static int32_t *cols, *rows;
    static int ccap, rcap;
    RrMode7 m;
    RrM7Samples smp;
    int d[4] = { 0, 0, ow, oh };
    if (ow <= 0 || oh <= 0)
        return;
    if (ow > ccap) {
        free(cols);
        cols = (int32_t *)malloc((size_t)ow * 3 * 4);
        ccap = cols ? ow : 0;
    }
    if (oh > rcap) {
        free(rows);
        rows = (int32_t *)malloc((size_t)oh * 4 * 4);
        rcap = rows ? oh : 0;
    }
    if (!cols || !rows || !ppu_mode7(f, pic, &m))
        return;
    rr_m7_tables(&m, ow, oh, &smp, cols, rows);
    rr_ref_mode7(out, ow, oh, &m, d, &smp);
}
