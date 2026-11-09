// audio.c: the game's side of its music code, and the messages to it.
#include <stdatomic.h>
#include <string.h>
#include "nshvc/nshvc.h"
#include "audio.h"
#include "zelda_rtl.h"
#include "variables.h"
#include "features.h"
#include "spc_player.h"
#include "assets.h"

enum { kMsg_Ports, kMsg_Bank };
typedef struct Message {
  uint8 kind;
  uint8 ports[4];
  const uint8 *bank;
  const nshvc_sample *samples;
} Message;

enum { kSampleTables = 8, kKnownSamples = 512 };
static nshvc_sample g_tables[kSampleTables][256];
static int g_table_next;
static struct { uint64 hash; nshvc_sample s; } g_known[kKnownSamples];
static int g_known_count;
static uint8 g_bank_ram[65536];

static uint8 g_ports[4];
static _Atomic uint8 g_status[4];

// Only the audio thread touches these
static SpcPlayer *g_player;
static uint8 g_queue[16][4];
static int g_queue_read, g_queue_count;
static uint32 g_samples;

void Music_Command(uint32_t adr, uint8_t val) {
  g_ports[adr & 3] = val;
}

uint8_t Music_IsStarted(void) {
  return g_ram[kRam_MusicPlaying];
}

uint8_t Music_Status(uint32_t adr) {
  return atomic_load(&g_status[adr & 3]);
}

void Music_SendCommands(void) {
  Message m;
  memset(&m, 0, sizeof(m));
  m.kind = kMsg_Ports;
  memcpy(m.ports, g_ports, 4);
  nshvc_music_send(&m, sizeof(m));
}

bool Music_IsPlaying(void) {
  return Music_Status(0) != 0;
}

bool ZeldaIsPlayingMusicTrack(uint8 track) {
  return track == music_unk1;
}

bool ZeldaIsPlayingMusicTrackWithBug(uint8 track) {
  return track == (enhanced_features0 & kFeatures0_MiscBugFixes ? music_unk1 : last_music_control);
}

uint8 ZeldaGetEntranceMusicTrack(int i) {
  return kEntranceData_musicTrack[i];
}

// The same samples recur between banks, so each is loaded once.
static nshvc_sample FindOrLoadSample(const uint8 *data, uint32 size, uint32 loop) {
  uint64 h = 14695981039346656037ull ^ loop;
  for (uint32 i = 0; i < size; i++)
    h = (h ^ data[i]) * 1099511628211ull;
  for (int i = 0; i < g_known_count; i++)
    if (g_known[i].hash == h)
      return g_known[i].s;
  nshvc_sample s = nshvc_sample_load(data, size, NSHVC_SAMPLE_BRR, loop);
  if (g_known_count < kKnownSamples) {
    g_known[g_known_count].hash = h;
    g_known[g_known_count++].s = s;
  }
  return s;
}

static const nshvc_sample *LoadBankSamples(const uint8 *data) {
  nshvc_sample *table = g_tables[g_table_next++ % kSampleTables];
  for (;;) {
    int numbytes = *(uint16 *)data;
    if (numbytes == 0)
      break;
    int target = *(uint16 *)(data + 2);
    data += 4;
    for (; numbytes; numbytes--)
      g_bank_ram[target++ & 0xffff] = *data++;
  }
  enum { kSampleDirectory = 0x3c00 };
  for (int e = 0; e < 256; e++) {
    const uint8 *d = &g_bank_ram[kSampleDirectory + e * 4];
    uint32 start = d[0] | d[1] << 8, loop = d[2] | d[3] << 8, blocks = 0, at = start;
    table[e].id = 0;
    if (start < 0x200 || start > 0xfff0)
      continue;
    while (at + 9 <= 0x10000 && blocks < 7000) {
      blocks++;
      if (g_bank_ram[at] & 1)
        break;
      at += 9;
    }
    if (loop < start || loop >= start + blocks * 9)
      loop = start;
    table[e] = FindOrLoadSample(&g_bank_ram[start], blocks * 9, loop - start);
  }
  return table;
}

void LoadSongBank(const uint8 *p) {  // 808888
  Message m;
  memset(&m, 0, sizeof(m));
  m.kind = kMsg_Bank;
  m.bank = p;
  m.samples = LoadBankSamples(p);
  nshvc_music_send(&m, sizeof(m));
}

// The audio thread, once for each of the 32,040 samples a second.
void Audio_Tick(void) {
  Message m;
  if (!g_player) {
    g_player = SpcPlayer_Create();
    SpcPlayer_Initialize(g_player);
  }
  while (nshvc_music_receive(&m, sizeof(m))) {
    if (m.kind == kMsg_Bank) {
      SpcPlayer_Upload(g_player, m.bank);
      Voices_UseSamples(m.samples);
    } else if (g_queue_count < 16) {
      memcpy(g_queue[(g_queue_read + g_queue_count++) & 15], m.ports, 4);
    }
  }
  // Called once a sample. The game's frame is 534 samples and the driver's timer 64.
  g_samples++;
  if (g_samples % 534 == 0 && g_queue_count) {
    memcpy(g_player->commands, g_queue[g_queue_read], 4);
    g_queue_read = (g_queue_read + 1) & 15;
    g_queue_count--;
  }
  if (g_samples % 64 == 0)
    SpcPlayer_Tick(g_player);
  for (int i = 0; i < 4; i++)
    atomic_store(&g_status[i], g_player->status_to_game[i]);
}
