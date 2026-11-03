/*
 * window_map.c: the windows' edges mapped onto a larger screen
 * (nshvc_ext_window_map), on window_wide.h's cartridge. Console column c
 * maps to c * q / 65536, rounded down, + o, and a window from L to R
 * covers L's column up to the one before R + 1's; on a 400 x 240 screen
 * (the console's picture at columns 72-327, its line 1 at row 8) the map
 * across the screen is (102400, 0). Each scene's spans worked out by hand:
 *
 *   0  off, window 1 64-127 on BG1: the console's, 136-199
 *   1  across, the same: 100-199
 *   2  across, 0-255: 0-399
 *   3  across, 200-100: none
 *   4  across, 0-255 inverted: none
 *   5  across, window 1 32-95 (50-149) XOR window 2 64-191 (100-299)
 *      inverted: 0-49, 100-149, 300-399
 *   6  across with nshvc_ext_window_edges, 0-63: the map's, 0-99
 *   7  the edges alone, 0-63: open to the left, 0-135
 *   8  across, WH0 and WH1 by HDMA each line: line L L - 1 to L + 30
 *   9  across, window 1 40-79 over the sprite (columns 72-135): 62-124
 *      masked, the sprite shown at 125-135
 *  10  off, the same: 112-151 masked, the sprite shown at 72-111
 *  11  across, the colour window (window 1, 64-127) clipping to black:
 *      100-199
 *  12  across, the colour window by window 2, 128-255: 200-399
 *  13  (65536, 0) on a 256 x 224 screen, 64-127: the console's, 64-127
 *  14  across a 426 x 240 screen (109056, 0), 13-200: 5538 / 256 = 21 to
 *      85626 / 256 - 1 = 333
 *  15  (65536, 72), 64-127: the console's, 136-199
 *  16  (98304, 10) (1.5), 64-127: 96 + 10 = 106 to 192 + 10 - 1 = 201
 *  17  (100000, 7), 13-200: 1300000 / 65536 = 19, + 7 = 26, to
 *      20100000 / 65536 = 306, + 7 - 1 = 312
 *  18  (131072, -100) (2), 40-79: -20 to 59, on the screen 0-59
 *  19  (40000000, -73100), 120-120 (products past 32 bits, unsigned or
 *      not): 4800000000 / 65536 = 73242, - 73100 = 142, to 4840000000 /
 *      65536 = 73852, - 73100 - 1 = 751, on the screen 142-399
 *  20  (98304, 10), WH0 and WH1 by HDMA each line
 *  21  (81920, 20) (1.25), 56-216: 70 + 20 = 90 to 271 + 20 - 1 = 290
 *  22  (98304, 10), window 1 40-79 over the sprite: 70-129 masked, the
 *      sprite shown at 130-135
 *
 * Exit 0 when every pixel holds. With arguments, the scenes run through the
 * front end (nshvc_main) for NSHVC_COMPOSECHECK=1 and NSHVC_RRCHECK=1;
 * --badmap: a map of scale 0, fatal (exit 70).
 */
#include "window_wide.h"

#define S(...) { 400, 240, __VA_ARGS__ }
#define ACROSS .map = 1, .mq = 400 * 256
static const Scene sc[] = {
    S(.target = T_BG, .wh = { 64, 127 }, .sel = 0x02, .ns = 1, .spans = { { 136, 199 } }),
    S(.target = T_BG, ACROSS, .wh = { 64, 127 }, .sel = 0x02, .ns = 1, .spans = { { 100, 199 } }),
    S(.target = T_BG, ACROSS, .wh = { 0, 255 }, .sel = 0x02, .ns = 1, .spans = { { 0, 399 } }),
    S(.target = T_BG, ACROSS, .wh = { 200, 100 }, .sel = 0x02, .ns = 0),
    S(.target = T_BG, ACROSS, .wh = { 0, 255 }, .sel = 0x03, .ns = 0),
    S(.target = T_BG, ACROSS, .wh = { 32, 95, 64, 191 }, .sel = 0x0E, .logic = 2, .ns = 3,
      .spans = { { 0, 49 }, { 100, 149 }, { 300, 399 } }),
    S(.target = T_BG, ACROSS, .edges = 1, .wh = { 0, 63 }, .sel = 0x02, .ns = 1, .spans = { { 0, 99 } }),
    S(.target = T_BG, .edges = 1, .wh = { 0, 63 }, .sel = 0x02, .ns = 1, .spans = { { 0, 135 } }),
    S(.target = T_BG, ACROSS, .sel = 0x02, .hdma = 1),
    S(.target = T_OBJ, ACROSS, .wh = { 40, 79 }, .sel = 0x02, .ns = 1, .spans = { { 62, 124 } }),
    S(.target = T_OBJ, .wh = { 40, 79 }, .sel = 0x02, .ns = 1, .spans = { { 112, 151 } }),
    S(.target = T_COLOR, ACROSS, .wh = { 64, 127 }, .sel = 0x02, .ns = 1, .spans = { { 100, 199 } }),
    S(.target = T_COLOR, ACROSS, .wh = { 0, 0, 128, 255 }, .sel = 0x08, .ns = 1, .spans = { { 200, 399 } }),
    { 256, 224, .target = T_BG, .map = 1, .mq = 65536, .wh = { 64, 127 }, .sel = 0x02, .ns = 1, .spans = { { 64, 127 } } },
    { 426, 240, .target = T_BG, .map = 1, .mq = 426 * 256, .wh = { 13, 200 }, .sel = 0x02, .ns = 1,
      .spans = { { 21, 333 } } },
    S(.target = T_BG, .map = 1, .mq = 65536, .mo = 72, .wh = { 64, 127 }, .sel = 0x02, .ns = 1, .spans = { { 136, 199 } }),
    S(.target = T_BG, .map = 1, .mq = 98304, .mo = 10, .wh = { 64, 127 }, .sel = 0x02, .ns = 1, .spans = { { 106, 201 } }),
    S(.target = T_BG, .map = 1, .mq = 100000, .mo = 7, .wh = { 13, 200 }, .sel = 0x02, .ns = 1, .spans = { { 26, 312 } }),
    S(.target = T_BG, .map = 1, .mq = 131072, .mo = -100, .wh = { 40, 79 }, .sel = 0x02, .ns = 1, .spans = { { 0, 59 } }),
    S(.target = T_BG, .map = 1, .mq = 40000000, .mo = -73100, .wh = { 120, 120 }, .sel = 0x02, .ns = 1,
      .spans = { { 142, 399 } }),
    S(.target = T_BG, .map = 1, .mq = 98304, .mo = 10, .sel = 0x02, .hdma = 1),
    S(.target = T_BG, .map = 1, .mq = 81920, .mo = 20, .wh = { 56, 216 }, .sel = 0x02, .ns = 1, .spans = { { 90, 290 } }),
    S(.target = T_OBJ, .map = 1, .mq = 98304, .mo = 10, .wh = { 40, 79 }, .sel = 0x02, .ns = 1, .spans = { { 70, 129 } }),
};

int main(int argc, char **argv)
{
    return ww_main("window_map", sc, (int)(sizeof sc / sizeof sc[0]), argc, argv);
}
