/*
 * window_columns.c: a window's edges as the screen's columns
 * (nshvc_ext_window_columns), on window_wide.h's cartridge, on a 400 x 240
 * screen (the console's picture at columns 72-327, its line 1 at row 8);
 * each scene's spans worked out by hand:
 *
 *   0  window 1 at 50-300 on BG1, WH0-WH1 10-20 (not used): 50-300
 *   1  0-399 inverted: none
 *   2  300-50: none
 *   3  given 100-200 and given back: the registers' 10-20, 82-92
 *   4  window 1 by its registers 64-127 stretched (nshvc_ext_window_map
 *      400 * 256, 0: 100-199) OR window 2 at 250-260, its registers 0-255 (not used):
 *      100-199, 250-260
 *   5  window 1 at 5-10, its registers 0-255 with nshvc_ext_window_edges
 *      (not used): 5-10
 *   6  window 2 at 0-99 over the sprite (columns 72-135): the sprite shown
 *      at 100-135
 *   7  the same inverted: 100-399 masked, the sprite shown at 72-99
 *   8  the colour window by window 1 at 390-8191, past the screen's
 *      right: 390-399 clipped to black
 *   9  window 1 at 150-160 while HDMA writes WH0 and WH1 each line: 150-160
 *      on every row
 *
 * Exit 0 when every pixel holds. With arguments, the scenes run through the
 * front end (nshvc_main) for NSHVC_COMPOSECHECK=1 and NSHVC_RRCHECK=1;
 * --bad: a window not 1 or 2 given columns, fatal (exit 70); --badmap: a map of
 * scale 0, fatal.
 */
#include "window_wide.h"

#define S(...) { 400, 240, __VA_ARGS__ }
#define FULL (400 * 256)   /* the map across a 400-wide screen */
static const Scene sc[] = {
    S(.target = T_BG, .wh = { 10, 20 }, .sel = 0x02, .cn = 1, .cols = { { 50, 300 } }, .ns = 1,
      .spans = { { 50, 300 } }),
    S(.target = T_BG, .sel = 0x03, .cn = 1, .cols = { { 0, 399 } }, .ns = 0),
    S(.target = T_BG, .sel = 0x02, .cn = 1, .cols = { { 300, 50 } }, .ns = 0),
    S(.target = T_BG, .wh = { 10, 20 }, .sel = 0x02, .release = 1, .ns = 1, .spans = { { 82, 92 } }),
    S(.target = T_BG, .map = 1, .mq = FULL, .wh = { 64, 127, 0, 255 }, .sel = 0x0A, .logic = 0, .cn = 2,
      .cols = { { 0, 0 }, { 250, 260 } }, .ns = 2, .spans = { { 100, 199 }, { 250, 260 } }),
    S(.target = T_BG, .edges = 1, .wh = { 0, 255 }, .sel = 0x02, .cn = 1, .cols = { { 5, 10 } }, .ns = 1,
      .spans = { { 5, 10 } }),
    S(.target = T_OBJ, .wh = { 0, 0, 0, 255 }, .sel = 0x08, .cn = 2, .cols = { { 0, 0 }, { 0, 99 } }, .ns = 1,
      .spans = { { 0, 99 } }),
    S(.target = T_OBJ, .wh = { 0, 0, 0, 255 }, .sel = 0x0C, .cn = 2, .cols = { { 0, 0 }, { 0, 99 } }, .ns = 1,
      .spans = { { 100, 399 } }),
    S(.target = T_COLOR, .sel = 0x02, .cn = 1, .cols = { { 390, 8191 } }, .ns = 1, .spans = { { 390, 399 } }),
    S(.target = T_BG, .sel = 0x02, .hdma = 1, .cn = 1, .cols = { { 150, 160 } }, .ns = 1, .spans = { { 150, 160 } }),
};

int main(int argc, char **argv)
{
    return ww_main("window_columns", sc, (int)(sizeof sc / sizeof sc[0]), argc, argv);
}
