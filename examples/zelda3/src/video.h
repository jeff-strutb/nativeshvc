// The game's picture, on nativeshvc. Graphics are written to engine objects as
// they change; the picture is described once per draw.
#ifndef ZELDA3_VIDEO_H_
#define ZELDA3_VIDEO_H_

#include "types.h"

// What a per-line effect changes
enum { kLines_Window1, kLines_Window2, kLines_Bg1Scroll, kLines_Bg2Scroll, kLines_Mode7A, kLines_Mode7D };

void Video_Init(int extra_columns, float aspect);

void Video_BgTiles(int first, const void *planar, int count);
void Video_SpriteTiles(int first, const void *planar, int count);
void Video_TextTiles(int first, const void *planar, int count);
// A destination the game's data names by number: a map cell or a tile.
void Video_WriteLocation(uint32 where, const void *words, int count);
void Video_WriteDown(uint32 where, const void *words, int count);  // a tilemap column
void Video_Fill(uint32 where, uint16 value, int count, int step);

void Video_Mode7Texels(const uint8 *src);
void Video_Mode7MapFill(uint8 tile);
void Video_Mode7MapBytes(uint32 addr, const uint8 *src, int count);

void Video_SetMaps(uint8 bg1, uint8 bg2);
void Video_Palette(const uint16 *colours);
void Video_Sprites(const void *table);
// Slots 0 and 1, turned on by bits 6 and 7 of line_effects_enabled
void Video_LineValues(int slot, int target, const uint16 *one_per_line);
void Video_MapZoom(const uint16 *one_per_line);
void Video_AttractWindows(void);
void Video_EndingScroll(void);
void Video_EndOfFrame(void);
void Video_SideSpace(int left, int right);
void Video_Draw(void);

#endif  // ZELDA3_VIDEO_H_
