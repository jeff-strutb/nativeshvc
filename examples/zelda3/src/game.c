// game.c: the game, on nativeshvc.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nshvc/nshvc.h"
#include "types.h"
#include "zelda_rtl.h"
#include "assets.h"
#include "audio.h"
#include "video.h"
#include "variables.h"
#include "features.h"
#include "util.h"
#include "controls.h"

const uint8 *g_asset_ptrs[kNumberOfAssets];
uint32 g_asset_sizes[kNumberOfAssets];
extern uint32 g_wanted_zelda_features;

static int g_extra_columns;
static float g_shape;

void Die(const char *error) {
  fprintf(stderr, "Error: %s\n", error);
  exit(1);
}

MemBlk FindInAssetArray(int asset, int idx) {
  return FindIndexInMemblk((MemBlk){ g_asset_ptrs[asset], g_asset_sizes[asset] }, idx);
}

static void LoadAssets(void) {
  static const char kSignature[] = { kAssets_Sig };
  nshvc_asset a = nshvc_asset_open("zelda3_assets.dat");
  size_t n = 0, cap = 4u << 20;
  uint8 *d = malloc(cap);
  uint32 got;
  if (!a.id || !d)
    Die("zelda3_assets.dat is not in the game's package");
  while ((got = nshvc_asset_read(a, d + n, (uint32)(cap - n))) != 0) {
    n += got;
    if (n == cap && !(d = realloc(d, cap *= 2)))
      Die("out of memory");
  }
  nshvc_asset_close(a);
  if (n < 88 || memcmp(d, kSignature, 48) || *(uint32 *)(d + 80) != kNumberOfAssets)
    Die("zelda3_assets.dat is not valid");
  uint32 offset = 88 + kNumberOfAssets * 4 + *(uint32 *)(d + 84);
  for (uint32 i = 0; i < kNumberOfAssets; i++) {
    uint32 size = *(uint32 *)(d + 88 + i * 4);
    offset = (offset + 3) & ~3u;
    g_asset_sizes[i] = size;
    g_asset_ptrs[i] = d + offset;
    offset += size;
  }
}

static void Init(void) {
  LoadAssets();
  ZeldaInitialize();
  ZeldaSetLanguage(NULL);
  ZeldaReadSram();
  Video_Init(g_extra_columns, g_shape);
  if (g_extra_columns)
    g_wanted_zelda_features = kFeatures0_ExtendScreen64 | kFeatures0_WidescreenVisualFixes;
}

// The devices, as this game's joypad word (controls.c)
static void Update(const nshvc_input *in) {
  ZeldaRunFrame(Controls_Read(in));
}

static void Draw(float between) {
  (void)between;
  ZeldaDraw();
  Controls_Draw();
}

#ifndef ZELDA3_CHECK_BUILD
int main(int argc, char **argv) {
  static nshvc_game game;
  const char *wide = getenv("ZELDA3_ASPECT");
  int a, b;
  if (wide && sscanf(wide, "%d:%d", &a, &b) == 2 && a * 3 > b * 4) {
    g_extra_columns = IntMin((224 * a * 6 / (b * 7) - 256 + 1) / 2, 96);
    g_shape = (float)a / (float)b;
    game.window_aspect = g_shape;
  }
  game.title = "The Legend of Zelda: A Link to the Past";
  game.id = "zelda3";
  game.update_hz = 60;
  game.init = Init;
  game.update = Update;
  game.draw = Draw;
  game.music.hz = 32040;
  game.music.tick = Audio_Tick;
  game.music.queue_bytes = 4096;
  return nshvc_run(&game, argc, argv);
}
#endif
