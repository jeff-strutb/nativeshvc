// controls.c: the game's controls. nativeshvc reports each device as it is;
// this decides what each key, controller button, stick and finger does.
#include "controls.h"

enum {
  kB = 1 << 0, kY = 1 << 1, kSelect = 1 << 2, kStart = 1 << 3, kUp = 1 << 4, kDown = 1 << 5,
  kLeft = 1 << 6, kRight = 1 << 7, kA = 1 << 8, kX = 1 << 9, kL = 1 << 10, kR = 1 << 11,
};

// Keyboard: arrows to move, Z B, X A, A Y, S X, Q L, W R, Return Start,
// Shift or Tab Select.
static const struct { nshvc_key key; int button; } kKeys[] = {
  { 0x26, kUp }, { 0x28, kDown }, { 0x25, kLeft }, { 0x27, kRight }, { 'Z', kB }, { 'X', kA }, { 'A', kY },
  { 'S', kX }, { 'Q', kL }, { 'W', kR }, { 0x0D, kStart }, { 0x10, kSelect }, { 0x09, kSelect },
};

// A controller, by each button's place: the SNES's own layout.
static const struct { uint32_t ctl; int button; } kButtons[] = {
  { NSHVC_CTL_SOUTH, kB }, { NSHVC_CTL_EAST, kA }, { NSHVC_CTL_WEST, kY }, { NSHVC_CTL_NORTH, kX },
  { NSHVC_CTL_LB, kL }, { NSHVC_CTL_RB, kR }, { NSHVC_CTL_BACK, kSelect }, { NSHVC_CTL_MENU, kStart },
  { NSHVC_CTL_UP, kUp }, { NSHVC_CTL_DOWN, kDown }, { NSHVC_CTL_LEFT, kLeft }, { NSHVC_CTL_RIGHT, kRight },
};

// The on-screen controls on a touch screen, as fractions of the window:
// centre x, y and radius (of the height). A finger presses a disc within
// 1.4 radii of its centre.
static const struct { float x, y, r; int button; } kZones[] = {
  { 0.10f, 0.62f, 0.07f, kUp }, { 0.10f, 0.86f, 0.07f, kDown }, { 0.04f, 0.74f, 0.07f, kLeft },
  { 0.16f, 0.74f, 0.07f, kRight }, { 0.84f, 0.86f, 0.08f, kB }, { 0.93f, 0.74f, 0.08f, kA },
  { 0.75f, 0.74f, 0.08f, kY }, { 0.84f, 0.62f, 0.08f, kX }, { 0.06f, 0.30f, 0.08f, kL },
  { 0.94f, 0.30f, 0.08f, kR }, { 0.42f, 0.92f, 0.05f, kSelect }, { 0.58f, 0.92f, 0.05f, kStart },
};
enum { kZoneCount = sizeof kZones / sizeof kZones[0] };

static int g_touched;  // a finger has been on the screen: the discs are shown
static nshvc_image g_disc;

int Controls_Read(const nshvc_input *in) {
  int pad = 0;
  for (int i = 0; i < (int)(sizeof kKeys / sizeof kKeys[0]); i++)
    if (NSHVC_KEY_HELD(in, kKeys[i].key))
      pad |= kKeys[i].button;
  for (int c = 0; c < 8; c++) {
    if (!in->controller[c].on)
      continue;
    for (int i = 0; i < (int)(sizeof kButtons / sizeof kButtons[0]); i++)
      if (in->controller[c].buttons & kButtons[i].ctl)
        pad |= kButtons[i].button;
    if (in->controller[c].ly < -0.5f) pad |= kUp;
    if (in->controller[c].ly > 0.5f) pad |= kDown;
    if (in->controller[c].lx < -0.5f) pad |= kLeft;
    if (in->controller[c].lx > 0.5f) pad |= kRight;
  }
  if (in->touches) {
    nshvc_display d = nshvc_display_info();
    float w = (float)d.width, h = (float)d.height;
    g_touched = 1;
    for (int t = 0; t < in->touches; t++)
      for (int z = 0; z < kZoneCount; z++) {
        float dx = in->touch[t].x - kZones[z].x * w, dy = in->touch[t].y - kZones[z].y * h;
        float r = kZones[z].r * h * 1.4f;
        if (dx * dx + dy * dy <= r * r)
          pad |= kZones[z].button;
      }
  }
  return pad;
}

void Controls_Draw(void) {
  enum { D = 64 };
  if (!g_touched)
    return;
  nshvc_display d = nshvc_display_info();
  if (d.width <= 0 || d.height <= 0)
    return;
  if (!g_disc.id) {
    // a soft disc: a bright rim round a faint middle
    static uint32_t disc[D * D];
    for (int y = 0; y < D; y++)
      for (int x = 0; x < D; x++) {
        int dx = 2 * x + 1 - D, dy = 2 * y + 1 - D, r2 = dx * dx + dy * dy;
        disc[y * D + x] = r2 > D * D ? 0u : r2 > (D - 8) * (D - 8) ? 0xB0FFFFFFu : 0x40FFFFFFu;
      }
    g_disc = nshvc_image_create(D, D, NSHVC_IMAGE_RGBA);
    nshvc_image_write(g_disc, disc);
  }
  for (int z = 0; z < kZoneCount; z++) {
    float cx = kZones[z].x * (float)d.width, cy = kZones[z].y * (float)d.height, r = kZones[z].r * (float)d.height;
    float corners[6] = { cx - r, cy - r, cx + r, cy - r, cx - r, cy + r };
    nshvc_overlay_set((uint32_t)z, g_disc, corners, 1.0f);
  }
}
