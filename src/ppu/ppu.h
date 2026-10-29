/*
 * ppu.h: the picture unit (ppu.c). Platform agnostic C: the game's writes
 * to the PPU's registers ($2100-$2133) and its reads ($2134-$213F) in;
 * each line's state, the tiles, palette and sprites recorded as the frame
 * is shown; a picture out.
 */
#ifndef NSHVC_PPU_H
#define NSHVC_PPU_H

#include <stdint.h>

#define PPU_WIDTH       256
#define PPU_HIRES_WIDTH 512
#define PPU_EXT_WIDTH   1024    /* the widest screen nshvc_ext_screen sets */
#define PPU_EXT_HEIGHT  480     /* the tallest */
#define PPU_VRAM_PAGES  4       /* VRAM in 64 KB pages, up to 256 KB (nshvc_ext_vram) */
#define PPU_MAX_HEIGHT  239

/* The screen state a line is drawn from, as it stood at the line's
   horizontal blank. */
typedef struct {
    uint8_t inidisp, obsel, bgmode, mosaic, bgsc[4], bgnba[2], m7sel;
    uint8_t w12sel, w34sel, wobjsel, wh[4], wbglog, wobjlog, tm, ts, tmw, tsw, cgwsel, cgadsub, setini;
    uint16_t bghofs[4], bgvofs[4];
    uint16_t m7hofs, m7vofs;
    int16_t m7a, m7b, m7c, m7d, m7x, m7y;
    uint8_t fixed_r, fixed_g, fixed_b;
    uint16_t oamaddr;
    uint8_t oam_prio;
    uint8_t page[5];          /* the VRAM page BG1-4 and the sprites read (nshvc_ext_layer_page) */
} PpuScreen;

/* A frame, everything its picture is drawn from: each line's screen state
   and palette, and the copies of the tiles and sprite table its lines show
   (more than one when the game changed them in a blanked stretch of the
   frame); what the GPU compositor draws from. */
#define PPU_VERSIONS 4
/* A line whose state the game changed while it was being drawn (a write
   from an IRQ, a DMA in the middle of a line) is drawn in segments: segment
   0 from line[] and palette[], each further one from pixel seg_x on with
   its own state and palette. */
#define PPU_SEGS 8
typedef struct {
    PpuScreen line[PPU_MAX_HEIGHT + 1];         /* lines 1 to height */
    uint16_t palette[PPU_MAX_HEIGHT + 1][256];
    uint8_t version[PPU_MAX_HEIGHT + 1];        /* the copy each line shows */
    uint8_t mosaic_row[PPU_MAX_HEIGHT + 1];     /* the line a vertical mosaic repeats */
    int height;                                 /* 224, or 239 with overscan */
    int width;                                  /* 256, or wider (nshvc_ext_screen) */
    int screen_height;                          /* height, or taller */
    int left, top;                              /* the screen's column of the hardware's column 0
                                                   and row of its line 1 (nshvc_ext_screen_origin;
                                                   the centre by default) */
    int hires;                                  /* a line of the frame is hires (512 wide) */
    int field;                                  /* its field (STAT78 bit 7) */
    int interlace;                              /* interlaced (SETINI bit 0 as the frame began): its
                                                   lines are the field's rows of a picture twice as
                                                   tall, woven with the frame before's */
    int versions;
    uint16_t vram[PPU_VERSIONS][PPU_VRAM_PAGES * 0x8000];
    uint8_t oam[PPU_VERSIONS][576];             /* the table, then the extended X and Y bits */
    uint8_t segs[PPU_MAX_HEIGHT + 1];           /* segments past the first (0: none) */
    uint16_t seg_x[PPU_MAX_HEIGHT + 1][PPU_SEGS];
    PpuScreen seg_line[PPU_MAX_HEIGHT + 1][PPU_SEGS];
    uint16_t seg_palette[PPU_MAX_HEIGHT + 1][PPU_SEGS][256];
    /* the canvas the frame shows (nshvc_ext_canvas_chars), as it stood
       when the frame began: its layer, the VRAM word address its first
       character's data starts at, its characters, the texels a console
       pixel spans along each axis (k), and the texels, each character 8k x
       8k of them, row by row (cv_len bytes); cv_on 0: none */
    int cv_on, cv_layer, cv_k;
    unsigned cv_addr, cv_count;
    size_t cv_len;
    const uint8_t *cv;
    uint32_t cv_gen;
} PpuFrame;

/* A line's screen state packed for the GPU compositor: PPU_LINE_WORDS
   32-bit words (the layout render/compose/core.glsl reads). */
#define PPU_LINE_WORDS 20
void ppu_pack_line(const PpuFrame *f, unsigned line, uint32_t *w);
/* whether a line's state draws it 512 pixels wide (modes 5 and 6, or
   SETINI's pseudo-hires) */
int ppu_line_hires(const PpuScreen *s);

/* the last finished frame's record */
const PpuFrame *ppu_frame(void);
/* a frame drawn from its record (the reference the GPU compositor is held
   to): ppu_frame_width(f) x f->screen_height pixels, 0xFFRRGGBB: f->width,
   twice that when a line is hires */
void ppu_draw_frame(const PpuFrame *f, uint32_t *out);
static inline int ppu_frame_width(const PpuFrame *f) { return f->hires ? 2 * f->width : f->width; }
/* the last finished frame, drawn from its record when first asked for; an
   interlaced one woven with the frame before (twice f->screen_height rows,
   as wide as the wider of the two) */
const uint32_t *ppu_picture(int *width, int *height);

/* The presentation of a frame with Mode 7 or a layer's canvas
   (render/rr_mode7.h): m set to
   frame f's record for rr_mode7, pic its picture (ppu_draw_frame's, one
   row a line: not an interlaced weave); valid until the next call. 0 when
   there is no memory for it. */
struct RrMode7;
int ppu_mode7(const PpuFrame *f, const uint32_t *pic, struct RrMode7 *m);
/* frame f presented at ow x oh pixels into out (0xFFRRGGBB): rr_mode7.h
   rr_ref_mode7 of ppu_mode7's record, as the window shows it. Every
   output pixel is pic scaled with sharp bilinear exactly as rr_ref_image
   scales it, but that a picture pixel on a Mode 7 line is composed again
   by the console's rules for that pixel with the Mode 7 plane under the
   output pixel, filtered by the pixel's footprint on the plane (its
   texels' edges covered where magnified, supersampled where minified; a
   Mode 7 layer under mosaic keeps its block's texel), and a picture pixel
   of a layer's canvas composed again with the canvas's texel under the
   output pixel's centre. */
void ppu_present(const PpuFrame *f, const uint32_t *pic, uint32_t *out, int ow, int oh);
/* whether a line of f shows a Mode 7 layer or the frame's canvas (else
   ppu_present is pic scaled, nothing more) */
int ppu_frame_mode7(const PpuFrame *f);

/* ---- the console's side ---- */
void ppu_reset(void);
/* A write to $2100 + reg ($00-$33). */
void ppu_write(uint8_t reg, uint8_t v);
/* A read of $2100 + reg ($00-$3F but $37, which the bus latches). */
uint8_t ppu_read(uint8_t reg);
/* whether a write-only register ($00-$33) reads the first PPU's open bus */
int ppu_open1(uint8_t reg);
/* the counters latched (SLHV, WRIO): the dot and the line */
void ppu_latch_counters(uint16_t h, uint16_t v);
void ppu_frame_start(void);              /* line 0 */
void ppu_render_line(unsigned line);     /* lines 1 to the frame's height */
void ppu_vblank_start(void);             /* the frame is done */
int ppu_forced_blank(void);
int ppu_overscan(void);                  /* SETINI bit 2 as it stands */
int ppu_interlace(void);                 /* SETINI bit 0 as it stands */
int ppu_field(void);                     /* the field (STAT78 bit 7) */
void ppu_field_flip(void);               /* a new field: the counters reached line 0 */
unsigned long ppu_frames(void);

/* the extensions (nshvc.h nshvc_ext_screen, nshvc_ext_sprite_x) */
void ppu_ext_screen(int width, int height);
void ppu_ext_screen_origin(int x, int y);
void ppu_ext_sprite_x(int sprite, int on);
void ppu_ext_sprite_y(int sprite, int on);
void ppu_ext_sprite_limits(int on);
void ppu_ext_vram(int kb);
void ppu_ext_vram_page(int page);
void ppu_ext_layer_page(int layer, int page);
void ppu_ext_vram_copy(int to_page, int from_page);
void ppu_ext_canvas_chars(int layer, unsigned word_addr, int count, const uint8_t *texels, int k);
/* the texels a console pixel spans along each axis at the window's size
   now (nshvc_ext_canvas_scale): the front end sets it as it presents */
void ppu_set_canvas_scale(int k);
int ppu_canvas_scale(void);
void ppu_ext_vram_direct(int page);
/* the page the data port's writes go to directly (nshvc_ext_vram_direct),
   or -1 */
int ppu_vram_direct(void);
/* a write to $21C0-$21C5: ignored with no VRAM past 64 KB, else masked to its pages */
void ppu_ext_page_reg(uint8_t reg, uint8_t v);

/* the memories as they stand */
const uint16_t *ppu_vram(void);
const uint16_t *ppu_cgram(void);
const uint8_t *ppu_oam(void);

/* PPU checks (NSHVC_PPUCHECK): frames drawn from their records against
   the frames drawn a line at a time */
extern unsigned long ppu_check_frames, ppu_check_bad, ppu_version_overflow, ppu_segment_overflow;

#endif
