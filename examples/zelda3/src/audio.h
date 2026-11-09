#ifndef ZELDA3_AUDIO_H_
#define ZELDA3_AUDIO_H_
#include "types.h"

enum { kMusicSlot0 = 0, kMusicSlot1 = 1, kMusicSlot2 = 2, kMusicSlot3 = 3 };

void Audio_Tick(void);
void Music_SendCommands(void);
bool Music_IsPlaying(void);
bool ZeldaIsPlayingMusicTrack(uint8 track);
bool ZeldaIsPlayingMusicTrackWithBug(uint8 track);
uint8 ZeldaGetEntranceMusicTrack(int track);
void Music_Command(uint32_t adr, uint8_t val);
uint8_t Music_IsStarted(void);
uint8_t Music_Status(uint32_t adr);

#endif  // ZELDA3_AUDIO_H_
