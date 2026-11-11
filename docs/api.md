# The native engine's API, version 6.1

**Status: released and in use. `docs/contract.md` holds the stability rules.** `native-engine-plan.md` records how the engine was planned and what has been run.

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
    uint32_t    update_hz;      /* the fixed rate of update(), e.g. 60; at most 1000000000, a nanosecond each */
    void (*init)(void);                         /* once, before the first update */
    void (*update)(const nshvc_input *input);   /* game thread, update_hz times a second */
    void (*draw)(float between);                /* game thread, once per picture */
    void (*shutdown)(void);                     /* once, at exit */
    nshvc_music music;          /* the game's music code (section 6); zeroed if none */
    float window_aspect;        /* the window's shape when it opens, width over height; 0: 4:3 */
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

The game sets the picture in `draw` with these calls: the SNES's capabilities, in its units. The engine draws them
natively on the GPU, at the window's own resolution, with nothing emulated and no picture made at the console's size:

| Capability | Drawn as |
|---|---|
| Tile layers | An instance for each 8 x 8 texels of a tile the screen shows, placed by the scroll, its map entry read by the vertex stage, its texels from the tile set's texture; the engine keeps whether each tile written is blank (every texel 0), solid (none) or mixed, and draws the solid tiles and the mixed tiles separately, a blank tile not at all, so a layer's cost follows what its tiles hold |
| Mosaic | An instance for each block, of the one texel it shows |
| Mode 7 | A plane textured through the matrix: the plane is drawn into a texture from its map and tiles when they change |
| Sprites | An instance each (in pieces where the line limits or the wrap cut one); the first in priority order to show a pixel keeps it, by a stencil bit |
| Images as layers, draw lists | A quad; the list's own triangles and lines |
| Priorities between layers and sprites | Depth and the depth test |
| Windows | Stencil bits, set from rectangles made from the window edges |
| Changes by line | Strips: the rows (and columns) where a capability's values are the same are drawn together, with those values |
| Colour maths, brightness, blanking | Done where a layer's colour is written, on its own pixel and what the other screen shows there (the sub screen, drawn into a target of its own only on pictures whose maths need it), exactly as the SNES defines it: each channel in 5-bit steps (eighths of a step for colours past 15 bits) added or taken away, halved and kept to the range, then brightness |
| Hires | The sub screen in the main's even half-columns, by a stencil bit |
| Interlace | The picture keeps the other field's half-rows from the picture before; only an interlaced picture is drawn through a kept picture, any other is one pass into the window |

Texels are drawn sharp: a pixel a texel covers takes its colour whole, and only the pixel an edge between texels falls in
mixes the two. Where Mode 7's plane is drawn smaller than its texels, it is read from the plane in colour and its
smaller levels by arithmetic, the same on every GPU, so it does not shimmer. Edges of geometry are smoothed by the picture's samples a pixel (8 where the GPU has them, else 4). The cost follows
what the game draws: a draw list's or the sprites' rows of one setting are drawn once, however the setting changes down
the screen.

### 3.1 Screen and settings

```c
typedef struct nshvc_screen {
    int32_t width, height;      /* any size, the game's to set; 0: the original's 256 x 224 */
    int32_t origin_x, origin_y; /* where the original's 256 x 224 sits in it, for layers that wrap */
    uint8_t overscan;           /* lets the console's 239 lines be shown, as the SNES's setting: what the original's
                                   limits and a layer kept to its size cover; it does not change `height`, which a game
                                   that shows 239 lines sets to 239 */
    uint8_t interlace;          /* fields woven: each picture draws one field's half-rows, keeping the other's */
    uint8_t brightness;         /* 0-15, the SNES's */
    uint8_t blank;              /* the screen black */
    int32_t show_left, show_right, show_top, show_bottom;   /* the part shown, the rest black; right, bottom 0: to the edge */
    float   aspect;             /* shown shape, width over height: 0 the original's pixels (256 x 224 is 4:3), 8.0f / 7.0f etc; NSHVC_ASPECT_FILL fills the window */
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
                                        /* f: the SNES's planar formats, or packed indices, or Mode 7's;
                                           count 0 writes nothing; a tile never written reads 0 */
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
    uint8_t outside;                /* 0 repeat, 1 transparent, 2 tile 0 */
    uint8_t flip_x, flip_y, extbg;
    nshvc_tiles tiles; nshvc_map map;
} nshvc_mode7;
void nshvc_mode7_set(const nshvc_mode7 *m);
```

Drawn as a plane at the window's resolution, its texture coordinates from the matrix: the console's own pixel shows
the texel the console shows there, and the window's pixels between show the plane between. Where a pixel covers more than
a texel, the plane in colour is read at the smaller level whose texels are no longer than the pixel's footprint and the texels
that level has under the footprint, each by the share of the footprint's area inside it: the exact area mean wherever the
footprint covers whole texels of that level and the plane is flat inside one where it does not, with no GPU filter in it. A
footprint across the plane's edge mixes the plane and what is outside it by how much of each it covers. An `outside`
other than 0, 1 or 2 is reported and not taken: `nshvc_mode7_set` takes the rest and keeps `outside` as it was; a line
change to it is left out. Each line's own rounding of the matrix, as the console's, holds on every row.

Where it is filtered, the plane in colour is kept full size with its smaller levels: for each layer's (BG1's, and BG2's
two priorities) the most common colouring among its rows drawn smaller than their texels (the base), and the four other colourings (palette, direct colour, tile
set) whose rows are nearest; a row in one of those reads it in one read, exactly. Past those, each colouring has the plane
at a quarter of the size in its own colours, and its rows read the base plus that less the base at the same size, exact
wherever a pixel spans four texels or more, only the finest detail of the difference lost nearer. With tile 0 outside,
tile 0 repeated across the plane is kept beside each of those, so a footprint across the edge mixes the two exactly.
Only colourings drawn smaller than their texels somewhere have planes; each is made again only when its plane, tiles or
colours change, a plane of tile 0 only when tile 0's colours do. GPU memory: the plane (1 MB); about 5.6 MB for each of
up to three bases and four full-size planes; the quarter-size planes in arrays of 64 layers (about 22.4 MB each, the
first holding the first 57 colourings past the full-size ones, more arrays as more colourings come; they are kept once
made); with tile 0 outside, up to seven planes of tile 0 (about 5.6 MB each).

### 3.5 Sprites

```c
typedef struct nshvc_sprite {
    int32_t x, y;                   /* any position */
    uint16_t width, height;         /* any size in tiles x 8; the SNES's sizes are 8 to 64 */
    nshvc_tiles tiles; uint16_t tile;
    uint8_t palette, priority, hflip, vflip;    /* vflip of a sprite twice as high as wide (the SNES's 16 x 32 and 32 x 64)
                                                   is the SNES's: each square half flips in place, the halves keep their places */
} nshvc_sprite;
void nshvc_sprites_show(uint8_t main_screen, uint8_t sub_screen);   /* which screens the sprites are on; until set, main only */
void nshvc_sprites_interlace(uint8_t on);   /* OBJ interlace (6.1): see below; off until set, each picture */
void nshvc_sprites_set(const nshvc_sprite *list, uint32_t count, uint32_t first_priority);
                                    /* in order; first_priority is the SNES's priority rotation */
```

**OBJ interlace** (`nshvc_sprites_interlace`, 6.1) is a sprite-wide setting, so it sits with `nshvc_sprites_show` and, like it, is
set in `draw` and off until set. On, each sprite is half as high on screen (`height` tiles cover `height * 4` rows) and shows
every other row of itself: the even rows on the even field, the odd rows on the odd. With screen interlace (`nshvc_screen.interlace`)
the picture's two fields are its half-rows, at the screen's finer rows, so a sprite shows all its rows, one to each of those rows.
Without it the console's field alternates with each picture anyway, and so does the engine's: a sprite shows its even rows in one
picture and its odd rows in the next, each row stretched over the picture row it covers. A sprite's vertical flip is taken before the
field is. The SNES's size selects 6 and 7 give small sprites of 16 x 16, not 16 x 32, under OBJ interlace (a hardware quirk): a game
that wants that gives the 16 x 16 size itself.

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

Any field of sections 3.1 to 3.6 can change from a line on: scroll, palette entries (15-bit colours, as
`nshvc_palette_write` takes), the fixed colour, window edges, brightness, layer enables, mode and the Mode 7 matrix. This replaces HDMA and raster interrupts.

## 4. Images, the drawing pipeline and shaders

```c
nshvc_image nshvc_image_create(uint32_t w, uint32_t h, nshvc_image_format f);  /* indexed or full colour */
void nshvc_image_write(nshvc_image i, const void *texels);   /* a full-colour texel's alpha: how much of it covers
                                                               its place, the same as a layer and in a list */

nshvc_draw_list nshvc_draw_list_create(void);
void nshvc_draw_list_clear(nshvc_draw_list d, uint32_t width, uint32_t height);   /* its size in pixels */
void nshvc_draw_tris(nshvc_draw_list d, const nshvc_vertex *v, uint32_t count, const nshvc_material *m);
void nshvc_draw_lines(nshvc_draw_list d, const nshvc_vertex *v, uint32_t count, const nshvc_material *m);
void nshvc_draw_image(nshvc_draw_list d, nshvc_image i, const nshvc_rect *to, const nshvc_material *m);
void nshvc_draw_rule(nshvc_draw_list d, nshvc_fill_rule r);   /* smooth, or vertices on the list's whole pixels */
void nshvc_draw_samples(nshvc_draw_list d, uint32_t samples);  /* 1, 2, 4, 8 (the default) or 16 a pixel */

typedef struct nshvc_vertex {
    float x, y;         /* in the list's pixels, with sub-pixel precision */
    float z;            /* depth in the list, 0 nearest to 1 farthest */
    float w;            /* what x, y and z were divided by in projecting them; 0 or 1 none */
    float u, v;         /* in the material's image's texels */
    uint32_t colour;    /* 0xAARRGGBB, without an image; alpha 0 draws nothing */
} nshvc_vertex;
```

- A draw list is a game's own drawing, such as its 3D, drawn natively straight into the frame at the window's
  resolution: the list's `width` x `height` pixels cover the screen, as an image layer's do.
- It takes its layer's place (`nshvc_layer.draw_list`) among the layers and sprites: its depths lie between those of
  the priorities in front of and behind its layer's, so the layers and sprites in front cover it and those behind are
  covered. Within the list the nearer `z` is drawn, and of equal depths the later, so a flat list draws in the order
  given. `w` makes texture coordinates follow perspective.
- Its pixels take part in windows, colour maths and brightness as its layer's do. A list on a layer that neither screen
  shows is not drawn at all.
- Each of a list's pixels has several samples, 8 unless the game sets otherwise with `nshvc_draw_samples`. Each sample is
  composed as a pixel is (palette, see-through 0, priorities, windows, colour maths) and the colours averaged, so
  polygon edges are smooth and nothing else changes. 1 gives hard edges. A count above what the GPU can fill is capped
  to the most it can; the picture itself has 8 samples a pixel where the GPU has them, else 4. A list with that many
  or more is drawn in the picture's own pass; one with fewer is drawn into a window-sized target of its own at its
  count and placed where its layer is, which costs one extra window-sized pass for that list, only on pictures where a
  game asks for it (a picture draws at most eight such lists this way; the rest are drawn with the picture's samples
  and the engine reports it). The setting is the list's, kept until changed. The picture `nshvc_picture_read` returns is
  made the same way. A pixel's alpha says how much of it a triangle covers. A line is a list pixel wide at any window size.
- A material's image is full colour, or indexed through the palette with 0 see-through, its texels drawn sharp; without
  one, the vertices' colours, mixed across the triangle.
- Jobs may fill separate lists in parallel.

```c
nshvc_shader nshvc_shader_load(const char *name);     /* a shader in the engine's shading language, a fragment-only subset
                                                         of GLSL 4.5, compiled at build time for every backend (section 13) */
```

A material names an image, an optional shader, and its inputs. A game's shader takes the place of the fragment stage of
the engine's own draw-list program: `vec4 shade(vec2 uv)` returns the pixel's colour, its alpha how much of the pixel it
covers, and reads `nshvc_image(ivec2)` (the material's image's texel, an indexed one's through the palette, or the
vertices' colour without an image) and
`nshvc_input(int)`. The core picture needs no shader from the game, and nothing in the core needs compute
(principle 11).

## 5. Display and the window

```c
nshvc_display nshvc_display_info(void);   /* window pixels, safe area, refresh rate, keyboard present */
void nshvc_overlay_set(uint32_t id, nshvc_image i, const float corners[6], float alpha);   /* the game's UI over the picture */
void nshvc_text_input(int on);             /* the system's on-screen keyboard where there is no keyboard */
uint32_t nshvc_text_read(char *buffer, uint32_t max);   /* what has been typed since the last read */
void nshvc_pause(int on);                  /* the engine stops calling update; draw goes on */
```

Drawing at any window size and shape, fullscreen and the window itself are the engine's: the picture is drawn at the
window's own pixels, never at the console's size and scaled. A game never sees which platform it is on.

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
void nshvc_echo_writes(int on);                 /* whether the echo is written back to its buffer; on until turned off */
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
typedef struct nshvc_input {
    uint32_t keys[8], keys_pressed[8], keys_released[8];   /* every key, one bit each, by nshvc_key */
    struct { float x, y, dx, dy, wheel_x, wheel_y; uint8_t buttons, inside; } pointer;   /* window pixels; wheel in lines */
    struct { int32_t id; float x, y; } touch[10];          /* every finger, in window pixels */
    uint8_t touches;
    struct { uint8_t on, style; uint16_t buttons; float lx, ly, rx, ry, lt, rt; } controller[8];
} nshvc_input;

const char *nshvc_key_name(nshvc_key key);                        /* "Z", "Return", "F1"... */
const char *nshvc_controller_name(uint32_t c);                    /* the controller's product name */
const char *nshvc_controller_label(uint32_t c, uint32_t ctl);     /* an NSHVC_CTL_* bit as that controller shows it */
int nshvc_window_to_picture(float x, float y, int32_t *picture_x, int32_t *picture_y);   /* 0 outside the picture */
```

- The engine reports each device as the device has it, and maps nothing to a game's controls. There are no SNES
  buttons, bindings or on-screen controls in the engine: the game decides what every key, button, stick, pointer and
  finger does, as a Unity or Godot game does.
- Every key is reported by its `nshvc_key` (a Windows virtual key code on every host): held, gone down and gone up since
  the last update (`NSHVC_KEY_HELD`, `_PRESSED`, `_RELEASED`). The pointer is in window pixels, with how far the mouse
  moved since the last update, its buttons and its wheel. Every finger on a touch screen has an id that lasts while it is
  down. Each controller gives its buttons (`NSHVC_CTL_*`, by their place, south being the bottom face button on every
  family), both sticks and both triggers.
- `nshvc_key_name` and `nshvc_controller_label` name a key or button as the player sees it (a controller's in its
  family's terms: A, Cross...), so a game's prompts and its "press the button you want" match the device in hand. A
  game that offers rebinding keeps the bindings in its own save.
- On a touch screen the game draws its own controls, with `nshvc_overlay_set`, and reads the fingers itself.
  `nshvc_window_to_picture` gives the picture's pixel under a window pixel, for a light gun's aim or a tap on the
  picture.
- While `nshvc_text_input(1)` is on, a touch screen with no keyboard shows the system's keyboard, and what is typed
  reads with `nshvc_text_read`. The keys are still reported; the game decides what they do meanwhile.
- `update` receives the input sampled for that update, so a recorded run replays exactly. A headless `--input FILE`
  gives the devices by update number: each line is the update, then what is held from it on, until the next line.
  `key=VK` (hex), `controller=N,BUTTONS` (hex), `stick=N,LX,LY,RX,RY`, `trigger=N,LT,RT`, `pointer=X,Y[,BUTTONS]` and
  `touch=ID,X,Y`; `#` begins a comment. A key held on one line and not the next reads as gone up.

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
FILE`. The devices are replayed by update number (section 7), so a run repeats exactly. `nshvc_picture_read()` returns the last finished picture as the
renderer drew it, at the window's size with its overlays (headless, at the screen's size, a pixel for a pixel), read
back from the GPU, for checks and screenshots, or for a game's own use (from `update`, `draw` or shutdown; it waits for
a picture being drawn to finish, a picture sent and not yet drawn waits for it, the run ending or not, and it costs a
readback, so a game calls it when it needs one). `--shots` writes the same. A headless picture is drawn only when it is read; the environment variable `RR_DRAW=1` makes a run with no window draw every picture, into a texture nothing reads, to time the drawing without a display (`RR_SIZE=WxH` sets the size).

## 11. Stability rules

- `NSHVC_VERSION_MAJOR`, `_MINOR` and `_PATCH` say which version a build is. This version is 6.1: it adds `nshvc_sprites_interlace`. 6.0 draws the picture
natively (section 3): the renderer is new, and these calls change. `nshvc_vertex` has `z` and `w`, and its `colour` is
0xAARRGGBB, no longer a palette index; `nshvc_draw_samples` is kept, as 4.7 had it, with the cost above for a list with fewer samples than the picture's; `NSHVC_FILL_EXACT` puts vertices on the list's whole pixels; a game's shader returns alpha as
coverage; `nshvc_picture_read` and `--shots` give the picture at the window's size. 5.0 reported each input device as itself and nothing more: `nshvc_input` lost the SNES pad, mouse and gun fields, and `nshvc_players`, the `nshvc_control_*` calls, `nshvc_source` and the engine's on-screen touch controls are gone; `pointer.dx` and `dy`, `nshvc_key_name`, `nshvc_controller_name`, `nshvc_controller_label` and `nshvc_window_to_picture` are new, and `--input` replays devices. Within 4: 4.1 added `nshvc_sprites_show` and its two line fields; 4.2 `aspect` in `nshvc_screen`; 4.3 the `show_*` fields and `window_aspect` in `nshvc_game`; 4.4 `nshvc_echo_writes`; 4.5 the devices themselves in `nshvc_input`; 4.6 controller bindings; 4.7 `nshvc_draw_samples`, and draw lists smooth at their edges by default.
- **Major (7.0):** the only release that may change or remove anything a game uses.
- **Minor (6.1):** may only add calls, settings, or fields at the end of a struct that a zeroed struct leaves off. A 6.0 game
  builds and plays the same.
- **Patch (6.0.1):** fixes where the engine differs from what this document describes.
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
| `nshvc_ext_key_for`, `_key_bind`, `_key_name`, `nshvc_player_label`, `nshvc_player_source` | The game's own controls, with `nshvc_key_name` and `nshvc_controller_label` (section 7) |
| `nshvc_ext_text_input`, `nshvc_keyboard_attached`, `nshvc_safe_area`, `nshvc_window_size` | `nshvc_text_input`, `nshvc_display_info` |
| `nshvc_plug`, `nshvc_plugged`, the seven `NSHVC_PORT_*` devices | The game's own: it reads the devices in `nshvc_input` as its players' joypads, mice, guns or multitap |
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
| `nshvc_set_frame_hook`, `nshvc_set_hooks` | `draw`, and the game's own mapping of the devices (section 7) |
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
3. **The mouse's speed setting and the guns' extra buttons are the game's own.** The engine reports each device as it is
   (section 7); a game that read those makes them from the devices it gets.
