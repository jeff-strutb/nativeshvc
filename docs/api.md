# The native engine's API, version 4.0 (draft)

**Status: built, 2026-11-08. The maintainer accepted the recommendations on its open points (section 13).** The plan is in `native-engine-plan.md`. Names and fields may still be refined during building, under section 11's
rules once 4.0 is released.

## 0. Conventions

- **One header,** `nshvc/nshvc.h`, with C11 and no platform types. Everything is prefixed `nshvc_`.
- **The SNES's units, without its limits.** Where a value defines how something looks or sounds, the API uses the SNES's
  own units: 15-bit colour, its tile formats, its pitch and envelope scales, its 8-bit volumes. A converted game's numbers
  carry straight across. Where the SNES had a limit, such as screen size, sprite count or sample memory, the API takes a
  wider type, and the limit becomes a setting.
- **No addresses, registers, ports or clock.** Objects are handles, such as `nshvc_tiles` and `nshvc_map`, made by the
  game. Positions are pixels. Time is the update count and real time.
- **Errors** in a game's use of the API are reported, naming the call and the bad value, and end the run in debug builds.
  Release builds report them once and ignore the call.

## 1. The game and the loop

```c
typedef struct nshvc_game {
    const char *title;          /* window and app name */
    const char *id;             /* save folder name: letters, digits, dashes */
    uint32_t    update_hz;      /* the fixed rate of update(), e.g. 60 */
    void (*init)(void);                         /* once, before the first update */
    void (*update)(const nshvc_input *input);   /* game thread, update_hz times a second */
    void (*draw)(float between);                /* game thread, once per picture */
    void (*shutdown)(void);                     /* once, at exit */
    nshvc_music music;          /* the game's music code (section 6); zeroed if none */
} nshvc_game;

int nshvc_run(const nshvc_game *game, int argc, char **argv);   /* returns at exit */
```

- **The engine owns the loop** and calls the game only through these functions. Nothing enters game code at any other
  time.
- `update` runs at `update_hz` in real time, as many times as needed to catch up, and then `draw` runs once for each
  picture the display takes. `between` runs from 0 to 1: how far real time is past the last update towards the next. The
  game uses it to place what moved, and the engine never interpolates on its own.
- **`draw` sets the whole picture** (sections 3 to 5). The engine takes what `draw` set when it returns, and the render
  thread draws it while the game goes on.
- A machine too slow for `update_hz` catches up on at most 0.25 s of real time, and older time is dropped: the game runs
  slower instead of falling further behind. Game state depends only on the updates run and their input.
- `nshvc_updates()` returns the number of updates since `init`, and `nshvc_seconds()` returns real time, for a game that
  wants them. Using real time in game logic makes it non-deterministic, which is the game's own choice (principle 8).
- `nshvc_quit()` ends the run once the current call to the game returns.

## 2. Threads and jobs

| Thread | Runs | May call |
|---|---|---|
| Game | `init`, `update`, `draw`, `shutdown` | Everything except sections 6.3 and 6.4 |
| Audio | The music code (section 6) | Sections 6.3 and 6.4 only |
| Job | Job functions | `nshvc_draw_list_*` on lists the job was given, and nothing else |
| Render, system | The engine's own | Never game code |

```c
typedef void (*nshvc_job_fn)(void *context, uint32_t index);
void nshvc_jobs(nshvc_job_fn fn, void *context, uint32_t count);           /* runs fn for 0..count-1; returns when all are done */
nshvc_job_group nshvc_jobs_begin(nshvc_job_fn fn, void *context, uint32_t count);
void nshvc_jobs_wait(nshvc_job_group group);
uint32_t nshvc_cores(void);
```

- On one core, jobs run in order on the calling thread, so a game is correct on any machine (principle 11).
- **Determinism:** the engine never lets timing change game state. A job writes only its own results, and the game reads
  them after the wait. The engine never hands the game anything from the render or audio threads.

## 3. The picture

The game sets the picture in `draw` with these calls. The engine draws every pixel on the GPU by the SNES's rules.

### 3.1 Screen and settings

```c
typedef struct nshvc_screen {
    int32_t width, height;      /* any size; 256 x 224 the original's */
    int32_t origin_x, origin_y; /* where the original's 256 x 224 sits in it, for layers that wrap */
    uint8_t overscan;           /* 239 lines in place of 224, as the SNES's setting */
    uint8_t interlace;          /* fields woven */
    uint8_t brightness;         /* 0-15, the SNES's */
    uint8_t blank;              /* the screen black */
} nshvc_screen;
void nshvc_screen_set(const nshvc_screen *s);

typedef struct nshvc_limits {
    uint16_t sprites_per_line;  /* 32 the SNES's; 0 none */
    uint16_t sprite_tiles_per_line;  /* 34 the SNES's; 0 none */
    uint8_t  sprite_wrap;       /* sprites wrap at 256 rows, as the SNES's */
    uint8_t  layer_clip[5];     /* a layer (BG1-4, sprites) kept to the original's 256 x 224 */
} nshvc_limits;
void nshvc_limits_set(const nshvc_limits *l);   /* the default is no limits */
```

### 3.2 Tiles, maps and palettes

```c
nshvc_tiles nshvc_tiles_create(uint8_t bpp, uint32_t count, uint8_t texels_per_pixel);  /* 2, 4 or 8 bpp; texels_per_pixel 1, or more for high-resolution art */
void nshvc_tiles_write(nshvc_tiles t, uint32_t first, const void *data, uint32_t count, nshvc_tile_format f);
                                        /* f: the SNES's planar formats, or packed indices, or Mode 7's */
typedef struct nshvc_map_entry { uint16_t tile; uint8_t palette, priority, hflip, vflip; } nshvc_map_entry;
nshvc_map nshvc_map_create(uint32_t width, uint32_t height);   /* in tiles, any size */
void nshvc_map_write(nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const nshvc_map_entry *entries);
void nshvc_map_write_snes(nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *words);
                                        /* the SNES's 16-bit map words, for converted data */
void nshvc_palette_write(uint32_t first, uint32_t count, const uint16_t *bgr15);   /* 256 entries */
void nshvc_palette_write_rgb(uint32_t first, uint32_t count, const uint32_t *rgb24); /* colours past 15 bits */
```

Writes are uploads: only what changed crosses to the GPU, and a write in `draw` shows in that picture.

### 3.3 Layers

```c
typedef struct nshvc_layer {
    uint8_t     enabled_main, enabled_sub;
    nshvc_tiles tiles;
    nshvc_map   map;
    uint8_t     tile_size;          /* 8 or 16 */
    int32_t     scroll_x, scroll_y;
    uint8_t     mosaic;             /* 1-16 */
    uint8_t     wrap;               /* the map repeats */
    nshvc_map   offset_per_tile;    /* modes 2, 4, 6: the SNES's scheme, or none */
    uint8_t     direct_colour;
    nshvc_image image;              /* in place of tiles and map: a whole picture, such as a high-resolution or
                                       full-colour background (section 4) */
    nshvc_draw_list draw_list;      /* in place of tiles and map: a game's chip work (section 4) */
} nshvc_layer;
void nshvc_mode_set(uint8_t mode, uint8_t bg3_priority, uint8_t hires);   /* modes 0-7: bpp and priority order, as the SNES's */
void nshvc_layer_set(uint8_t layer, const nshvc_layer *l);                /* layers 0-3: BG1-BG4 */
```

### 3.4 Mode 7

```c
typedef struct nshvc_mode7 {
    int16_t a, b, c, d;             /* the matrix, 8.8 fixed point, as the SNES's */
    int16_t centre_x, centre_y, scroll_x, scroll_y;
    uint8_t outside;                /* repeat, transparent, or tile 0 */
    uint8_t flip_x, flip_y, extbg;
    nshvc_tiles tiles; nshvc_map map;
} nshvc_mode7;
void nshvc_mode7_set(const nshvc_mode7 *m);
```

Drawn at the window's resolution and anti-aliased, as today.

### 3.5 Sprites

```c
typedef struct nshvc_sprite {
    int32_t x, y;                   /* any position */
    uint16_t width, height;         /* any size in tiles x 8; the SNES's sizes are 8 to 64 */
    nshvc_tiles tiles; uint16_t tile;
    uint8_t palette, priority, hflip, vflip;
} nshvc_sprite;
void nshvc_sprites_set(const nshvc_sprite *list, uint32_t count, uint32_t first_priority);
                                    /* in order; first_priority is the SNES's priority rotation */
```

### 3.6 Windows, colour maths and brightness

```c
typedef struct nshvc_window { int32_t left, right; } nshvc_window;   /* screen columns */
typedef struct nshvc_windows {
    nshvc_window w[2];
    uint8_t layer_use[6];   /* per BG1-4, sprites, colour: which windows, inverted, and their logic */
    uint8_t mask_main[5], mask_sub[5];
} nshvc_windows;
void nshvc_windows_set(const nshvc_windows *w);

typedef struct nshvc_blend {
    uint8_t  subtract, half, use_fixed;
    uint8_t  layers;                /* which of BG1-4, sprites, backdrop take part */
    uint8_t  clip, prevent;         /* the colour window's regions, as the SNES's */
    uint16_t fixed_bgr15;
} nshvc_blend;
void nshvc_blend_set(const nshvc_blend *b);
```

### 3.7 Changes by line

```c
void nshvc_line(uint32_t line, nshvc_line_field field, int32_t value);
void nshvc_line_from(uint32_t line, int32_t column, nshvc_line_field field, int32_t value);  /* a split inside a line */
void nshvc_lines(uint32_t first, uint32_t count, nshvc_line_field field, const int32_t *values);
```

Any field of sections 3.1 to 3.6 can change from a line on: scroll, palette entries, the fixed colour, window edges,
brightness, layer enables, mode and the Mode 7 matrix. This replaces HDMA and raster interrupts.

## 4. Images, the drawing pipeline and shaders

```c
nshvc_image nshvc_image_create(uint32_t w, uint32_t h, nshvc_image_format f);  /* indexed or full colour */
void nshvc_image_write(nshvc_image i, const void *texels);

nshvc_draw_list nshvc_draw_list_create(void);
void nshvc_draw_list_clear(nshvc_draw_list d, uint32_t width, uint32_t height);   /* its size in pixels */
void nshvc_draw_tris(nshvc_draw_list d, const nshvc_vertex *v, uint32_t count, const nshvc_material *m);
void nshvc_draw_lines(nshvc_draw_list d, const nshvc_vertex *v, uint32_t count, const nshvc_material *m);
void nshvc_draw_image(nshvc_draw_list d, nshvc_image i, const nshvc_rect *to, const nshvc_material *m);
void nshvc_draw_rule(nshvc_draw_list d, nshvc_fill_rule r);   /* smooth, or an exact span rule for original polygons */
```

- Vertices are screen positions with sub-pixel precision, plus a palette index, colour or texture coordinate. Drawing is
  in the order given, and a game sorts its own polygons, as the Super FX's code did.
- A draw list becomes a layer (`nshvc_layer.draw_list`) and is composed by the SNES's rules: priority, windows and colour
  maths. Its pixels are drawn at the window's resolution, or at the original's for an exact look.
- Jobs may fill separate lists in parallel.

```c
nshvc_shader nshvc_shader_load(const char *name);     /* a shader in the engine's shading language, a fragment-only subset
                                                         of GLSL 4.5, compiled at build time for every backend (section 13) */
```

A material names a texture or palette, an optional shader, and its inputs. The core picture needs no shader from
the game, and nothing in the core needs compute (principle 11).

## 5. Display and the window

```c
nshvc_display nshvc_display_info(void);   /* window pixels, safe area, refresh rate, keyboard present */
void nshvc_overlay_set(uint32_t id, nshvc_image i, const float corners[6], float alpha);   /* the game's UI over the picture */
void nshvc_text_input(int on);             /* the system's on-screen keyboard where there is no keyboard */
uint32_t nshvc_text_read(char *buffer, uint32_t max);   /* what has been typed since the last read */
void nshvc_pause(int on);                  /* the engine stops calling update; draw goes on */
```

Sharp scaling to any window size and shape, fullscreen and the window itself are the engine's. A game never sees which
platform it is on.

## 6. Sound

### 6.1 Samples and settings (game thread)

```c
nshvc_sample nshvc_sample_load(const void *data, uint32_t size, nshvc_sample_format f, uint32_t loop_start);
                                        /* f: the SNES's BRR, or 16-bit PCM */
typedef struct nshvc_sound_limits { uint16_t voices; uint32_t sample_bytes; } nshvc_sound_limits;  /* 8 and 64 KB the SNES's; 0 none */
void nshvc_sound_limits_set(const nshvc_sound_limits *l);
```

### 6.2 The music code

```c
typedef struct nshvc_music {
    uint32_t hz;                /* how often tick() runs on the audio thread */
    void (*tick)(void);         /* the game's music code */
    uint32_t queue_bytes;       /* room for messages from the game */
} nshvc_music;
void nshvc_music_send(const void *message, uint32_t size);   /* game thread to music code; one way */
```

### 6.3 Voices (audio thread)

```c
void nshvc_voice_key_on(uint32_t v, nshvc_sample s);
void nshvc_voice_key_off(uint32_t v);
void nshvc_voice_pitch(uint32_t v, uint16_t pitch);          /* 0x1000 the sample's own rate, as the SNES's */
void nshvc_voice_volume(uint32_t v, int8_t left, int8_t right);
void nshvc_voice_adsr(uint32_t v, uint8_t attack, uint8_t decay, uint8_t sustain_level, uint8_t sustain_rate);
void nshvc_voice_gain(uint32_t v, nshvc_gain_mode mode, uint8_t value);   /* direct, linear or bent rise, linear or exponential fall */
void nshvc_voice_options(uint32_t v, uint8_t noise, uint8_t echo, uint8_t pitch_mod);
nshvc_voice_state nshvc_voice_read(uint32_t v);               /* envelope level, last output, ended: what drivers read */
```

### 6.4 The whole mix (audio thread)

```c
void nshvc_sound_volume(int8_t left, int8_t right);
void nshvc_sound_noise_rate(uint8_t rate);
void nshvc_echo(uint8_t delay_16ms, int8_t feedback, int8_t left, int8_t right, const int8_t fir[8]);
void nshvc_sound_mute(int on);
uint32_t nshvc_music_receive(void *buffer, uint32_t max);    /* the next message from the game, or 0 */
```

- Mixed by the SNES's rules: BRR decoding, Gaussian interpolation, envelopes, noise, pitch modulation, echo and its filter.
  It runs at 32,040 Hz by default, as the console's DSP does, and is resampled to the device.
- **Audio thread rules:** music code must never wait, lock, allocate, do file work or block. The engine reports any call
  that would, naming it.

## 7. Input

```c
typedef enum { NSHVC_PAD, NSHVC_MOUSE, NSHVC_SUPERSCOPE, NSHVC_JUSTIFIER } nshvc_device;
void nshvc_players(uint32_t count, const nshvc_device *devices);   /* what each player uses; a multitap is just more players */

typedef struct nshvc_input {
    uint16_t pad[8];            /* the SNES's buttons, B Y Select Start Up Down Left Right A X L R, as bits */
    struct { int16_t dx, dy; uint8_t buttons, speed; } mouse[2];
    struct { int32_t x, y; uint8_t trigger, cursor, turbo, pause, start; uint8_t on_screen; } gun[2];
} nshvc_input;

const char *nshvc_control_name(uint32_t player, uint32_t button);   /* as the player's device labels it */
nshvc_source nshvc_control_source(uint32_t player);                 /* keyboard, controller and family, mouse, touch */
void nshvc_control_bind(uint32_t player, uint32_t button, nshvc_key key);
```

- The engine maps every keyboard, controller, mouse and touch screen to these automatically, with on-screen controls on a
  touch screen. A light gun's aim is simply where the pointer or touch is, in screen pixels.
- `update` receives the input sampled for that update, so a recorded run replays exactly.

## 8. Saves and game data

```c
int  nshvc_save_write(const char *name, const void *data, uint32_t size);   /* written off the game thread */
int  nshvc_save_read(const char *name, void *data, uint32_t size);          /* returns bytes read, or -1 */
void nshvc_save_delete(const char *name);

nshvc_asset nshvc_asset_open(const char *path);   /* from the game's package, on every platform */
uint32_t    nshvc_asset_read(nshvc_asset a, void *data, uint32_t size);
void        nshvc_asset_close(nshvc_asset a);
```

What a game saves and packages, and where its data comes from, is the game's.

## 9. Profiling

```c
void nshvc_profile_begin(const char *zone);
void nshvc_profile_end(void);
nshvc_profile nshvc_profile_frame(void);   /* each thread's time in the last frame, the engine's and the game's */
```

## 10. Testing and headless runs

The same game runs headless with no window or audio device: `--headless --updates N --input FILE --shots DIR --sound
FILE`. Input is replayed by update number, so a run repeats exactly. `nshvc_picture_read()` returns the last picture, read
back from the GPU, for checks and screenshots.

## 11. Stability rules

- `NSHVC_VERSION_MAJOR`, `_MINOR` and `_PATCH` say which version a build is. This draft is 4.0.
- **Major (5.0):** the only release that may change or remove anything a game uses.
- **Minor (4.1):** may only add calls, settings, or fields at the end of a struct that a zeroed struct leaves off. A 4.0 game
  builds and plays the same.
- **Patch (4.0.1):** fixes where the engine differs from the SNES's rules, checked against bsnes.
- Every struct a game fills is zeroed to mean "the default". Every default is the SNES's own look and sound, with no limits.

## 12. Every current feature, mapped

### Kept, as these calls

| Today | Becomes |
|---|---|
| `nshvc_main`, `nshvc_power`, `nshvc_run_frame` | `nshvc_run` and `nshvc_game` |
| Picture modes 0-7, 16x16 tiles, offset-per-tile, direct colour, mosaic, hires, pseudo-hires, overscan, interlace, OBJ interlace | Section 3 |
| Mode 7, EXTBG, Mode 7 at the window's resolution | Section 3.4 |
| Sprites, every size, flips, priority rotation, line limits | Section 3.5, `nshvc_limits` |
| Windows, colour maths, brightness | Section 3.6 |
| HDMA's per-line changes | Section 3.7 |
| `nshvc_ext_screen`, `_screen_origin` | `nshvc_screen` |
| `nshvc_ext_sprite_page`, `_sprite_x`, `_sprite_y` | 32-bit sprite positions |
| `nshvc_ext_layer_outside`, `nshvc_ext_vram`, `_vram_page`, `_layer_page`, `_vram_copy`, `_vram_direct` | Maps and tile sets of any size, with no pages |
| `nshvc_ext_layer_clip`, `_sprite_wrap`, `_sprite_limits` | `nshvc_limits` |
| `nshvc_ext_window_edges`, `_window_map`, `_window_columns` | Windows in screen columns |
| `nshvc_ext_canvas_chars`, `_canvas_scale`, `_canvas_presented` | High-resolution tiles (`texels_per_pixel`) and `nshvc_display_info` |
| `nshvc_ext_canvas_screen`, `_screen32` | A layer from an `nshvc_image` |
| `nshvc_overlay_image`, `_place`, `_update` | `nshvc_overlay_set` |
| `nshvc_ext_pause`, `_paused` | `nshvc_pause` |
| `nshvc_ext_key_for`, `_key_bind`, `_key_name`, `nshvc_player_label`, `nshvc_player_source` | Section 7 |
| `nshvc_ext_text_input`, `nshvc_keyboard_attached`, `nshvc_safe_area`, `nshvc_window_size` | `nshvc_text_input`, `nshvc_display_info` |
| `nshvc_plug`, `nshvc_plugged`, the seven `NSHVC_PORT_*` devices | `nshvc_players`. A multitap is more players, and an empty port is a player with no device |
| `nshvc_set_joypad`, `_mouse`, `_gun`, `nshvc_game_buttons` | `nshvc_input`, and headless replay |
| `nshvc_save_dir`, battery saves | Section 8 |
| `nshvc_audio_take`, `nshvc_audio_queued` | The engine's audio output; headless `--sound` |
| `nshvc_picture` | `nshvc_picture_read` |
| `nshvc_frames` | `nshvc_updates` |
| `nshvc_fatal` | The engine's error report |
| The DSP: BRR, Gaussian, ADSR and gain, noise, pitch modulation, echo and FIR, key on and off, mute, ENVX, OUTX and ENDX | Section 6 |

### Retired, and why

| Today | Why |
|---|---|
| `machine.h`: `rd8`, `wr8` and the bus, `cpu`, `abi_` routines, `call_routine`, `cpu_wai`, `nshvc_wait_*` | The bus, processor state and waits are hardware (principle 5) |
| `spc.h`: `spc_rd`, `spc_wr`, the SPC700's state, the boot ROM | The SPC700 is gone. The game's music code calls voices |
| `cart.h`: the chip connector, `nshvc_coro_*` | A chip's logic is the game's, feeding a draw list |
| `hw.h`: register names | There are no registers |
| `nshvc_cart`: mappings, ROM, routine tables, `nshvc_access_kind` | No address space. Data comes from the game's package |
| `nshvc_set_cpu_costs`, the cost profile | No console speed (principle 6) |
| `nshvc_hold_interrupts` | No interrupts |
| `nshvc_set_frame_hook`, `nshvc_set_hooks` | `draw` and the engine's input mapping |
| `nshvc_set_write_hook`, `_read_hook`, `_register_write_hook`, `nshvc_peek` | No bus to watch |
| `nshvc_wram`, `nshvc_sram`, `nshvc_aram`, `nshvc_vram`, `nshvc_cgram`, `nshvc_oam` | No console memories. A game's data is its own |
| `nshvc_ext_gpu_device`, `_canvas_screen_gpu`, `_canvas_chars_gpu`, `_overlay_gpu`, the GPU handles and fences | Per-platform GPU code in a game (principle 4). Shaders take their place |
| `nshvc_game_main` | `nshvc_run` |
| STAT77, the H and V counters, open bus, the multiplier and divider | Hardware state a game read. A converted game does its own maths |
| Script `save` and `load`, `fork` states | No save states |
| `NSHVC_SCALE`, `NSHVC_STATES`, `NSHVC_SCRIPT_NOFORK`, `NSHVC_SPCLOG` | Options for retired parts |

## 13. Settled on review (the maintainer, 2026-11-08)

1. **The shading language** is a fragment-only subset of GLSL 4.5. The engine's tools compile it at build time to SPIR-V for
   Vulkan, Metal Shading Language for Metal, and DXIL for D3D12.
2. **`draw` sets the whole picture each time.** The settings of sections 3.1 and 3.3 to 3.7 apply to the picture being
   drawn and nothing carries over. Tile sets, maps, palettes, images and samples are objects that persist until changed.
3. **The mouse's speed setting and the guns' extra buttons stay,** as the SNES had them, because games read them.
