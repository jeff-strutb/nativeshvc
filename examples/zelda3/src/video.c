// video.c: the game's picture, on nativeshvc.
#include <string.h>
#include "nshvc/nshvc.h"
#include "video.h"
#include "variables.h"
#include "zelda_rtl.h"

static nshvc_tiles g_bg_tiles, g_text_tiles, g_obj_tiles, g_m7_tiles;
static nshvc_map g_bg_large[2], g_bg_small[2], g_text_map, g_m7_map;
static uint8 g_m7_texels[0x4000], g_m7_cells[0x4000];
static bool g_m7_texels_dirty, g_m7_cells_dirty;
static uint8 g_bg1_map = 0x13, g_bg2_map = 3;
static uint16 g_oam[0x110];
static uint8 g_fixed[3];
static int g_window[4];
static int16 g_m7a, g_m7d;
static int g_extra, g_show_left, g_show_right;
static float g_aspect;
static bool g_split_text;
static uint16 g_split_text_scroll;

// A stretch of lines with one value, or a value the game keeps changing
typedef struct LineRun {
  uint8 lines;
  uint16 value;
  const uint16 *source;
} LineRun;
typedef struct LineEffect {
  int target;
  const uint16 *one_per_line;
  const LineRun *runs;
} LineEffect;
static LineEffect g_effects[2];

static const LineRun kAttractWindows1[] = { {0x20, 0x00ff}, {0x50, 0xe018}, {0x50, 0xe018}, {1, 0x00ff}, {0} };
static const LineRun kAttractWindows2[] = { {0x48, 0x00ff}, {0x30, 0xd830}, {1, 0x00ff}, {0} };

void Video_Init(int extra_columns, float aspect) {
  g_extra = extra_columns;
  g_aspect = aspect;
  g_bg_tiles = nshvc_tiles_create(4, 1024, 1);
  g_obj_tiles = nshvc_tiles_create(4, 512, 1);
  g_text_tiles = nshvc_tiles_create(2, 512, 1);
  g_m7_tiles = nshvc_tiles_create(8, 256, 1);
  for (int i = 0; i < 2; i++) {
    g_bg_large[i] = nshvc_map_create(64, 64);
    g_bg_small[i] = nshvc_map_create(32, 32);
  }
  g_text_map = nshvc_map_create(64, 64);
  g_m7_map = nshvc_map_create(128, 128);
}

// Tilemap words at |addr|, in rows of a 64 x 64 map
static void WriteMap(nshvc_map m, uint32 offs, const uint16 *w, int n) {
  while (n > 0) {
    int screen = offs >> 10, cell = offs & 0x3ff;
    int x = cell & 31, y = cell >> 5, run = IntMin(n, 32 - x);
    nshvc_map_write_snes(m, (screen & 1) * 32 + x, (screen >> 1) * 32 + y, run, 1, w);
    offs += run, w += run, n -= run;
  }
}

void Video_BgTiles(int first, const void *planar, int count) {
  nshvc_tiles_write(g_bg_tiles, first, planar, count, NSHVC_TILES_PLANAR);
}

void Video_SpriteTiles(int first, const void *planar, int count) {
  nshvc_tiles_write(g_obj_tiles, first, planar, count, NSHVC_TILES_PLANAR);
  nshvc_tiles_write(g_bg_tiles, first + 512, planar, count, NSHVC_TILES_PLANAR);
}

void Video_TextTiles(int first, const void *planar, int count) {
  nshvc_tiles_write(g_text_tiles, first, planar, count, NSHVC_TILES_PLANAR);
}

void Video_WriteLocation(uint32 addr, const void *words, int count) {
  const uint16 *w = (const uint16 *)words;
  if (bg_mode == 7 && addr < 0x4000) {
    for (int i = 0; i < count; i++) {
      g_m7_cells[addr + i] = (uint8)w[i];
      g_m7_texels[addr + i] = w[i] >> 8;
    }
    g_m7_cells_dirty = g_m7_texels_dirty = true;
    return;
  }
  if (addr < 0x2000) {
    int i = addr >> 12;
    WriteMap(g_bg_large[i], addr & 0xfff, w, count);
    if ((addr & 0xfff) < 0x400)
      WriteMap(g_bg_small[i], addr & 0x3ff, w, IntMin(count, 0x400 - (addr & 0x3ff)));
  } else if (addr < 0x6000) {
    assert((count & 15) == 0 && (addr & 15) == 0);
    if (addr >= 0x4000)
      Video_SpriteTiles((addr - 0x4000) >> 4, w, count >> 4);
    else
      Video_BgTiles((addr - 0x2000) >> 4, w, count >> 4);
  } else if (addr < 0x7000) {
    WriteMap(g_text_map, addr & 0xfff, w, count);
  } else {
    assert((count & 7) == 0 && (addr & 7) == 0);
    Video_TextTiles((addr - 0x7000) >> 3, w, count >> 3);
  }
}

void Video_WriteDown(uint32 addr, const void *words, int count) {
  const uint16 *w = (const uint16 *)words;
  for (int i = 0; i < count; i++, addr += 32)
    Video_WriteLocation(addr, &w[i], 1);
}

void Video_Fill(uint32 addr, uint16 value, int count, int step) {
  if (step == 1) {
    static uint16 row[32];
    while (count > 0) {
      int run = IntMin(count, 32 - (addr & 31));
      for (int i = 0; i < run; i++)
        row[i] = value;
      Video_WriteLocation(addr, row, run);
      addr += run, count -= run;
    }
  } else {
    for (; count > 0; count--, addr += step)
      Video_WriteLocation(addr, &value, 1);
  }
}

void Video_Mode7Texels(const uint8 *src) {
  memcpy(g_m7_texels, src, sizeof(g_m7_texels));
  g_m7_texels_dirty = true;
}

void Video_Mode7MapFill(uint8 tile) {
  memset(g_m7_cells, tile, sizeof(g_m7_cells));
  g_m7_cells_dirty = true;
}

void Video_Mode7MapBytes(uint32 addr, const uint8 *src, int count) {
  memcpy(&g_m7_cells[addr], src, count);
  g_m7_cells_dirty = true;
}

void Video_SetMaps(uint8 bg1, uint8 bg2) {
  g_bg1_map = bg1;
  g_bg2_map = bg2;
}

void Video_Palette(const uint16 *colours) {
  nshvc_palette_write(0, 256, colours);
}

void Video_Sprites(const void *table) {
  memcpy(g_oam, table, sizeof(g_oam));
}

void Video_LineValues(int slot, int target, const uint16 *one_per_line) {
  g_effects[slot] = (LineEffect){ target, one_per_line, NULL };
}

void Video_MapZoom(const uint16 *one_per_line) {
  Video_LineValues(0, kLines_Mode7A, one_per_line);
  Video_LineValues(1, kLines_Mode7D, one_per_line);
}

void Video_AttractWindows(void) {
  g_effects[0] = (LineEffect){ kLines_Window1, NULL, kAttractWindows1 };
  g_effects[1] = (LineEffect){ kLines_Window2, NULL, kAttractWindows2 };
}

void Video_EndingScroll(void) {
  static LineRun runs[7];
  runs[0] = (LineRun){ 0x52, 0, (uint16 *)&g_ram[0x600] };
  runs[1] = (LineRun){ 8, 0, &bg2_scroll_x2 };
  runs[2] = (LineRun){ 8, 0, (uint16 *)&g_ram[0x602] };
  runs[3] = (LineRun){ 5, 0, (uint16 *)&g_ram[0x604] };
  runs[4] = (LineRun){ 0x10, 0, (uint16 *)&g_ram[0x606] };
  runs[5] = (LineRun){ 1, 0, &bg2_scroll_x2 };
  g_effects[1] = (LineEffect){ kLines_Bg2Scroll, NULL, runs };
}

void Video_EndOfFrame(void) {
  const uint8 c[3] = { fixed_colour0, fixed_colour1, fixed_colour2 };
  for (int i = 0; i < 3; i++) {
    if (c[i] & 0x20) g_fixed[0] = c[i] & 31;
    if (c[i] & 0x40) g_fixed[1] = c[i] & 31;
    if (c[i] & 0x80) g_fixed[2] = c[i] & 31;
  }
  g_split_text = irq_flag != 0;
  g_split_text_scroll = selectfile_var8;
  if (irq_flag & 0x80)
    irq_flag = 0;
}

void Video_SideSpace(int left, int right) {
  g_show_left = g_extra - IntMin(left, g_extra);
  g_show_right = 256 + g_extra + IntMin(right, g_extra);
}

static int WindowUse(unsigned n) {
  return (n & 2 ? NSHVC_WIN1 : 0) | (n & 1 ? NSHVC_WIN1_INVERT : 0) | (n & 8 ? NSHVC_WIN2 : 0) | (n & 4 ? NSHVC_WIN2_INVERT : 0);
}

static void DrawSprites(void) {
  static nshvc_sprite s[128];
  for (int i = 0; i < 128; i++) {
    uint16 a = g_oam[i * 2], b = g_oam[i * 2 + 1];
    int hi = g_oam[0x100 + i / 8] >> ((i & 7) * 2) & 3;
    int x = (a & 0xff) | (hi & 1) << 8, y = a >> 8;
    if (x >= 256 + g_extra) x -= 512;
    if (y >= 224) y -= 256;
    s[i].x = x + g_extra;
    s[i].y = y;
    s[i].width = s[i].height = hi & 2 ? 2 : 1;
    s[i].tiles = g_obj_tiles;
    s[i].tile = b & 0x1ff;
    s[i].palette = b >> 9 & 7;
    s[i].priority = b >> 12 & 3;
    s[i].hflip = b >> 14 & 1;
    s[i].vflip = b >> 15 & 1;
  }
  nshvc_sprites_set(s, 128, 0);
}

static nshvc_map BgMap(int sc) {
  int which = (sc >> 4) & 1;
  return (sc & 3) ? g_bg_large[which] : g_bg_small[which];
}

static void DrawLayer(int n, nshvc_tiles t, nshvc_map m, int x, int y) {
  nshvc_layer l;
  memset(&l, 0, sizeof(l));
  l.enabled_main = layers_main >> n & 1;
  l.enabled_sub = layers_sub >> n & 1;
  l.tiles = t;
  l.map = m;
  l.tile_size = 8;
  l.scroll_x = x;
  l.scroll_y = y;
  l.mosaic = mosaic_setting >> n & 1 ? (mosaic_setting >> 4) + 1 : 1;
  l.wrap = 1;
  nshvc_layer_set(n, &l);
}

static void SetWindowEdge(int line, int edge, int v) {
  static const nshvc_line_field f[4] = { NSHVC_LINE_WIN1_LEFT, NSHVC_LINE_WIN1_RIGHT, NSHVC_LINE_WIN2_LEFT, NSHVC_LINE_WIN2_RIGHT };
  if (g_window[edge] != v) {
    g_window[edge] = v;
    nshvc_line(line, f[edge], v + g_extra);
  }
}

static void ApplyLineValue(int line, int target, uint16 v) {
  switch (target) {
  case kLines_Window1: SetWindowEdge(line, 0, v & 0xff); SetWindowEdge(line, 1, v >> 8); break;
  case kLines_Window2: SetWindowEdge(line, 2, v & 0xff); SetWindowEdge(line, 3, v >> 8); break;
  case kLines_Bg1Scroll: nshvc_line(line, NSHVC_LINE_SCROLL_X, v & 0x3ff); break;
  case kLines_Bg2Scroll: nshvc_line(line, NSHVC_LINE_SCROLL_X + 1, v & 0x3ff); break;
  case kLines_Mode7A: g_m7a = v; nshvc_line(line, NSHVC_LINE_M7_A, (int16)v); break;
  case kLines_Mode7D: g_m7d = v; nshvc_line(line, NSHVC_LINE_M7_D, (int16)v); break;
  }
}

static void DrawLineEffects(void) {
  for (int k = 0; k < 2; k++) {
    const LineEffect *e = &g_effects[k];
    if (!(line_effects_enabled & (0x40 << k)))
      continue;
    if (e->one_per_line) {
      for (int line = 0; line < 224; line++)
        ApplyLineValue(line, e->target, e->one_per_line[line]);
    } else {
      int line = 0;
      for (const LineRun *r = e->runs; r->lines && line < 224; r++) {
        ApplyLineValue(line, e->target, r->source ? *r->source : r->value);
        line += r->lines;
      }
    }
  }
}

void Video_Draw(void) {
  nshvc_screen scr;
  nshvc_windows w;
  nshvc_blend b;
  nshvc_mode7 m7;
  int mode = bg_mode & 7;

  memset(&scr, 0, sizeof(scr));
  scr.brightness = screen_setting & 15;
  scr.blank = screen_setting >> 7;
  if (g_extra) {
    nshvc_limits lim;
    scr.width = 256 + 2 * g_extra;
    scr.origin_x = g_extra;
    scr.aspect = g_aspect;
    scr.show_left = g_show_left;
    scr.show_right = g_show_right;
    memset(&lim, 0, sizeof(lim));
    lim.layer_clip[2] = 1;  // the status bar and text stay in the original's columns
    nshvc_limits_set(&lim);
  }
  nshvc_screen_set(&scr);
  nshvc_mode_set(mode, bg_mode >> 3 & 1, 0);

  if (g_m7_texels_dirty) {
    nshvc_tiles_write(g_m7_tiles, 0, g_m7_texels, 256, NSHVC_TILES_MODE7);
    g_m7_texels_dirty = false;
  }
  if (g_m7_cells_dirty) {
    static uint16 cells[0x4000];
    for (int i = 0; i < 0x4000; i++)
      cells[i] = g_m7_cells[i];
    nshvc_map_write_snes(g_m7_map, 0, 0, 128, 128, cells);
    g_m7_cells_dirty = false;
  }

  if (mode == 7) {
    memset(&m7, 0, sizeof(m7));
    m7.a = g_m7a;
    m7.d = g_m7d;
    m7.centre_x = m7_centre_x;
    m7.centre_y = m7_centre_y;
    m7.scroll_x = bg1_scroll_x & 0x1fff;
    m7.scroll_y = bg1_scroll_y & 0x1fff;
    m7.outside = 1;
    m7.tiles = g_m7_tiles;
    m7.map = g_m7_map;
    nshvc_mode7_set(&m7);
    DrawLayer(0, g_bg_tiles, g_m7_map, bg1_scroll_x, bg1_scroll_y);
  } else {
    DrawLayer(0, g_bg_tiles, BgMap(g_bg1_map), bg1_scroll_x, bg1_scroll_y);
    DrawLayer(1, g_bg_tiles, BgMap(g_bg2_map), bg2_scroll_x, bg2_scroll_y);
    DrawLayer(2, g_text_tiles, g_text_map, bg3_scroll_x2, bg3_scroll_y2);
  }

  nshvc_sprites_show(layers_main >> 4 & 1, layers_sub >> 4 & 1);
  DrawSprites();

  memset(&w, 0, sizeof(w));
  w.w[0].left = g_window[0] + g_extra;
  w.w[0].right = g_window[1] + g_extra;
  w.w[1].left = g_window[2] + g_extra;
  w.w[1].right = g_window[3] + g_extra;
  w.layer_use[0] = WindowUse(window_use_bg12 & 15);
  w.layer_use[1] = WindowUse(window_use_bg12 >> 4);
  w.layer_use[2] = WindowUse(window_use_bg34 & 15);
  w.layer_use[3] = WindowUse(window_use_bg34 >> 4);
  w.layer_use[4] = WindowUse(window_use_obj_colour & 15);
  w.layer_use[5] = WindowUse(window_use_obj_colour >> 4);
  for (int i = 0; i < 5; i++) {
    w.mask_main[i] = window_mask_main >> i & 1;
    w.mask_sub[i] = window_mask_sub >> i & 1;
  }
  nshvc_windows_set(&w);

  memset(&b, 0, sizeof(b));
  b.subtract = blend_setting >> 7 & 1;
  b.half = blend_setting >> 6 & 1;
  b.layers = blend_setting & 63;
  b.use_fixed = !(blend_select & 2);
  b.prevent = blend_select >> 4 & 3;
  b.clip = blend_select >> 6 & 3;
  b.fixed_bgr15 = g_fixed[0] | g_fixed[1] << 5 | g_fixed[2] << 10;
  nshvc_blend_set(&b);

  DrawLineEffects();
  if (g_split_text) {
    nshvc_line(127, NSHVC_LINE_SCROLL_X + 2, g_split_text_scroll);
    nshvc_line(127, NSHVC_LINE_SCROLL_Y + 2, 0);
  }
}
