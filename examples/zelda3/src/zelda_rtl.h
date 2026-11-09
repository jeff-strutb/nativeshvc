// This file defines various things related to the runtime environment
// of the code
#ifndef ZELDA3_ZELDA_RTL_H_
#define ZELDA3_ZELDA_RTL_H_

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#include "types.h"
#include "features.h"


typedef struct ZeldaEnv {
  uint8 *ram;
  uint8 *sram;
  
  MemBlk dialogue_blk;
  MemBlk dialogue_font_blk;
  uint8 dialogue_flags;
} ZeldaEnv;
extern ZeldaEnv g_zenv;

typedef void PlayerHandlerFunc();
typedef void HandlerFuncK(int k);


void Music_Command(uint32_t adr, uint8_t val);
uint8_t Music_IsStarted();
uint8_t Music_Status(uint32_t adr);


// 512x480 32-bit pixels. Returns true if we instead draw 1024x960

void ZeldaInitialize();
void ZeldaDraw(void);
void ZeldaRunFrameInternal(uint16 input, int run_what);
void ZeldaRunFrame(int input_state);
void LoadSongBank(const uint8 *p);
bool ZeldaIsPlayingMusicTrack(uint8 track);
uint8 ZeldaGetEntranceMusicTrack(int track);
void ZeldaSetLanguage(const char *language);

// Things for state management

enum {
  kSaveLoad_Save = 0,
  kSaveLoad_Load = 1,
  kSaveLoad_Replay = 2,
};

void ZeldaWriteSram();
void ZeldaReadSram();



// Button definitions, zelda splits them in separate 8-bit high/low
enum {
  kJoypadL_A = 0x80,
  kJoypadL_X = 0x40,
  kJoypadL_L = 0x20,
  kJoypadL_R = 0x10,

  kJoypadH_B = 0x80,
  kJoypadH_Y = 0x40,
  kJoypadH_Select = 0x20,
  kJoypadH_Start = 0x10,

  kJoypadH_Up = 0x8,
  kJoypadH_Down = 0x4,
  kJoypadH_Left = 0x2,
  kJoypadH_Right = 0x1,

  kJoypadH_AnyDir = 0xf,
};

#endif  // ZELDA3_ZELDA_RTL_H_
