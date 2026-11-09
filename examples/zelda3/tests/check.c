// check.c: the game with hooks for checks. ZELDA3_CHECK=1 at build time uses this
// in place of game.c's entry. Environment (desktop only):
//   ZELDA3_CHECK_MODULE=FRAME:MODULE  start a game module at an update
// At exit it saves check.txt: the updates run and a hash of the last picture.
#define ZELDA3_CHECK_BUILD
#include "../src/game.c"

static uint64 g_module_at;
static int g_module, g_entrance = -1;

static void CheckUpdate(const nshvc_input *in) {
  if (g_module_at && nshvc_updates() == g_module_at) {
    if (g_entrance >= 0) which_entrance = g_entrance;
    main_module_index = g_module;
    submodule_index = 0;
    subsubmodule_index = 0;
  }
  Update(in);
}

static void CheckShutdown(void) {
  int w = 0, h = 0;
  const uint32_t *p = nshvc_picture_read(&w, &h);
  uint64 hash = 14695981039346656037ull;
  char text[80];
  for (int i = 0; p && i < w * h; i++)
    hash = (hash ^ (p[i] & 0xffffff)) * 1099511628211ull;
  int n = snprintf(text, sizeof text, "updates %llu size %dx%d picture %016llx\n", (unsigned long long)nshvc_updates(), w, h, (unsigned long long)hash);
  nshvc_save_write("check.txt", text, n);
  fputs(text, stdout);
  fflush(stdout);
}

int main(int argc, char **argv) {
  static nshvc_game game;
  static char updates[] = "--updates", count[] = "900";
  char *fixed[] = { argv[0], updates, count, NULL };
  const char *m = getenv("ZELDA3_CHECK_MODULE");
  unsigned long long at;
  int mod;
  if (m && sscanf(m, "%llu:%i", &at, &mod) == 2) {
    g_module_at = at;
    g_module = mod;
  }
  if (getenv("ZELDA3_CHECK_ENTRANCE"))
    g_entrance = strtol(getenv("ZELDA3_CHECK_ENTRANCE"), NULL, 0);
  game.title = "zelda3 check";
  game.id = "zelda3-check";
  game.update_hz = 60;
  game.init = Init;
  game.update = CheckUpdate;
  game.draw = Draw;
  game.shutdown = CheckShutdown;
  game.music.hz = 32040;
  game.music.tick = Audio_Tick;
  game.music.queue_bytes = 4096;
  return argc > 1 ? nshvc_run(&game, argc, argv) : nshvc_run(&game, 3, fixed);
}
