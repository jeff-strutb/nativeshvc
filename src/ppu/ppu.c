/*
 * ppu.c: the picture unit: the game's screen state (its modes, layers,
 * scroll, windows, colour maths, brightness), its tiles and maps, palette
 * and sprite table, drawn a line at a time.
 *
 * The game sets the state through the registers ($2100-$2133, ppu_write)
 * and reads back through $2134-$213F (ppu_read). ppu_render_line records
 * one line's state at that moment into the frame's record (the console's
 * clock calls it as the line's horizontal blank comes, after the line's
 * changes and before its HDMA); the GPU compositor draws the frame from
 * the record. The drawing here is the reference the compositor is checked
 * against: a frame drawn from its record (ppu_draw_frame, ppu_picture),
 * and under NSHVC_PPUCHECK each line drawn as it is recorded, against
 * which the record itself is checked:
 *
 *   backgrounds  modes 0-7: 2, 4 and 8 bits a pixel, 8x8 and 16x16 tiles,
 *                maps of 32 or 64 tiles each way, mosaic, offset-per-tile
 *                (modes 2, 4, 6), direct colour, Mode 7 with its wrap modes
 *                and EXTBG; modes 5 and 6 and pseudo-hires 512 wide
 *   sprites      the 32-sprite and 34-tile limits of a line (and their
 *                flags), every size table, the name gap, priority rotation
 *   windows      two per layer with their logic, on main and sub screens
 *   colour maths add or subtract the sub screen or the fixed colour, halved,
 *                the colour window's clip and prevent regions
 *   brightness   and forced blank; overscan (239 lines)
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "ppu.h"
#include "rr_mode7.h"
#include "snes.h"

static uint16_t vram[PPU_VRAM_PAGES * 0x8000];
static int vram_pages = 1;            /* nshvc_ext_vram */
static unsigned port_page;            /* the page the data port reads and writes */
static int direct_page = -1;          /* nshvc_ext_vram_direct: the data port's writes land here at once */
static int hidden_dirty;              /* a page no layer showed changed since the last copy */
static uint16_t cgram[256];
static uint8_t oam[544];
static uint32_t picture[2 * PPU_EXT_WIDTH * PPU_MAX_HEIGHT];    /* drawn a line at a time (NSHVC_PPUCHECK) */
static uint32_t shown[2 * PPU_EXT_WIDTH * PPU_EXT_HEIGHT];
/* the interlaced picture: each field's lines on its own rows (the hardware's
   line y on row 2(y - 1) + field), over the rows of the frame before */
static uint32_t woven[2 * PPU_EXT_WIDTH * 2 * PPU_EXT_HEIGHT];
static int woven_w, woven_h;
static unsigned long woven_of;                  /* the frame woven last */
/* the extensions (nshvc.h): the screen's width asked for (from the next
   frame), the sprites' extended X bits */
static int ext_width = PPU_WIDTH, ext_height = 224;
static int ext_limits = 1;           /* the line's sprite and tile limits (nshvc_ext_sprite_limits) */
static uint8_t ext_x[16], ext_y[16];
/* the sprite table as it stands with the extended X and Y bits after it, as
   a frame's copies hold it (what the drawing reads) */
static uint8_t oam_live[576];
/* the screen being drawn: sw pixels a line (256, or wider with
   nshvc_ext_screen), the hardware's column 0 at sl */
static int sw = PPU_WIDTH, sl = 0;
/* the row being drawn, from the hardware's line 1 (0): below 0 and past
   the hardware's last line on a taller screen */
static int draw_row;
static unsigned long frames_done, shown_of;     /* frames finished; the one shown[] holds */
static int draw_lines = -1;                     /* lines drawn as recorded (NSHVC_PPUCHECK) */

static PpuScreen cur = { .inidisp = 0x80 };   /* the state as the game sets it */
static const PpuScreen *s = &cur;             /* the state being drawn */
/* the frames' records: one being made, the last finished */
static PpuFrame frames[2];
static PpuFrame *making = &frames[0], *finished = &frames[1];
static int dirty = 1;                           /* the tiles or sprites changed since the last copy */
unsigned long ppu_version_overflow;             /* frames that needed more copies than PPU_VERSIONS */
/* what the drawing reads: the live memory, or a record's */
static const uint16_t *d_vram = vram, *d_cgram = cgram;
static const uint8_t *d_oam = oam;
static int d_field;                             /* the field of the frame being drawn */
/* the frame being drawn from its record, whose canvas its layer shows (the
   console's own drawing, the live CGRAM target's: none) */
static const PpuFrame *d_frame;

/* the canvas (nshvc_ext_canvas_chars): a layer's characters in a range of
   VRAM drawn from the game's texels, k a console pixel along each axis */
static struct {
    int on, layer, k;
    unsigned addr, count;
    uint8_t *data;
    size_t len, cap;
    uint32_t gen;
} cv_now;
static uint8_t *cv_kept[2];           /* each frame record's copy of it */
static size_t cv_kept_cap[2];
static int cv_scale = 1;              /* the window's texels a console pixel (ppu_set_canvas_scale) */

/* the data ports: where the game's writes of tiles, palette and sprites go */
static uint8_t bgofs_latch, bghofs_latch;
static uint8_t m7_latch;
static uint8_t m7b_last;
static uint8_t vmain;
static uint16_t vmaddr, vram_prefetch;
static uint8_t cgaddr, cg_flip, cg_latch;
static uint16_t oam_at;
static uint8_t oam_latch;
static uint8_t range_over, time_over;
static unsigned mosaic_y;
/* what the read side keeps: the two PPUs' open bus, the counters' latch */
static uint8_t ppu1_mdr, ppu2_mdr;
static uint16_t h_latch, v_latch;
static uint8_t h_flip, v_flip, latch_flag, field;
/* The changes the game made to the line being drawn, in the order made:
   from which pixel each shows, and what it sets (the state just after the
   write, of which the register's own fields are taken; or a colour). The
   line's state before the first of them is its base. */
#define PPU_EVENTS 64
typedef struct {
    int16_t x;
    uint8_t reg;            /* $2100 + reg; 0x22 a colour */
    uint8_t index;          /* the colour's */
    uint16_t color;
    PpuScreen after;
} PpuEvent;
static unsigned live_vline, live_n;
static PpuEvent live_ev[PPU_EVENTS];
static PpuScreen live_base;
static uint16_t live_base_pal[256];
unsigned long ppu_segment_overflow;

/* the last finished frame, drawn from its record when first asked for */
const uint32_t *ppu_picture(int *width, int *height)
{
    if (frames_done && shown_of != frames_done) {
        ppu_draw_frame(finished, shown);
        shown_of = frames_done;
    }
    if (frames_done && finished->interlace && woven_of == frames_done) {
        if (width)
            *width = woven_w;
        if (height)
            *height = woven_h;
        return woven;
    }
    if (width)
        *width = frames_done ? ppu_frame_width(finished) : PPU_WIDTH;
    if (height)
        *height = frames_done ? finished->screen_height : 224;
    return shown;
}
const PpuFrame *ppu_frame(void) { return finished; }
int ppu_forced_blank(void) { return (cur.inidisp & 0x80) != 0; }
int ppu_overscan(void) { return (cur.setini & 0x04) != 0; }
int ppu_interlace(void) { return cur.setini & 0x01; }
int ppu_field(void) { return field; }
unsigned long ppu_frames(void) { return frames_done; }
const uint16_t *ppu_vram(void) { return vram; }
const uint16_t *ppu_cgram(void) { return cgram; }
const uint8_t *ppu_oam(void) { return oam; }

int ppu_line_hires(const PpuScreen *l)
{
    unsigned mode = l->bgmode & 7;
    return mode == 5 || mode == 6 || (l->setini & 0x08);
}

void ppu_reset(void)
{
    memset(vram, 0, sizeof vram);
    vram_pages = 1;
    port_page = 0;
    direct_page = -1;
    hidden_dirty = 0;
    memset(cgram, 0, sizeof cgram);
    memset(oam, 0, sizeof oam);
    memset(picture, 0, sizeof picture);
    memset(shown, 0, sizeof shown);
    memset(frames, 0, sizeof frames);
    frames[0].height = frames[1].height = 224;
    frames[0].width = frames[1].width = PPU_WIDTH;
    frames[0].screen_height = frames[1].screen_height = 224;
    ext_width = sw = PPU_WIDTH;
    ext_height = 224;
    sl = 0;
    memset(ext_x, 0, sizeof ext_x);
    memset(ext_y, 0, sizeof ext_y);
    ext_limits = 1;
    making = &frames[0];
    finished = &frames[1];
    frames_done = shown_of = woven_of = 0;
    woven_w = woven_h = 0;
    dirty = 1;
    memset(&cur, 0, sizeof cur);
    cur.inidisp = 0x80;
    s = &cur;
    bgofs_latch = bghofs_latch = m7_latch = m7b_last = 0;
    vmain = 0;
    vmaddr = vram_prefetch = 0;
    cgaddr = cg_flip = cg_latch = 0;
    oam_at = 0;
    oam_latch = 0;
    range_over = time_over = 0;
    mosaic_y = 1;
    /* both PPUs' open bus as the console powers on (measured) */
    ppu1_mdr = ppu2_mdr = 0xFF;
    h_latch = v_latch = 0;
    h_flip = v_flip = latch_flag = field = 0;
    live_vline = live_n = 0;
    cv_now.on = 0;
    cv_now.gen++;
}

/* ---- registers --------------------------------------------------------------------- */
static const uint8_t *live_oam(void)
{
    memcpy(oam_live, oam, sizeof oam);
    memcpy(oam_live + 544, ext_x, sizeof ext_x);
    memcpy(oam_live + 560, ext_y, sizeof ext_y);
    return oam_live;
}

static uint16_t vram_remap(uint16_t a)
{
    switch ((vmain >> 2) & 3) {
    case 1: return (uint16_t)((a & 0xFF00) | (a & 0x00E0) >> 5 | (a & 0x001F) << 3);
    case 2: return (uint16_t)((a & 0xFE00) | (a & 0x01C0) >> 6 | (a & 0x003F) << 3);
    case 3: return (uint16_t)((a & 0xFC00) | (a & 0x0380) >> 7 | (a & 0x007F) << 3);
    }
    return a;
}

static void vram_step(void)
{
    static const uint16_t inc[4] = { 1, 32, 128, 128 };
    vmaddr = (uint16_t)(vmaddr + inc[vmain & 3]);
}

/* A change to page p outside the console's bus (nshvc_ext_vram_direct,
   nshvc_ext_vram_copy): shown from the next line drawn when a layer reads
   p; else kept back until a layer is switched to it, so changes to a page
   no layer shows take none of the frame's copies */
static void page_changed(unsigned p)
{
    int l;
    for (l = 0; l < 5; l++)
        if (cur.page[l] == p) {
            dirty = 1;
            return;
        }
    hidden_dirty = 1;
}

/* VRAM can be written only while nothing is drawn from it: in forced
   blank, or in the vertical blank (the write is lost otherwise; the
   address still moves on) */
static int vram_open(void)
{
    return (cur.inidisp & 0x80) || snes_vcounter() >= snes_vdisp();
}

/* Where a write made while a line is drawn shows (measured against the
   reference, in master clocks from the line's start c): what the output
   stage takes (brightness, the screens, windows, colour maths, colours)
   from pixel (c - 52) / 4; what the backgrounds' tile fetches take (their
   maps, characters and scroll) from the first tile whose map entry is
   fetched at or after it (bg_fetch_slot), the scroll's fine part as the
   line began; Mode 7's matrix, scroll, flips and wrap as the output stage, as
   it works each pixel out as the pixel is made. (The PPU makes pixel x at
   54 + 4x clocks on from the line's start, a write landing at the end of
   the CPU's cycle; a write at c shows from the first pixel made at or
   after it.) */
#define PPU_OUT_CLOCK0 52
/* Mode 7's own event for a write to $210D/$210E (they are BG1's scroll
   and Mode 7's): its scroll from the pixel, as its matrix */
#define PPU_M7_SCROLL 0x80
#define PPU_HBLANK_CLOCK 1096

static int fetch_reg(uint8_t reg) { return reg >= 0x07 && reg <= 0x14; }

/* the background a fetch register is for (its fine scroll moves the tiles'
   edges) */
static int fetch_bg(uint8_t reg)
{
    if (reg >= 0x07 && reg <= 0x0A)
        return reg - 0x07;
    if (reg == 0x0B)
        return 0;
    if (reg == 0x0C)
        return 2;
    return (reg - 0x0D) >> 1;
}

/* The tile fetches: a tile each 32 clocks (the counters' H 32g for the
   tile 8g on), each layer's map entry, which takes all its registers, in a
   slot of 4 clocks of those 32 by the mode (in modes 2, 4 and 6 BG3's are
   the offset-per-tile entries, slot 2, which the next tile of BG1 and BG2
   takes); the counters run 4 clocks ahead of the line's clock here. */
static const uint8_t bg_fetch_slot[8][4] = {
    { 3, 2, 1, 0 }, { 2, 1, 0, 0 }, { 1, 0, 2, 0 }, { 1, 0, 0, 0 },
    { 1, 0, 2, 0 }, { 1, 0, 0, 0 }, { 1, 0, 2, 0 }, { 0, 0, 0, 0 },
};

/* The pixel of the line being drawn a write now shows from: <= 0 all of
   it, >= 256 none of it (the next line). */
static int write_pixel(uint8_t reg, uint32_t c, const PpuScreen *base)
{
    if (fetch_reg(reg)) {
        unsigned mode = base->bgmode & 7, bg = (unsigned)fetch_bg(reg);
        int h = (int)c + 4 - 4 * bg_fetch_slot[mode][bg];
        /* the first tile whose fetch comes at or after the write */
        int t = h <= 0 ? 0 : (h + 31) / 32;
        if (bg == 2 && (mode == 2 || mode == 4 || mode == 6))
            t++;
        if (t == 0)
            return 0;
        return t * 8 - (base->bghofs[bg] & 7);
    }
    return c < PPU_OUT_CLOCK0 ? 0 : (int)((c - PPU_OUT_CLOCK0) / 4);
}

/* Before a write that changes what is drawn, made while a line is being
   drawn: the line's base kept, if this is the first. (The data ports'
   addresses and OAM change nothing drawn now; CGRAM's data port is
   recorded with the colour, after the write.) */
static int write_in_line(uint8_t reg, uint32_t *c)
{
    unsigned v;
    if (reg == 0x02 || reg == 0x03 || reg == 0x04 || (reg >= 0x15 && reg <= 0x19) || reg == 0x21)
        return 0;
    v = snes_vcounter();
    if (v < 1 || v >= snes_vblank_line() || v > PPU_MAX_HEIGHT)
        return 0;
    *c = snes_line_clock();
    if (*c >= PPU_HBLANK_CLOCK)
        return 0;
    if (live_vline != v) {
        live_vline = v;
        live_n = 0;
    }
    if (!live_n) {
        live_base = cur;
        memcpy(live_base_pal, cgram, sizeof cgram);
    }
    return 1;
}

static void event_after_write(uint8_t reg, uint32_t c, uint8_t index, uint16_t color)
{
    int x = write_pixel(reg, c, &live_base);
    PpuEvent *e;
    if (x >= PPU_WIDTH)
        return;
    if (live_n == PPU_EVENTS) {
        ppu_segment_overflow++;
        return;
    }
    e = &live_ev[live_n++];
    e->x = (int16_t)(x < 0 ? 0 : x);
    e->reg = reg;
    e->index = index;
    e->color = color;
    e->after = cur;
}

static void draw_line(unsigned line, uint32_t *out, int width);

static unsigned cgram_target(unsigned ahead);

static void ppu_write_reg(uint8_t reg, uint8_t v);
static void take_version(void);

void ppu_write(uint8_t reg, uint8_t v)
{
    uint32_t c;
    int in_line = write_in_line(reg, &c);
    uint8_t index = cgaddr, flip = cg_flip;
    ppu_write_reg(reg, v);
    if (!in_line)
        return;
    if (reg == 0x22) {
        if (flip)
            event_after_write(reg, c, index, cgram[index]);
        return;
    }
    event_after_write(reg, c, 0, 0);
    if (reg == 0x0D || reg == 0x0E)
        event_after_write(reg | PPU_M7_SCROLL, c, 0, 0);
}

static void ppu_write_reg(uint8_t reg, uint8_t v)
{
    switch (reg) {
    case 0x00:
        /* the forced blank lifted while the frame's lines are drawn: what
           the blank changed shows from here, though the line's horizontal
           blank may come in another blank */
        if ((cur.inidisp & 0x80) && !(v & 0x80) && dirty && making->versions) {
            unsigned l = snes_vcounter();
            if (l >= 1 && l < snes_vblank_line())
                take_version();
        }
        cur.inidisp = v;
        return;
    case 0x01: cur.obsel = v; return;
    case 0x02:
        cur.oamaddr = (uint16_t)((cur.oamaddr & 0x100) | v);
        oam_at = (uint16_t)(cur.oamaddr << 1);
        return;
    case 0x03:
        cur.oamaddr = (uint16_t)((cur.oamaddr & 0xFF) | (v & 1) << 8);
        cur.oam_prio = v & 0x80;
        oam_at = (uint16_t)(cur.oamaddr << 1);
        return;
    case 0x04: {
        uint16_t a = oam_at & 0x3FF;
        dirty = 1;
        if (a < 0x200) {
            if (!(a & 1))
                oam_latch = v;
            else {
                oam[a - 1] = oam_latch;
                oam[a] = v;
            }
        } else {
            oam[0x200 | (a & 0x1F)] = v;
        }
        oam_at = (uint16_t)((oam_at + 1) & 0x3FF);
        return;
    }
    case 0x05: cur.bgmode = v; return;
    case 0x06: cur.mosaic = v; return;
    case 0x07: case 0x08: case 0x09: case 0x0A: cur.bgsc[reg - 7] = v; return;
    case 0x0B: cur.bgnba[0] = v; return;
    case 0x0C: cur.bgnba[1] = v; return;
    case 0x0D: case 0x0F: case 0x11: case 0x13: {
        int n = (reg - 0x0D) >> 1;
        cur.bghofs[n] = (uint16_t)(((v << 8) | (bgofs_latch & ~7) | (bghofs_latch & 7)) & 0x3FF);
        bgofs_latch = v;
        bghofs_latch = v;
        if (reg == 0x0D) {
            cur.m7hofs = (uint16_t)(((v << 8) | m7_latch) & 0x1FFF);
            m7_latch = v;
        }
        return;
    }
    case 0x0E: case 0x10: case 0x12: case 0x14: {
        int n = (reg - 0x0E) >> 1;
        cur.bgvofs[n] = (uint16_t)(((v << 8) | bgofs_latch) & 0x3FF);
        bgofs_latch = v;
        if (reg == 0x0E) {
            cur.m7vofs = (uint16_t)(((v << 8) | m7_latch) & 0x1FFF);
            m7_latch = v;
        }
        return;
    }
    case 0x15: vmain = v; return;
    case 0x16:
        vmaddr = (uint16_t)((vmaddr & 0xFF00) | v);
        vram_prefetch = vram[port_page << 15 | (vram_remap(vmaddr) & 0x7FFF)];
        return;
    case 0x17:
        vmaddr = (uint16_t)((vmaddr & 0x00FF) | v << 8);
        vram_prefetch = vram[port_page << 15 | (vram_remap(vmaddr) & 0x7FFF)];
        return;
    case 0x18: {
        uint32_t a = port_page << 15 | (vram_remap(vmaddr) & 0x7FFF);
        if (direct_page >= 0) {
            a = (uint32_t)direct_page << 15 | (vram_remap(vmaddr) & 0x7FFF);
            vram[a] = (uint16_t)((vram[a] & 0xFF00) | v);
            page_changed((unsigned)direct_page);
        } else if (vram_open()) {
            dirty = 1;
            vram[a] = (uint16_t)((vram[a] & 0xFF00) | v);
        }
        if (!(vmain & 0x80))
            vram_step();
        return;
    }
    case 0x19: {
        uint32_t a = port_page << 15 | (vram_remap(vmaddr) & 0x7FFF);
        if (direct_page >= 0) {
            a = (uint32_t)direct_page << 15 | (vram_remap(vmaddr) & 0x7FFF);
            vram[a] = (uint16_t)((vram[a] & 0x00FF) | v << 8);
            page_changed((unsigned)direct_page);
        } else if (vram_open()) {
            dirty = 1;
            vram[a] = (uint16_t)((vram[a] & 0x00FF) | v << 8);
        }
        if (vmain & 0x80)
            vram_step();
        return;
    }
    case 0x1A: cur.m7sel = v; return;
    case 0x1B: cur.m7a = (int16_t)(v << 8 | m7_latch); m7_latch = v; return;
    case 0x1C: cur.m7b = (int16_t)(v << 8 | m7_latch); m7_latch = v; m7b_last = v; return;
    case 0x1D: cur.m7c = (int16_t)(v << 8 | m7_latch); m7_latch = v; return;
    case 0x1E: cur.m7d = (int16_t)(v << 8 | m7_latch); m7_latch = v; return;
    case 0x1F: cur.m7x = (int16_t)(v << 8 | m7_latch); m7_latch = v; return;
    case 0x20: cur.m7y = (int16_t)(v << 8 | m7_latch); m7_latch = v; return;
    case 0x21: cgaddr = v; cg_flip = 0; return;
    case 0x22:
        if (!cg_flip) {
            cg_latch = v;
            cg_flip = 1;
        } else {
            cgram[cgram_target(4)] = (uint16_t)((v & 0x7F) << 8 | cg_latch);
            cgaddr++;
            cg_flip = 0;
        }
        return;
    case 0x23: cur.w12sel = v; return;
    case 0x24: cur.w34sel = v; return;
    case 0x25: cur.wobjsel = v; return;
    case 0x26: case 0x27: case 0x28: case 0x29: cur.wh[reg - 0x26] = v; return;
    case 0x2A: cur.wbglog = v; return;
    case 0x2B: cur.wobjlog = v; return;
    case 0x2C: cur.tm = v; return;
    case 0x2D: cur.ts = v; return;
    case 0x2E: cur.tmw = v; return;
    case 0x2F: cur.tsw = v; return;
    case 0x30: cur.cgwsel = v; return;
    case 0x31: cur.cgadsub = v; return;
    case 0x32:
        if (v & 0x20) cur.fixed_r = v & 0x1F;
        if (v & 0x40) cur.fixed_g = v & 0x1F;
        if (v & 0x80) cur.fixed_b = v & 0x1F;
        return;
    case 0x33: cur.setini = v; return;
    }
}

void ppu_latch_counters(uint16_t h, uint16_t v)
{
    h_latch = h;
    v_latch = v;
    latch_flag = 1;
}

/* The read side: the multiplier's product (M7A times M7B's last byte, as
   signed numbers), the sprite table, VRAM through its prefetch, CGRAM,
   the counters, and the two PPUs' status. Of the registers that only take
   writes, $2104-$2106, $2108-$210A, $2114-$2116, $2118-$211A, $2124-$2126
   and $2128-$212A read the first PPU's open bus (the rest the CPU's: the
   bus asks ppu_open1 first). */
int ppu_open1(uint8_t reg)
{
    switch (reg & 0x0F) {
    case 0x4: case 0x5: case 0x6: case 0x8: case 0x9: case 0xA:
        return (reg >> 4) == 0 || (reg >> 4) == 1 || (reg >> 4) == 2;
    }
    return 0;
}

uint8_t ppu_read(uint8_t reg)
{
    uint8_t v;
    switch (reg) {
    case 0x34: case 0x35: case 0x36: {
        uint32_t p = (uint32_t)((int32_t)cur.m7a * (int8_t)m7b_last);
        return ppu1_mdr = (uint8_t)(p >> ((reg - 0x34) * 8));
    }
    case 0x38: {
        uint16_t a = oam_at & 0x3FF;
        v = a < 0x200 ? oam[a] : oam[0x200 | (a & 0x1F)];
        oam_at = (uint16_t)((oam_at + 1) & 0x3FF);
        return ppu1_mdr = v;
    }
    case 0x39: case 0x3A:
        v = reg == 0x39 ? (uint8_t)vram_prefetch : (uint8_t)(vram_prefetch >> 8);
        if ((reg == 0x39) == !(vmain & 0x80)) {
            vram_prefetch = vram[port_page << 15 | (vram_remap(vmaddr) & 0x7FFF)];
            vram_step();
        }
        return ppu1_mdr = v;
    case 0x3B:
        if (!cg_flip) {
            v = (uint8_t)cgram[cgram_target(4)];
            cg_flip = 1;
        } else {
            v = (uint8_t)((cgram[cgram_target(4)] >> 8 & 0x7F) | (ppu2_mdr & 0x80));
            cgaddr++;
            cg_flip = 0;
        }
        return ppu2_mdr = v;
    case 0x3C:
        v = h_flip ? (uint8_t)((h_latch >> 8 & 1) | (ppu2_mdr & 0xFE)) : (uint8_t)h_latch;
        h_flip ^= 1;
        return ppu2_mdr = v;
    case 0x3D:
        v = v_flip ? (uint8_t)((v_latch >> 8 & 1) | (ppu2_mdr & 0xFE)) : (uint8_t)v_latch;
        v_flip ^= 1;
        return ppu2_mdr = v;
    case 0x3E:
        /* STAT77: time over, range over, the first PPU's version (1) */
        return ppu1_mdr = (uint8_t)(time_over << 7 | range_over << 6 | (ppu1_mdr & 0x10) | 0x01);
    case 0x3F:
        /* STAT78: the field, the latch, PAL, the second PPU's version (3) */
        v = (uint8_t)(field << 7 | latch_flag << 6 | (ppu2_mdr & 0x20) | (snes_pal() ? 0x10 : 0) | 0x03);
        h_flip = v_flip = 0;
        latch_flag = 0;
        return ppu2_mdr = v;
    }
    return ppu1_mdr;
}

void ppu_ext_screen(int width, int height)
{
    if (width < PPU_WIDTH || width > PPU_EXT_WIDTH || (width & 1) || height < 224 || height > PPU_EXT_HEIGHT || (height & 1))
        snes_fatal("nshvc_ext_screen(%d, %d): the width is 256 to %d, the height 224 to %d, both even", width, height,
                   PPU_EXT_WIDTH, PPU_EXT_HEIGHT);
    ext_width = width;
    ext_height = height;
}

void ppu_ext_sprite_limits(int on) { ext_limits = on != 0; }

void ppu_ext_vram(int kb)
{
    if (kb < 64 || kb > 64 * PPU_VRAM_PAGES || kb % 64)
        snes_fatal("nshvc_ext_vram(%d): 64 to %d KB, in 64 KB pages", kb, 64 * PPU_VRAM_PAGES);
    vram_pages = kb / 64;
    dirty = 1;
}

void ppu_ext_vram_page(int page)
{
    if (page < 0 || page >= vram_pages)
        snes_fatal("nshvc_ext_vram_page(%d): VRAM has %d page(s) (nshvc_ext_vram)", page, vram_pages);
    port_page = (unsigned)page;
    vram_prefetch = vram[port_page << 15 | (vram_remap(vmaddr) & 0x7FFF)];
}

void ppu_ext_page_reg(uint8_t reg, uint8_t v)
{
    if (vram_pages == 1)
        return;
    v = (uint8_t)(v % vram_pages);
    if (reg == 0xC5)
        ppu_ext_vram_page(v);
    else
        ppu_ext_layer_page(reg - 0xC0, v);
}

void ppu_ext_layer_page(int layer, int page)
{
    if (layer < 0 || layer > 4 || page < 0 || page >= vram_pages)
        snes_fatal("nshvc_ext_layer_page(%d, %d): layers 0-3 (BG1-4) and 4 (sprites), pages below %d", layer, page,
                   vram_pages);
    cur.page[layer] = (uint8_t)page;
    if (hidden_dirty)
        dirty = 1;
}

void ppu_ext_vram_copy(int to_page, int from_page)
{
    if (to_page < 1 || to_page >= vram_pages || from_page < 0 || from_page >= vram_pages)
        snes_fatal("nshvc_ext_vram_copy(%d, %d): to pages 1-%d, from 0-%d (page 0 is the console's)", to_page,
                   from_page, vram_pages - 1, vram_pages - 1);
    if (to_page == from_page)
        return;
    memcpy(&vram[(unsigned)to_page << 15], &vram[(unsigned)from_page << 15], 0x8000 * sizeof vram[0]);
    page_changed((unsigned)to_page);
}

void ppu_ext_vram_direct(int page)
{
    if (page != -1 && (page < 1 || page >= vram_pages))
        snes_fatal("nshvc_ext_vram_direct(%d): pages 1-%d, or -1 (page 0 is the console's)", page, vram_pages - 1);
    direct_page = page;
}

int ppu_vram_direct(void) { return direct_page; }

void ppu_ext_canvas_chars(int layer, unsigned word_addr, int count, const uint8_t *texels, int k)
{
    size_t len;
    if (!texels || count <= 0) {
        cv_now.on = 0;
        cv_now.gen++;
        return;
    }
    if (layer < 0 || layer > 3 || word_addr >= (unsigned)vram_pages << 15 || k < 1 || k > 64 || count > 4096)
        snes_fatal("nshvc_ext_canvas_chars(%d, $%05X, %d, k %d): layers 0-3 (BG1-4), a word address below $%05X "
                   "(the layer's page below $8000, VRAM's pages from it), at most 4,096 characters, k 1 to 64",
                   layer, word_addr, count, k, (unsigned)vram_pages << 15);
    len = (size_t)count * 64u * (size_t)k * (size_t)k;
    if (len > cv_now.cap) {
        uint8_t *n = (uint8_t *)realloc(cv_now.data, len);
        if (!n)
            snes_fatal("no memory for the canvas (%zu bytes)", len);
        cv_now.data = n;
        cv_now.cap = len;
    }
    memcpy(cv_now.data, texels, len);
    cv_now.on = 1;
    cv_now.layer = layer;
    cv_now.addr = word_addr;
    cv_now.count = (unsigned)count;
    cv_now.k = k;
    cv_now.len = len;
    cv_now.gen++;
}

void ppu_set_canvas_scale(int k) { cv_scale = k < 1 ? 1 : k > 64 ? 64 : k; }
int ppu_canvas_scale(void) { return cv_scale; }

/* the frame being made takes the canvas as it stands as the frame begins */
static void cv_take(void)
{
    int slot = making == &frames[0] ? 0 : 1;
    if (!cv_now.on) {
        making->cv_on = 0;
        return;
    }
    if (!making->cv_on || making->cv_gen != cv_now.gen) {
        if (cv_now.len > cv_kept_cap[slot]) {
            uint8_t *n = (uint8_t *)realloc(cv_kept[slot], cv_now.len);
            if (!n)
                snes_fatal("no memory for the canvas (%zu bytes)", cv_now.len);
            cv_kept[slot] = n;
            cv_kept_cap[slot] = cv_now.len;
        }
        memcpy(cv_kept[slot], cv_now.data, cv_now.len);
    }
    making->cv_on = 1;
    making->cv_layer = cv_now.layer;
    making->cv_addr = cv_now.addr;
    making->cv_count = cv_now.count;
    making->cv_k = cv_now.k;
    making->cv_len = cv_now.len;
    making->cv = cv_kept[slot];
    making->cv_gen = cv_now.gen;
}

void ppu_ext_sprite_y(int sprite, int on)
{
    if (sprite < 0 || sprite > 127)
        return;
    dirty = 1;
    ext_y[sprite >> 3] = (uint8_t)((ext_y[sprite >> 3] & ~(1u << (sprite & 7))) | (on ? 1u : 0u) << (sprite & 7));
}

void ppu_ext_sprite_x(int sprite, int on)
{
    if (sprite < 0 || sprite > 127)
        return;
    dirty = 1;
    ext_x[sprite >> 3] = (uint8_t)((ext_x[sprite >> 3] & ~(1u << (sprite & 7))) | (on ? 1u : 0u) << (sprite & 7));
}

void ppu_frame_start(void)
{
    making->width = ext_width;
    making->screen_height = ext_height;
    sw = ext_width;
    sl = (ext_width - PPU_WIDTH) / 2;
    mosaic_y = 1;
    range_over = time_over = 0;
    making->field = field;
    making->interlace = cur.setini & 0x01;
}

void ppu_field_flip(void)
{
    field ^= 1;
}

/* NSHVC_PPUCHECK=1: each finished frame drawn again from its record and
   compared with the picture drawn a line at a time */
unsigned long ppu_check_frames, ppu_check_bad;
static uint32_t check_pic[2 * PPU_EXT_WIDTH * PPU_EXT_HEIGHT];

/* A frame drawn onto the woven picture's rows of its field (-1: both). The
   picture is as wide as the wider of its fields (a 256-wide line's pixels
   doubled beside a hires field's); a picture of another screen starts over
   from this frame's on both. */
static uint32_t field_pic[2 * PPU_EXT_WIDTH * PPU_EXT_HEIGHT];

static void weave(const PpuFrame *f, int rows_of)
{
    int w = ppu_frame_width(f), h = f->screen_height, r, x, p;
    ppu_draw_frame(f, field_pic);
    if (2 * h != woven_h || (woven_w != f->width && woven_w != 2 * f->width)) {
        woven_w = w;
        woven_h = 2 * h;
        rows_of = -1;
    } else if (w > woven_w) {
        /* a hires field beside a picture that was not: the rows kept widened */
        for (r = woven_h; r-- > 0;)
            for (x = woven_w; x-- > 0;)
                woven[(size_t)r * (size_t)w + 2 * x] = woven[(size_t)r * (size_t)w + 2 * x + 1] =
                    woven[(size_t)r * (size_t)woven_w + x];
        woven_w = w;
    }
    for (r = 0; r < h; r++)
        for (p = 0; p < 2; p++) {
            const uint32_t *from = field_pic + (size_t)r * (size_t)w;
            uint32_t *to = woven + (size_t)(2 * r + p) * (size_t)woven_w;
            if (rows_of >= 0 && p != rows_of)
                continue;
            if (w == woven_w)
                memcpy(to, from, (size_t)w * sizeof *to);
            else
                for (x = 0; x < w; x++)
                    to[2 * x] = to[2 * x + 1] = from[x];
        }
}

void ppu_vblank_start(void)
{
    static int check = -1;
    PpuFrame *t = finished;
    making->height = (int)snes_vblank_line() - 1;
    /* the height asked for as the frame began, at least the hardware's */
    if (making->screen_height < making->height)
        making->screen_height = making->height;
    finished = making;
    making = t;
    frames_done++;
    if (finished->interlace) {
        /* the frame before (its record still whole until the next frame's
           lines are recorded over it) if it is not woven in yet: both its
           rows when it was not interlaced */
        if (woven_of != frames_done - 1 && frames_done > 1)
            weave(making, making->interlace ? making->field : -1);
        weave(finished, finished->field);
        woven_of = frames_done;
    }
    if (check < 0)
        check = getenv("NSHVC_PPUCHECK") != NULL;
    if (check) {
        int w = ppu_frame_width(finished), y, pw = 2 * finished->width;
        int top = (finished->screen_height - finished->height) / 2;
        ppu_draw_frame(finished, check_pic);
        ppu_check_frames++;
        /* the lines were drawn 512 wide: a 256-wide frame is their even pixels */
        for (y = 0; y < finished->height; y++) {
            int x, bad = 0;
            for (x = 0; x < w && !bad; x++)
                bad = check_pic[(y + top) * w + x] != picture[y * pw + (w == finished->width ? 2 * x : x)];
            if (bad) {
                ppu_check_bad++;
                break;
            }
        }
    }
    /* forced blank lifted: OAM's address reloads at the vertical blank */
    if (!(cur.inidisp & 0x80))
        oam_at = (uint16_t)(cur.oamaddr << 1);
}

/* ---- drawing ---------------------------------------------------------------------- */
/* What one screen of one line holds at each pixel: a colour (15-bit BGR),
   its depth (higher in front; 0 the backdrop) and the layer it came from
   (0-3 BG1-4, 4 sprites with palettes 4-7, 5 the backdrop, 6 sprites with
   palettes 0-3, which colour maths never takes). */
typedef struct {
    uint16_t color;
    uint8_t z, layer;
    uint8_t ci;               /* the CGRAM entry its colour came from */
} Pixel;

static Pixel main_px[PPU_EXT_WIDTH], sub_px[PPU_EXT_WIDTH];

/* a layer's pixels for one line: colour, palette-0 (transparent) and
   priority */
typedef struct {
    uint16_t color;
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
    if (!d_frame || !d_frame->cv_on || d_frame->cv_layer != n)
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
        e = map_entry(n, tx, ty);
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

/* a sprite's X: the table's 9 bits as signed, and 512 more where the
   extension's bit is set (nshvc_ext_sprite_x) */
static int obj_x(unsigned s, uint8_t hi)
{
    int x = d_oam[s * 4] | ((hi & 1) << 8);
    if (x >= 256)
        x -= 512;
    if (d_oam[544 + (s >> 3)] >> (s & 7) & 1)
        x += 512;
    return x;
}

/* Whether sprite s (y its table's Y, h its height) covers row r of the
   screen (0 the hardware's line 1), and its row there. The hardware's Y
   wraps in 256 rows; a taller screen's rows past those 256 show only the
   sprites whose extended Y bit (nshvc_ext_sprite_y) puts them 256 lower. */
static int obj_row(unsigned s, unsigned y, unsigned h, int r, unsigned *row)
{
    int d;
    if (d_oam[560 + (s >> 3)] >> (s & 7) & 1) {
        d = r - ((int)y + 256);
        if (d < 0 || d >= (int)h)
            return 0;
        *row = (unsigned)d;
        return 1;
    }
    if (r >= 256)
        return 0;
    d = (r - (int)y) & 0xFF;
    if (d >= (int)h)
        return 0;
    *row = (unsigned)d;
    return 1;
}

static void draw_obj(unsigned line)
{
    unsigned first = s->oam_prio ? (s->oamaddr >> 1) & 0x7F : 0;
    unsigned i, n = 0, tiles = 0, k;
    unsigned list[32];
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
    for (i = 0; i < 128; i++)
        keep[i] = -1;
    /* the console's: the first 32 sprites in range of its 256 columns
       (range over past them), then their tiles in those columns from the
       last in the list, up to 34 (time over past them) */
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
            break;
        }
        tiles += on;
        keep[sp] = (int)on;
    }
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
            kept = !ext_limits;
            if (tx > -8 && tx < PPU_WIDTH) {
                if (ext_limits)
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
static uint8_t win_mask(uint8_t sel, uint8_t logic, int x)
{
    int w1 = x >= s->wh[0] && x <= s->wh[1], w2 = x >= s->wh[2] && x <= s->wh[3];
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
        if (!op[x].opaque)
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


/* A change of a line being drawn onto the state it shows from: the
   fields its register sets (the scroll's fine part stays the line's), or
   its colour. */
static void apply_event(PpuScreen *d, uint16_t *pal, const PpuEvent *e)
{
    const PpuScreen *a = &e->after;
    uint8_t r = e->reg;
    switch (r) {
    case 0x00: d->inidisp = a->inidisp; return;
    case 0x01: d->obsel = a->obsel; return;
    case 0x05: d->bgmode = a->bgmode; return;
    case 0x06: d->mosaic = a->mosaic; return;
    case 0x07: case 0x08: case 0x09: case 0x0A: d->bgsc[r - 0x07] = a->bgsc[r - 0x07]; return;
    case 0x0B: d->bgnba[0] = a->bgnba[0]; return;
    case 0x0C: d->bgnba[1] = a->bgnba[1]; return;
    case 0x0D: case 0x0F: case 0x11: case 0x13: {
        int n = (r - 0x0D) >> 1;
        d->bghofs[n] = (uint16_t)((a->bghofs[n] & ~7u) | (d->bghofs[n] & 7u));
        return;
    }
    case 0x0E: case 0x10: case 0x12: case 0x14: {
        int n = (r - 0x0E) >> 1;
        d->bgvofs[n] = a->bgvofs[n];
        return;
    }
    case 0x1A: d->m7sel = a->m7sel; return;
    case 0x1B: d->m7a = a->m7a; return;
    case 0x1C: d->m7b = a->m7b; return;
    case 0x1D: d->m7c = a->m7c; return;
    case 0x1E: d->m7d = a->m7d; return;
    case 0x1F: d->m7x = a->m7x; return;
    case 0x20: d->m7y = a->m7y; return;
    case 0x8D: d->m7hofs = a->m7hofs; return;
    case 0x8E: d->m7vofs = a->m7vofs; return;
    case 0x22: pal[e->index] = e->color; return;
    case 0x23: d->w12sel = a->w12sel; return;
    case 0x24: d->w34sel = a->w34sel; return;
    case 0x25: d->wobjsel = a->wobjsel; return;
    case 0x26: case 0x27: case 0x28: case 0x29: d->wh[r - 0x26] = a->wh[r - 0x26]; return;
    case 0x2A: d->wbglog = a->wbglog; return;
    case 0x2B: d->wobjlog = a->wobjlog; return;
    case 0x2C: d->tm = a->tm; return;
    case 0x2D: d->ts = a->ts; return;
    case 0x2E: d->tmw = a->tmw; return;
    case 0x2F: d->tsw = a->tsw; return;
    case 0x30: d->cgwsel = a->cgwsel; return;
    case 0x31: d->cgadsub = a->cgadsub; return;
    case 0x32: d->fixed_r = a->fixed_r; d->fixed_g = a->fixed_g; d->fixed_b = a->fixed_b; return;
    case 0x33: d->setini = a->setini; return;
    }
}

/* this line into the frame's record: its screen state and palette, and
   which copy of the tiles and sprites it shows (a new copy when they have
   changed since the last line shown; VRAM changes only while the screen is
   blanked) */
/* A copy of the tiles and sprites as they stand, for the lines drawn from
   now on (they change only while nothing is drawn: in a blank) */
static void take_version(void)
{
    if (making->versions < PPU_VERSIONS) {
        memcpy(making->vram[making->versions], vram, (size_t)vram_pages * 0x8000 * sizeof vram[0]);
        memcpy(making->oam[making->versions], oam, sizeof oam);
        memcpy(making->oam[making->versions] + 544, ext_x, sizeof ext_x);
        memcpy(making->oam[making->versions] + 560, ext_y, sizeof ext_y);
        making->versions++;
    } else {
        ppu_version_overflow++;
    }
    dirty = 0;
    hidden_dirty = 0;
}

static void record_line(unsigned line)
{
    if (line == 1) {
        cv_take();
        making->versions = 0;
        making->hires = 0;
        dirty = 1;
    }
    if (dirty && (making->versions == 0 || !(cur.inidisp & 0x80)))
        take_version();
    making->version[line] = (uint8_t)(making->versions - 1);
    making->segs[line] = 0;
    if (live_vline == line && live_n) {
        PpuScreen st = live_base;
        uint16_t pal[256];
        unsigned order[PPU_EVENTS], i, j, k = 0;
        memcpy(pal, live_base_pal, sizeof pal);
        /* the changes in the order they show (a stable sort: those that
           show at one pixel in the order made) */
        for (i = 0; i < live_n; i++) {
            for (j = i; j > 0 && live_ev[order[j - 1]].x > live_ev[i].x; j--)
                order[j] = order[j - 1];
            order[j] = i;
        }
        for (i = 0; i < live_n && live_ev[order[i]].x == 0; i++)
            apply_event(&st, pal, &live_ev[order[i]]);
        making->line[line] = st;
        memcpy(making->palette[line], pal, sizeof pal);
        while (i < live_n) {
            int x = live_ev[order[i]].x;
            for (; i < live_n && live_ev[order[i]].x == x; i++)
                apply_event(&st, pal, &live_ev[order[i]]);
            if (k == PPU_SEGS) {
                /* no segment left: the rest show with the last */
                ppu_segment_overflow++;
                making->seg_line[line][k - 1] = st;
                memcpy(making->seg_palette[line][k - 1], pal, sizeof pal);
                continue;
            }
            making->seg_x[line][k] = (uint16_t)x;
            making->seg_line[line][k] = st;
            memcpy(making->seg_palette[line][k], pal, sizeof pal);
            if (!(st.inidisp & 0x80) && ppu_line_hires(&st))
                making->hires = 1;
            k++;
        }
        making->segs[line] = (uint8_t)k;
    } else {
        making->line[line] = cur;
        memcpy(making->palette[line], cgram, sizeof cgram);
    }
    live_n = 0;
    if (!(making->line[line].inidisp & 0x80) && ppu_line_hires(&making->line[line]))
        making->hires = 1;
}

static void draw_line(unsigned line, uint32_t *out, int width);

/* A recorded line: its segments each drawn with their state and palette,
   each kept from its first pixel to the next's. */
static void draw_recorded(const PpuFrame *f, unsigned line, uint32_t *out, int width)
{
    static uint32_t part[2 * PPU_EXT_WIDTH];
    unsigned k, n = f->segs[line], scale = width == 2 * sw ? 2 : 1;
    draw_row = (int)line - 1;
    s = &f->line[line];
    d_cgram = f->palette[line];
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

void ppu_render_line(unsigned line)
{
    unsigned msize;
    if (line > PPU_MAX_HEIGHT)
        return;
    if (draw_lines < 0)
        draw_lines = getenv("NSHVC_PPUCHECK") != NULL;
    if (mosaic_y == 0)
        mosaic_y = 1;
    record_line(line);
    making->mosaic_row[line] = (uint8_t)mosaic_y;
    if (draw_lines) {
        d_field = making->field;
        d_vram = vram;
        d_oam = live_oam();
        d_frame = making;
        draw_recorded(making, line, &picture[(line - 1) * 2 * sw], 2 * sw);
        d_frame = NULL;
    }
    /* the vertical mosaic: the line the next lines repeat moves on every
       `size` lines */
    msize = (making->line[line].mosaic >> 4) + 1u;
    if (msize == 1 || line % msize == 0)
        mosaic_y = line + 1;
}

void ppu_draw_frame(const PpuFrame *f, uint32_t *out)
{
    unsigned keep = mosaic_y;
    int w = ppu_frame_width(f), keep_sw = sw, keep_sl = sl, top = (f->screen_height - f->height) / 2, r;
    const PpuScreen *keep_s = s;
    uint8_t ro = range_over, to = time_over;
    sw = f->width;
    d_field = f->field;
    d_frame = f;
    sl = (f->width - PPU_WIDTH) / 2;
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
            s = &f->line[edge];
            d_cgram = f->palette[edge];
            d_vram = f->vram[f->version[edge]];
            d_oam = f->oam[f->version[edge]];
            draw_row = line - 1;
            mosaic_y = (unsigned)line;
            draw_line((unsigned)edge, o, w);
        }
    }
    /* the sprite flags are the console's, not this drawing's */
    range_over = ro;
    time_over = to;
    d_frame = NULL;
    mosaic_y = keep;
    s = keep_s;
    sw = keep_sw;
    sl = keep_sl;
    d_vram = vram;
    d_cgram = cgram;
    d_oam = oam;
}

void ppu_pack_line(const PpuFrame *f, unsigned line, uint32_t *w)
{
    const PpuScreen *l = &f->line[line];
    w[0] = l->inidisp | l->obsel << 8 | l->bgmode << 16 | (uint32_t)l->mosaic << 24;
    w[1] = l->bgsc[0] | l->bgsc[1] << 8 | l->bgsc[2] << 16 | (uint32_t)l->bgsc[3] << 24;
    w[2] = l->bgnba[0] | l->bgnba[1] << 8 | l->m7sel << 16 | (uint32_t)l->setini << 24;
    w[3] = l->w12sel | l->w34sel << 8 | l->wobjsel << 16 | (uint32_t)l->wbglog << 24;
    w[4] = l->wh[0] | l->wh[1] << 8 | l->wh[2] << 16 | (uint32_t)l->wh[3] << 24;
    w[5] = l->wobjlog | l->tm << 8 | l->ts << 16 | (uint32_t)l->tmw << 24;
    w[6] = l->tsw | l->cgwsel << 8 | l->cgadsub << 16 | (uint32_t)l->oam_prio << 24;
    w[7] = l->bghofs[0] | (uint32_t)l->bghofs[1] << 16;
    w[8] = l->bghofs[2] | (uint32_t)l->bghofs[3] << 16;
    w[9] = l->bgvofs[0] | (uint32_t)l->bgvofs[1] << 16;
    w[10] = l->bgvofs[2] | (uint32_t)l->bgvofs[3] << 16;
    w[11] = l->fixed_r | l->fixed_g << 8 | l->fixed_b << 16;
    w[12] = l->oamaddr | (uint32_t)f->version[line] << 16 | (uint32_t)f->mosaic_row[line] << 24;
    w[13] = (uint16_t)l->m7a | (uint32_t)(uint16_t)l->m7b << 16;
    w[14] = (uint16_t)l->m7c | (uint32_t)(uint16_t)l->m7d << 16;
    w[15] = (uint16_t)l->m7x | (uint32_t)(uint16_t)l->m7y << 16;
    w[16] = l->m7hofs | (uint32_t)l->m7vofs << 16;
    w[17] = (uint32_t)f->hires;
    w[18] = w[19] = 0;
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
    uint16_t c = clip ? 0 : m->color;
    unsigned r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    if (bit < 8 && (s->cgadsub >> bit & 1) && !prevent) {
        uint16_t o;
        int halve = (s->cgadsub & 0x40) != 0 && !clip;
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
        if (s->cgadsub & 0x80) {
            int rr = (int)r - (o & 31), gg = (int)g - ((o >> 5) & 31), bb = (int)b - ((o >> 10) & 31);
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

/* Where a colour written or read now lands. While a line is being drawn
   (the screen not blanked, clocks 84-1091 of lines 1-224) the PPU is
   looking colours up, and CGRAM's port goes to the entry it looked up
   last: the main screen's colour of the last pixel made (pixel x at 54 +
   4x); else CGADD's. A write lands at the end of the CPU's cycle, 4
   clocks after where a read looks (`ahead`). */
static unsigned cgram_target(unsigned ahead)
{
    static uint32_t scratch[2 * PPU_EXT_WIDTH];
    const PpuScreen *keep_s = s;
    const uint16_t *kv = d_vram, *kc = d_cgram;
    const uint8_t *ko = d_oam;
    unsigned v = snes_vcounter(), c, x;
    if ((cur.inidisp & 0x80) || v < 1 || v >= snes_vblank_line() || v > PPU_MAX_HEIGHT)
        return cgaddr;
    c = snes_line_clock() + ahead;
    if (c < 88 || c >= 1096)
        return cgaddr;
    x = (c - 59) / 4;
    if (x > PPU_WIDTH - 1)
        x = PPU_WIDTH - 1;
    s = &cur;
    d_vram = vram;
    d_cgram = cgram;
    d_oam = live_oam();
    draw_row = (int)v - 1;
    draw_line(v, scratch, sw);
    s = keep_s;
    d_vram = kv;
    d_cgram = kc;
    d_oam = ko;
    return main_px[x + (unsigned)sl].ci;
}

/* ---- the presentation: the frame at the output's resolution ------------------------- */
/* (ppu.h ppu_mode7, render/rr_mode7.h) Each picture pixel on a Mode 7 line
   is given everything of draw_line's rules that does not depend on where
   the sample falls in it: the sprites' pixel over the backdrop on each
   screen, which Mode 7 layer may show on each (its enable bits and
   windows), the colour window's regions; its line's or segment's state
   once in the segment table. */
static RrM7Px *m7_px;
static size_t m7_px_cap;
static uint8_t m7_row[PPU_EXT_HEIGHT];
static RrM7Seg m7_seg[PPU_EXT_HEIGHT * (PPU_SEGS + 1)];
static uint16_t m7_pal[PPU_EXT_HEIGHT * (PPU_SEGS + 1)][256];
static uint16_t m7_vram[PPU_VERSIONS * PPU_VRAM_PAGES][0x8000];
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

int ppu_frame_mode7(const PpuFrame *f)
{
    int line, k;
    for (line = 1; line <= f->height; line++) {
        if (pres_is_m7(&f->line[line]) || pres_is_cv(f, &f->line[line]))
            return 1;
        for (k = 0; k < f->segs[line]; k++)
            if (pres_is_m7(&f->seg_line[line][k]) || pres_is_cv(f, &f->seg_line[line][k]))
                return 1;
    }
    return 0;
}

/* whether background n may show on a screen at column x (its enable bit
   and windows: put_layer's) */
static int cv_may(int n, int x, int sub)
{
    unsigned on = sub ? s->ts : s->tm, wn = sub ? s->tsw : s->tmw;
    int inside;
    if (!(on >> n & 1))
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
    if (!o->opaque || !(((sub ? s->ts : s->tm) >> 4) & 1))
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
    if (!(on >> n & 1) || (n == 1 && !(s->setini & 0x40)))
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
                    snes_fatal("no memory for the Mode 7 plane");
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
    int pw = ppu_frame_width(f), ph = f->screen_height, cpp = pw / f->width, top = (ph - f->height) / 2, r;
    int keep_sw = sw, keep_sl = sl, keep_row = draw_row;
    unsigned keep_my = mosaic_y;
    const PpuScreen *keep_s = s;
    uint8_t ro = range_over, to = time_over;
    size_t need = (size_t)pw * (size_t)ph;
    if (need > m7_px_cap) {
        free(m7_px);
        m7_px = (RrM7Px *)malloc(need * sizeof *m7_px);
        m7_px_cap = m7_px ? need : 0;
        if (!m7_px)
            return 0;
    }
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
    sl = (f->width - PPU_WIDTH) / 2;
    m->left = sl;
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
            const PpuScreen *st = k ? &f->seg_line[line][k - 1] : &f->line[edge];
            const uint16_t *pal = k ? f->seg_palette[line][k - 1] : f->palette[edge];
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
                d_frame = f;
                cv_capture = 1;
                draw_bg(cn, (unsigned)edge, mode);
                cv_capture = 0;
                d_frame = NULL;
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
                                (uint32_t)cn << 29;
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
    mosaic_y = keep_my;
    range_over = ro;
    time_over = to;
    s = keep_s;
    sw = keep_sw;
    sl = keep_sl;
    draw_row = keep_row;
    d_vram = vram;
    d_cgram = cgram;
    d_oam = oam;
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
