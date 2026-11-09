#include "nmi.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "messaging.h"
#include "video.h"
#include "assets.h"
#include "audio.h"

static const uint8 kNmiVramAddrs[] = {
  0, 0, 4, 8, 12, 8, 12, 0, 4, 0, 8, 4, 12, 4, 12, 0,
  8, 16, 20, 24, 28, 24, 28, 16, 20, 16, 24, 20, 28, 20, 28, 16,
  24, 96, 104,
};
static PlayerHandlerFunc *const kNmiSubroutines[25] = {
  &NMI_UploadTilemap_doNothing,
  &NMI_UploadTilemap,
  &NMI_UploadBG3Text,
  &NMI_UpdateOWScroll,
  &NMI_UpdateSubscreenOverlay,
  &NMI_UpdateBG1Wall,
  &NMI_TileMapNothing,
  &NMI_UpdateLoadLightWorldMap,
  &NMI_UpdateBG2Left,
  &NMI_UpdateBGChar3and4,
  &NMI_UpdateBGChar5and6,
  &NMI_UpdateBGCharHalf,
  &NMI_UploadSubscreenOverlayLatter,
  &NMI_UploadSubscreenOverlayFormer,
  &NMI_UpdateBGChar0,
  &NMI_UpdateBGChar1,
  &NMI_UpdateBGChar2,
  &NMI_UpdateBGChar3,
  &NMI_UpdateObjChar0,
  &NMI_UpdateObjChar2,
  &NMI_UpdateObjChar3,
  &NMI_UploadDarkWorldMap,
  &NMI_UploadGameOverText,
  &NMI_UpdatePegTiles,
  &NMI_UpdateStarTiles,
};
void NMI_UploadSubscreenOverlayFormer() {
  NMI_HandleArbitraryTileMap(&g_ram[0x12000], 0, 0x40);
}

void NMI_UploadSubscreenOverlayLatter() {
  NMI_HandleArbitraryTileMap(&g_ram[0x13000], 0x40, 0x80);
}

static void CopyToVram(uint32 dstv, const uint8 *src, int len) {
  Video_WriteLocation(dstv, src, len / 2);
}

static void CopyToVramVertical(uint32 dstv, const uint8 *src, int len) {
  assert(!(len & 1));
  Video_WriteDown(dstv, src, len >> 1);
}

static void Frame_End_AudioParts_Locked() {
  if (music_control == 0) {
//    if (Music_Status(kMusicSlot0) == last_music_control)
//      Music_Command(kMusicSlot0, 0);
    // Zelda causes unwanted music change when going in a portal. last_music_control doesn't hold the 
    // song but the last applied effect
  } else if (!ZeldaIsPlayingMusicTrackWithBug(music_control)) {
    last_music_control = music_control;
    Music_Command(kMusicSlot0, music_control);
    if (music_control < 0xf2)
      music_unk1 = music_control;
    music_control = 0;
  }

  if (sound_effect_ambient == 0) {
    if (Music_Status(kMusicSlot1) == sound_effect_ambient_last)
      Music_Command(kMusicSlot1, 0);
  } else {
    sound_effect_ambient_last = sound_effect_ambient;
    Music_Command(kMusicSlot1, sound_effect_ambient);
    sound_effect_ambient = 0;
  }
  Music_Command(kMusicSlot2, sound_effect_1);
  Music_Command(kMusicSlot3, sound_effect_2);
  sound_effect_1 = 0;
  sound_effect_2 = 0;

}

void Frame_End(uint16 joypad_input) {  // 8080c9

  Frame_End_AudioParts_Locked();

  if (!nmi_boolean) {
    nmi_boolean = true;
    NMI_DoUpdates();
    NMI_ReadJoypads(joypad_input);
  }

  if (is_nmi_thread_active) {
    NMI_UpdateIRQGFX();
    thread_other_stack = (thread_other_stack != 0x1f31) ? 0x1f31 : 0x1f2;
  }
  Video_EndOfFrame();
}

void NMI_ReadJoypads(uint16 joypad_input) {  // 8083d1
  uint16 both = joypad_input;
  uint16 reversed = 0;
  for (int i = 0; i < 16; i++, both >>= 1)
    reversed = reversed * 2 + (both & 1);
  uint8 r0 = reversed;
  uint8 r1 = reversed >> 8;

  joypad1L_last = r0;
  filtered_joypad_L = (r0 ^ joypad1L_last2) & r0;
  joypad1L_last2 = r0;

  joypad1H_last = r1;
  filtered_joypad_H = (r1 ^ joypad1H_last2) & r1;
  joypad1H_last2 = r1;
}

void NMI_DoUpdates() {  // 8089e0
  if (!nmi_disable_core_updates) {
    Video_SpriteTiles(0x10, &kLinkGraphics[dma_source_addr_0 - 0x8000], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x12, &kLinkGraphics[dma_source_addr_1 - 0x8000], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x14, &kLinkGraphics[dma_source_addr_2 - 0x8000], ((0x20) / 2) >> 4);

    Video_SpriteTiles(0x0, &kLinkGraphics[dma_source_addr_3 - 0x8000], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x2, &kLinkGraphics[dma_source_addr_4 - 0x8000], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x4, &kLinkGraphics[dma_source_addr_5 - 0x8000], ((0x20) / 2) >> 4);

    Video_SpriteTiles(0x5, &g_ram[dma_source_addr_6], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x7, &g_ram[dma_source_addr_7], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x9, &g_ram[dma_source_addr_8], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0xb, &g_ram[dma_source_addr_9], ((0x20) / 2) >> 4);
    Video_SpriteTiles(0xc, &g_ram[dma_source_addr_10], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x15, &g_ram[dma_source_addr_11], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x17, &g_ram[dma_source_addr_12], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x19, &g_ram[dma_source_addr_13], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x1b, &g_ram[dma_source_addr_14], ((0x20) / 2) >> 4);
    Video_SpriteTiles(0x1c, &g_ram[dma_source_addr_15], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x20, &g_ram[dma_source_addr_16], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x22, &g_ram[dma_source_addr_17], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x24, &g_ram[0xbd40], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x30, &g_ram[dma_source_addr_18], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x32, &g_ram[dma_source_addr_19], ((0x40) / 2) >> 4);
    Video_SpriteTiles(0x34, &g_ram[0xbd80], ((0x40) / 2) >> 4);

    if (BYTE(flag_travel_bird)) {
      Video_SpriteTiles(0xe, &g_ram[dma_source_addr_20], ((0x40) / 2) >> 4);
      Video_SpriteTiles(0x1e, &g_ram[dma_source_addr_21], ((0x40) / 2) >> 4);
    }

    Video_WriteLocation(animated_tile_vram_addr, &g_ram[animated_tile_data_src], (0x400) / 2);
  }

  if (flag_update_hud_in_nmi) {
    Video_WriteLocation(word_7E0219, hud_tile_indices_buffer, (165 * sizeof(uint16)) / 2);
  }

  if (flag_update_cgram_in_nmi) {
    Video_Palette(main_palette_buffer);
  }

  flag_update_hud_in_nmi = 0;
  flag_update_cgram_in_nmi = 0;

  Video_Sprites(&g_ram[0x800]);

  if (nmi_load_bg_from_vram) {
    const uint8 *p;
    switch (nmi_load_bg_from_vram) {
    case 1: p = g_ram + 0x1002; break;
    case 2: p = g_ram + 0x1000; break;
    case 3: p = kBgTilemap_0; break;
    case 4: p = g_ram + 0x21b; break;
    case 5: p = kBgTilemap_1; break;
    case 6: p = kBgTilemap_2; break;
    case 7: p = kBgTilemap_3; break;
    case 8: p = kBgTilemap_4; break;
    case 9: p = kBgTilemap_5; break;
    default: assert(0);
    }
    HandleStripes14(p);
    if (nmi_load_bg_from_vram == 1)
      vram_upload_offset = 0;
    nmi_load_bg_from_vram = 0;
  }

  if (nmi_update_tilemap_dst) {
    Video_WriteLocation(nmi_update_tilemap_dst * 256, &g_ram[0x10000 + nmi_update_tilemap_src], (0x200) / 2);
    nmi_update_tilemap_dst = 0;
  }

  if (nmi_copy_packets_flag) {
    uint8 *p = (uint8 *)uvram.data;
    do {
      int dst = WORD(p[0]);
      int vmain = p[2];
      int len = p[3];
      p += 4;
      if (vmain == 0x80) {
        // plain copy
        Video_WriteLocation(dst, p, (len) / 2);
      } else if (vmain == 0x81) {
        // copy with other increment
        assert((len & 1) == 0);
        Video_WriteDown(dst, p, len / 2);
      } else {
        assert(0);
      }
      p += len;
    } while (WORD(p[0]) != 0xffff);
    nmi_copy_packets_flag = 0;
    nmi_disable_core_updates = 0;
  }

  int idx = nmi_subroutine_index;
  nmi_subroutine_index = 0;
  kNmiSubroutines[idx]();
}

void NMI_UploadTilemap() {  // 808cb0
  Video_WriteLocation(kNmiVramAddrs[BYTE(nmi_load_target_addr)] << 8, &g_ram[0x1000], (0x800) / 2);

  *(uint16 *)&g_ram[0x1000] = 0;
  nmi_disable_core_updates = 0;
}

void NMI_UploadTilemap_doNothing() {  // 808ce3
}

void NMI_UploadBG3Text() {  // 808ce4
  Video_TextTiles(0x180, &g_ram[0x10000], ((0x7e0) / 2) >> 3);
  nmi_disable_core_updates = 0;
}

void NMI_UpdateOWScroll() {  // 808d13
  uint8 *src = (uint8 *)uvram.data;
  int f = WORD(src[0]);
  int step = (f & 0x8000) ? 32 : 1;
  int len = f & 0x3fff;
  src += 2;
  do {
    uint32 dst = WORD(src[0]);
    src += 2;
    if (step == 1)
      Video_WriteLocation(dst, src, len >> 1);
    else
      Video_WriteDown(dst, src, len >> 1);
    src += len & ~1;
  } while (!(src[1] & 0x80));
  nmi_disable_core_updates = 0;
}

void NMI_UpdateSubscreenOverlay() {  // 808d62
  NMI_HandleArbitraryTileMap(&g_ram[0x12000], 0, 0x80);
}

void NMI_HandleArbitraryTileMap(const uint8 *src, int i, int i_end) {  // 808dae
  uint16 *r10 = &word_7F4000;
  do {
    Video_WriteLocation(r10[i >> 1], src, (0x80) / 2);
    src += 0x80;
  } while ((i += 2) != i_end);
  nmi_disable_core_updates = 0;
}

void NMI_UpdateBG1Wall() {  // 808e09
  // Secret Wall Right
  CopyToVramVertical(nmi_load_target_addr, &g_ram[0xc880], 0x40);
  CopyToVramVertical(nmi_load_target_addr + 0x800, &g_ram[0xc8c0], 0x40);
}

void NMI_TileMapNothing() {  // 808e4b
}

void NMI_UpdateLoadLightWorldMap() {  // 808e54
  static const uint16 kLightWorldTileMapDsts[4] = { 0, 0x20, 0x1000, 0x1020 };
  const uint8 *src = GetLightOverworldTilemap();
  for (int j = 0; j != 4; j++) {
    int t = kLightWorldTileMapDsts[j];
    for (int i = 0x20; i; i--) {
      Video_Mode7MapBytes(t, src, 0x20);
      src += 32;
      t += 0x80;
    }
  }
}

void NMI_UpdateBG2Left() {  // 808ea9
  CopyToVram(0, &g_ram[0x10000], 0x800);
  CopyToVram(0x800, &g_ram[0x10800], 0x800);
}

void NMI_UpdateBGChar3and4() {  // 808ee7
  Video_BgTiles(0xc0, &g_ram[0x10000], ((0x1000) / 2) >> 4);
  nmi_disable_core_updates = 0;
}

void NMI_UpdateBGChar5and6() {  // 808f16
  Video_BgTiles(0x140, &g_ram[0x11000], ((0x1000) / 2) >> 4);
  nmi_disable_core_updates = 0;
}

void NMI_UpdateBGCharHalf() {  // 808f45
  Video_WriteLocation(BYTE(nmi_load_target_addr) * 256, &g_ram[0x11000], (0x400) / 2);
}

void NMI_UpdateBGChar0() {  // 808f72
  NMI_RunTileMapUpdateDMA(0x2000);
}

void NMI_UpdateBGChar1() {  // 808f79
  NMI_RunTileMapUpdateDMA(0x2800);
}

void NMI_UpdateBGChar2() {  // 808f80
  NMI_RunTileMapUpdateDMA(0x3000);
}

void NMI_UpdateBGChar3() {  // 808f87
  NMI_RunTileMapUpdateDMA(0x3800);
}

void NMI_UpdateObjChar0() {  // 808f8e
  Video_SpriteTiles(0x40, &g_ram[0x10000], ((0x800) / 2) >> 4);
  nmi_disable_core_updates = 0;
}

void NMI_UpdateObjChar2() {  // 808fbd
  NMI_RunTileMapUpdateDMA(0x5000);
}

void NMI_UpdateObjChar3() {  // 808fc4
  NMI_RunTileMapUpdateDMA(0x5800);
}

void NMI_RunTileMapUpdateDMA(int dst) {  // 808fc9
  Video_WriteLocation(dst, &g_ram[0x10000], 0x800);
  nmi_disable_core_updates = 0;
}

void NMI_UploadDarkWorldMap() {  // 808ff3
  const uint8 *src = g_ram + 0x1000;
  int t = 0x810;
  for (int i = 0x20; i; i--) {
    Video_Mode7MapBytes(t, src, 0x20);
    src += 32;
    t += 0x80;
  }
}

void NMI_UploadGameOverText() {  // 809038
  Video_TextTiles(0x100, &g_ram[0x2000], ((0x800) / 2) >> 3);
  Video_TextTiles(0x1a0, &g_ram[0x3400], ((0x600) / 2) >> 3);
}

void NMI_UpdatePegTiles() {  // 80908b
  Video_BgTiles(0x1d0, &g_ram[0x10000], ((0x100) / 2) >> 4);
}

void NMI_UpdateStarTiles() {  // 8090b7
  Video_BgTiles(0x1ed, &g_ram[0x10000], ((0x40) / 2) >> 4);
}

void HandleStripes14(const uint8 *p) {  // 8092a1
  while (!(p[0] & 0x80)) {
    uint16 vmem_addr = swap16(WORD(p[0]));
    uint8 vram_incr_amount = (p[2] & 0x80) >> 7;
    uint8 is_memset = p[2] & 0x40;  // Cpu BUS Address Step  (0=Increment, 2=Decrement, 1/3=Fixed) (DMA only)
    int len = (swap16(WORD(p[2])) & 0x3fff) + 1;
    p += 4;

    int step = vram_incr_amount ? 32 : 1;
    if (is_memset) {
      Video_Fill(vmem_addr, p[0] | p[1] << 8, (len + 1) >> 1, step);
      p += 2;
    } else if (step == 1) {
      Video_WriteLocation(vmem_addr, p, len >> 1);
      p += len;
    } else {
      Video_WriteDown(vmem_addr, p, len >> 1);
      p += len;
    }
  }
}

void NMI_UpdateIRQGFX() {  // 809347
  if (nmi_flag_update_polyhedral) {
    Video_SpriteTiles(0x180, &g_ram[0xe800], ((0x800) / 2) >> 4);
    nmi_flag_update_polyhedral = 0;
  }
}

