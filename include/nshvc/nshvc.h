/*
 * nshvc.h: nativeshvc, a native engine for decompiled SNES games.
 *
 * The engine owns the loop and calls the game at set points. The game says
 * what it means through plain calls. docs/api.md is the contract; this header
 * holds the parts built so far.
 */
#ifndef NSHVC_H
#define NSHVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NSHVC_VERSION_MAJOR 6
#define NSHVC_VERSION_MINOR 1
#define NSHVC_VERSION_PATCH 0

/* ---- input ------------------------------------------------------------------- */

/* Each device as the device has it (5.0). The engine maps nothing to a
   game's controls: the game decides what every key, button, stick, pointer
   and finger does. */

/* a controller's buttons as itself, as bits of nshvc_input.controller[].buttons,
   named by their place on an Xbox controller (south, east, west, north...) */
enum {
    NSHVC_CTL_SOUTH = 1u << 0, NSHVC_CTL_EAST = 1u << 1, NSHVC_CTL_WEST = 1u << 2, NSHVC_CTL_NORTH = 1u << 3,
    NSHVC_CTL_LB = 1u << 4, NSHVC_CTL_RB = 1u << 5, NSHVC_CTL_BACK = 1u << 6, NSHVC_CTL_MENU = 1u << 7,
    NSHVC_CTL_LS = 1u << 8, NSHVC_CTL_RS = 1u << 9, NSHVC_CTL_LT = 1u << 10, NSHVC_CTL_RT = 1u << 11,
    NSHVC_CTL_UP = 1u << 12, NSHVC_CTL_RIGHT = 1u << 13, NSHVC_CTL_DOWN = 1u << 14, NSHVC_CTL_LEFT = 1u << 15
};
enum { NSHVC_STYLE_GENERIC, NSHVC_STYLE_XBOX, NSHVC_STYLE_PLAYSTATION, NSHVC_STYLE_NINTENDO };
typedef int nshvc_key;          /* a Windows virtual key code, on every host */

typedef struct nshvc_input {
    /* every key, one bit a key by nshvc_key: held now; gone down since the
       last update (set even when already let go); gone up since the last update */
    uint32_t keys[8], keys_pressed[8], keys_released[8];
    /* the pointer, in window pixels from the top left; dx, dy how far the
       mouse moved since the last update (it moves on at the window's edge);
       buttons bit 0 left, 1 right, 2 middle; the wheel turned since the last
       update, in lines, positive up and right; inside: over the window */
    struct { float x, y, dx, dy, wheel_x, wheel_y; uint8_t buttons, inside; } pointer;
    /* every finger on a touch screen, in window pixels; an id lasts while
       its finger is down */
    struct { int32_t id; float x, y; } touch[10];
    uint8_t touches;
    /* each controller: on when connected; style NSHVC_STYLE_*; buttons
       NSHVC_CTL_*; sticks -1..1, y positive downward; triggers 0..1 */
    struct { uint8_t on, style; uint16_t buttons; float lx, ly, rx, ry, lt, rt; } controller[8];
} nshvc_input;

#define NSHVC_KEY_HELD(in, k)     (((in)->keys[(k) >> 5] >> ((k) & 31)) & 1u)
#define NSHVC_KEY_PRESSED(in, k)  (((in)->keys_pressed[(k) >> 5] >> ((k) & 31)) & 1u)
#define NSHVC_KEY_RELEASED(in, k) (((in)->keys_released[(k) >> 5] >> ((k) & 31)) & 1u)

/* a key's name as a keyboard shows it ("Z", "Return", "F1"...); "" unknown */
const char *nshvc_key_name(nshvc_key key);
/* controller c's product name; "" when none is connected there */
const char *nshvc_controller_name(uint32_t c);
/* button ctl (one NSHVC_CTL_* bit) as controller c shows it ("A", "Cross",
   "L1"...); "" when none is connected there */
const char *nshvc_controller_label(uint32_t c, uint32_t ctl);
/* window pixel (x, y), as the pointer and touches give it, as the picture's
   pixel; 0 when it is outside the picture */
int nshvc_window_to_picture(float x, float y, int32_t *picture_x, int32_t *picture_y);
/* what has been typed since the last read: 'A'-'Z', digits, space, 8, 13 */
uint32_t nshvc_text_read(char *buffer, uint32_t max);

/* ---- saves and game data ------------------------------------------------------------- */

int  nshvc_save_write(const char *name, const void *data, uint32_t size);
int  nshvc_save_read(const char *name, void *data, uint32_t size);      /* bytes read, or -1 */
void nshvc_save_delete(const char *name);

typedef struct nshvc_asset { uint32_t id; } nshvc_asset;
nshvc_asset nshvc_asset_open(const char *path);                         /* id 0: not found */
uint32_t    nshvc_asset_read(nshvc_asset a, void *data, uint32_t size);
void        nshvc_asset_close(nshvc_asset a);

/* ---- the game's music code --------------------------------------------------- */

typedef struct nshvc_music {
    uint32_t hz;                /* how often tick() runs on the audio thread */
    void (*tick)(void);         /* the game's music code */
    uint32_t queue_bytes;       /* room for messages from the game */
} nshvc_music;

/* ---- the game and the loop ---------------------------------------------------- */

typedef struct nshvc_game {
    const char *title;          /* window and app name */
    const char *id;             /* save folder name: letters, digits, dashes */
    uint32_t    update_hz;      /* the fixed rate of update(), e.g. 60 */
    void (*init)(void);                         /* once, before the first update */
    void (*update)(const nshvc_input *input);   /* game thread, update_hz times a second */
    void (*draw)(float between);                /* game thread, once per picture */
    void (*shutdown)(void);                     /* once, at exit */
    nshvc_music music;          /* the game's music code; zeroed if none */
    float window_aspect;        /* the window's shape when it opens, width over height; 0: 4:3 */
} nshvc_game;

/* Runs the game; returns at exit with the process's status.
 *   --headless       no window and no real time: one update, then one draw
 *   --updates N      end after N updates
 *   --input FILE     the devices by update number (docs/api.md section 4)
 *   --shots DIR      each picture, as DIR/NNNNNN.ppm
 *   --sound FILE     the sound: 16-bit stereo at 32,040 Hz (headless)
 *   --inspect FILE   what the game feeds the engine, each time F12 is pressed */
int nshvc_run(const nshvc_game *game, int argc, char **argv);

uint64_t nshvc_updates(void);   /* updates since init */
double   nshvc_seconds(void);   /* real time since init */
void     nshvc_quit(void);      /* end the run after the current call returns */
void     nshvc_pause(int on);   /* the engine stops calling update; draw goes on */

/* ---- sound: docs/api.md section 6 --------------------------------------------------- */

typedef struct nshvc_sample { uint32_t id; } nshvc_sample;
typedef enum { NSHVC_SAMPLE_BRR, NSHVC_SAMPLE_PCM16 } nshvc_sample_format;
typedef enum {
    NSHVC_GAIN_DIRECT = 0, NSHVC_GAIN_LINEAR_DOWN = 4, NSHVC_GAIN_EXP_DOWN = 5, NSHVC_GAIN_LINEAR_UP = 6, NSHVC_GAIN_BENT_UP = 7
} nshvc_gain_mode;
typedef struct nshvc_voice_state { uint8_t level; int8_t output; uint8_t ended; } nshvc_voice_state;
typedef struct nshvc_sound_limits { uint16_t voices; uint32_t sample_bytes; } nshvc_sound_limits;

/* game thread; a loop point of 0xFFFFFFFF (PCM) means none */
nshvc_sample nshvc_sample_load(const void *data, uint32_t size, nshvc_sample_format f, uint32_t loop_start);
void nshvc_sound_limits_set(const nshvc_sound_limits *l);
void nshvc_music_send(const void *message, uint32_t size);

/* audio thread: music code must never wait, lock, allocate, do file work or block */
void nshvc_voice_key_on(uint32_t v, nshvc_sample s);
void nshvc_voice_key_off(uint32_t v);
void nshvc_voice_pitch(uint32_t v, uint16_t pitch);
void nshvc_voice_volume(uint32_t v, int8_t left, int8_t right);
void nshvc_voice_adsr(uint32_t v, uint8_t attack, uint8_t decay, uint8_t sustain_level, uint8_t sustain_rate);
void nshvc_voice_gain(uint32_t v, nshvc_gain_mode mode, uint8_t value);
void nshvc_voice_options(uint32_t v, uint8_t noise, uint8_t echo, uint8_t pitch_mod);
nshvc_voice_state nshvc_voice_read(uint32_t v);
void nshvc_sound_volume(int8_t left, int8_t right);
void nshvc_sound_noise_rate(uint8_t rate);
void nshvc_echo(uint8_t delay_16ms, int8_t feedback, int8_t left, int8_t right, const int8_t fir[8]);
void nshvc_sound_mute(int on);
/* whether the echo's sum is written back to its buffer; on until turned off */
void nshvc_echo_writes(int on);
uint32_t nshvc_music_receive(void *buffer, uint32_t max);

/* ---- profiling and pictures ---------------------------------------------------------- */

#define NSHVC_PROFILE_ZONES 32
typedef struct nshvc_profile {
    double update_ms, draw_ms;      /* the game thread, in the game's own code */
    double engine_ms;               /* the game thread, in the engine: building the picture */
    double render_ms;               /* the render thread, drawing the last picture */
    double audio_ms;                /* the audio thread over the last second, per second of sound */
    struct { const char *name; double ms; } zones[NSHVC_PROFILE_ZONES];
    uint32_t zone_count;
} nshvc_profile;
void nshvc_profile_begin(const char *zone);
void nshvc_profile_end(void);
nshvc_profile nshvc_profile_frame(void);

/* the last finished picture as the renderer drew it, at the window's size
   (with no window, the screen's, a pixel for a pixel) with the overlays:
   0xFFRRGGBB, top row first, read back from the GPU (a stall: call it when
   needed); from update, draw or shutdown; NULL when this renderer keeps none.
   Valid until the next call. */
const uint32_t *nshvc_picture_read(int *width, int *height);

/* ---- jobs --------------------------------------------------------------------- */

typedef void (*nshvc_job_fn)(void *context, uint32_t index);
typedef struct nshvc_job_group { uint32_t id; } nshvc_job_group;

/* runs fn for 0..count-1; returns when all are done */
void nshvc_jobs(nshvc_job_fn fn, void *context, uint32_t count);
nshvc_job_group nshvc_jobs_begin(nshvc_job_fn fn, void *context, uint32_t count);
void nshvc_jobs_wait(nshvc_job_group group);
uint32_t nshvc_cores(void);

/* ---- the picture: set in draw; docs/api.md section 3 ------------------------------ */

typedef struct nshvc_tiles { uint32_t id; } nshvc_tiles;
typedef struct nshvc_map { uint32_t id; } nshvc_map;
typedef struct nshvc_image { uint32_t id; } nshvc_image;
typedef struct nshvc_draw_list { uint32_t id; } nshvc_draw_list;

typedef enum {
    NSHVC_TILES_PLANAR,         /* the SNES's planar format for the tile set's depth */
    NSHVC_TILES_PACKED,         /* a byte a texel */
    NSHVC_TILES_MODE7           /* Mode 7's: a byte a texel, 64 a tile */
} nshvc_tile_format;
typedef enum { NSHVC_IMAGE_INDEXED, NSHVC_IMAGE_RGBA } nshvc_image_format;

typedef struct nshvc_screen {
    int32_t width, height;      /* any size, the game's to set; 0: the original's 256 x 224 */
    int32_t origin_x, origin_y; /* where the original's 256 x 224 sits in it */
    uint8_t overscan;           /* lets the console's 239 lines be shown: what the original's limits and a layer
                                   kept to its size cover; it does not change `height`, which a game that shows
                                   239 lines sets to 239 */
    uint8_t interlace;          /* fields woven: each picture draws one field's half-rows and keeps the other's
                                   from the picture before; modes 5 and 6 show their layers' rows twice as fine */
    uint8_t brightness;         /* 0-15 */
    uint8_t blank;
    float   aspect;             /* the shape the screen is shown at, width over height, whatever the window's:
                                   0 the original's pixels (256 x 224 is 4:3; a wider screen is wider),
                                   8.0f / 7.0f square pixels and so on; NSHVC_ASPECT_FILL fills the window */
    int32_t show_left, show_right, show_top, show_bottom;
                                /* the part of the screen shown, in its columns and rows, the rest black;
                                   right 0 means the whole width, bottom 0 the whole height */
} nshvc_screen;
#define NSHVC_ASPECT_FILL (-1.0f)
void nshvc_screen_set(const nshvc_screen *s);

typedef struct nshvc_limits {
    uint16_t sprites_per_line;  /* 32 the SNES's; 0 none */
    uint16_t sprite_tiles_per_line;  /* 34 the SNES's; 0 none */
    uint8_t  sprite_wrap;
    uint8_t  layer_clip[5];
} nshvc_limits;
void nshvc_limits_set(const nshvc_limits *l);

nshvc_tiles nshvc_tiles_create(uint8_t bpp, uint32_t count, uint8_t texels_per_pixel);
void nshvc_tiles_write(nshvc_tiles t, uint32_t first, const void *data, uint32_t count, nshvc_tile_format f);
typedef struct nshvc_map_entry { uint16_t tile; uint8_t palette, priority, hflip, vflip; } nshvc_map_entry;
nshvc_map nshvc_map_create(uint32_t width, uint32_t height);
void nshvc_map_write(nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const nshvc_map_entry *entries);
void nshvc_map_write_snes(nshvc_map m, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *words);
void nshvc_palette_write(uint32_t first, uint32_t count, const uint16_t *bgr15);
void nshvc_palette_write_rgb(uint32_t first, uint32_t count, const uint32_t *rgb24);

nshvc_image nshvc_image_create(uint32_t w, uint32_t h, nshvc_image_format f);
void nshvc_image_write(nshvc_image i, const void *texels);

typedef struct nshvc_layer {
    uint8_t     enabled_main, enabled_sub;
    nshvc_tiles tiles;
    nshvc_map   map;
    uint8_t     tile_size;          /* 8 or 16 */
    int32_t     scroll_x, scroll_y;
    uint8_t     mosaic;             /* 1-16 */
    uint8_t     wrap;
    nshvc_map   offset_per_tile;
    uint8_t     direct_colour;
    nshvc_image image;
    nshvc_draw_list draw_list;
} nshvc_layer;
void nshvc_mode_set(uint8_t mode, uint8_t bg3_priority, uint8_t hires);
void nshvc_layer_set(uint8_t layer, const nshvc_layer *l);

typedef struct nshvc_mode7 {
    int16_t a, b, c, d;
    int16_t centre_x, centre_y, scroll_x, scroll_y;
    uint8_t outside;                /* 0 repeat, 1 transparent, 2 tile 0 */
    uint8_t flip_x, flip_y, extbg;
    nshvc_tiles tiles; nshvc_map map;
} nshvc_mode7;
void nshvc_mode7_set(const nshvc_mode7 *m);

typedef struct nshvc_sprite {
    int32_t x, y;
    uint16_t width, height;         /* in tiles */
    nshvc_tiles tiles; uint16_t tile;
    uint8_t palette, priority, hflip, vflip;    /* vflip of a sprite twice as high as wide (the SNES's 16 x 32 and 32 x 64)
                                                   is the SNES's: each square half flips in place, the halves keep their places */
} nshvc_sprite;
void nshvc_sprites_set(const nshvc_sprite *list, uint32_t count, uint32_t first_priority);
/* whether the sprites are on the main and the sub screen; until set, main only */
void nshvc_sprites_show(uint8_t main_screen, uint8_t sub_screen);
/* the SNES's OBJ interlace (6.1), per picture like the other sprite-wide settings and off until set: each sprite is
   half as high on screen, a sprite of `height` tiles covering height * 4 rows, and shows every other row of
   itself, the even rows on the picture's even field and the odd rows on its odd field. With screen interlace
   the fields are the picture's own half-rows, each at the screen's finer rows, so the sprite shows all its
   rows, one to a screen row. Without it the field still alternates with each picture, as on the SNES: a
   sprite shows its even rows in one picture and its odd rows in the next, each stretched to the rows it
   covers. A flipped sprite flips before the field is taken. The SNES's sizes 6 and 7 are 16 x 16 for the small
   sprites under OBJ interlace (a hardware quirk): a game gives that size itself. */
void nshvc_sprites_interlace(uint8_t on);

typedef struct nshvc_window { int32_t left, right; } nshvc_window;

/* a layer's use of the windows */
enum {
    NSHVC_WIN1 = 1, NSHVC_WIN1_INVERT = 2, NSHVC_WIN2 = 4, NSHVC_WIN2_INVERT = 8,
    NSHVC_WIN_OR = 0 << 4, NSHVC_WIN_AND = 1 << 4, NSHVC_WIN_XOR = 2 << 4, NSHVC_WIN_XNOR = 3 << 4
};
typedef struct nshvc_windows {
    nshvc_window w[2];
    uint8_t layer_use[6];           /* BG1-4, sprites, colour */
    uint8_t mask_main[5], mask_sub[5];
} nshvc_windows;
void nshvc_windows_set(const nshvc_windows *w);

typedef struct nshvc_blend {
    uint8_t  subtract, half, use_fixed;
    uint8_t  layers;                /* bits: BG1-4, sprites, backdrop */
    uint8_t  clip, prevent;         /* 0 never, 1 outside, 2 inside, 3 always */
    uint16_t fixed_bgr15;
} nshvc_blend;
void nshvc_blend_set(const nshvc_blend *b);

/* ---- the drawing pipeline: a game's own drawing, straight into the frame as a layer ---- */

typedef struct nshvc_vertex {
    float x, y;                     /* in the list's pixels, with sub-pixel precision */
    float z;                        /* depth in the list, 0 nearest to 1 farthest; the nearer is drawn, and of equal
                                       depths the later (6.0) */
    float w;                        /* what x, y and z were divided by in projecting them, for texture coordinates that
                                       follow perspective; 0 or 1 none (6.0) */
    float u, v;                     /* in the material's image's texels */
    uint32_t colour;                /* 0xAARRGGBB, without an image; alpha 0 draws nothing (6.0) */
} nshvc_vertex;
typedef struct nshvc_shader { uint32_t id; } nshvc_shader;
typedef struct nshvc_material {
    nshvc_image image;              /* full colour, or indexed through the palette (0 see-through);
                                       no image: the vertices' colour */
    nshvc_shader shader;            /* none: the image's or colour's value as it is */
    float inputs[8];                /* what the shader reads with nshvc_input */
} nshvc_material;
/* a shader the game's build compiled (tools/nshvc-shaders), by its name */
nshvc_shader nshvc_shader_load(const char *name);
typedef struct nshvc_rect { float x0, y0, x1, y1; } nshvc_rect;
typedef enum { NSHVC_FILL_SMOOTH, NSHVC_FILL_EXACT } nshvc_fill_rule;

nshvc_draw_list nshvc_draw_list_create(void);
void nshvc_draw_list_clear(nshvc_draw_list d, uint32_t width, uint32_t height);
void nshvc_draw_tris(nshvc_draw_list d, const nshvc_vertex *v, uint32_t count, const nshvc_material *m);
void nshvc_draw_lines(nshvc_draw_list d, const nshvc_vertex *v, uint32_t count, const nshvc_material *m);
void nshvc_draw_image(nshvc_draw_list d, nshvc_image i, const nshvc_rect *to, const nshvc_material *m);
/* smooth: vertices where given; exact: on the list's whole pixels (6.0) */
void nshvc_draw_rule(nshvc_draw_list d, nshvc_fill_rule r);
/* how many samples a pixel of the list has: 1, 2, 4, 8 (the default) or 16.
   Each is composed as a pixel is and the colours averaged, so edges are
   smooth; 1 gives hard edges. A GPU that can't fill so many uses the most it
   can; a list with fewer than the picture's own (8 where the GPU has them,
   else 4) is drawn into a target of its own, a window-sized pass for it.
   Kept until changed (6.0) */
void nshvc_draw_samples(nshvc_draw_list d, uint32_t samples);

/* what a line can change; those with a layer add it (0-3), the palette an entry */
typedef enum {
    NSHVC_LINE_SCROLL_X = 0, NSHVC_LINE_SCROLL_Y = 4, NSHVC_LINE_MAIN = 8, NSHVC_LINE_SUB = 12,
    NSHVC_LINE_WIN1_LEFT = 16, NSHVC_LINE_WIN1_RIGHT, NSHVC_LINE_WIN2_LEFT, NSHVC_LINE_WIN2_RIGHT,
    NSHVC_LINE_FIXED, NSHVC_LINE_BRIGHTNESS, NSHVC_LINE_MODE,
    NSHVC_LINE_M7_A, NSHVC_LINE_M7_B, NSHVC_LINE_M7_C, NSHVC_LINE_M7_D,
    NSHVC_LINE_M7_CENTRE_X, NSHVC_LINE_M7_CENTRE_Y, NSHVC_LINE_M7_SCROLL_X, NSHVC_LINE_M7_SCROLL_Y,
    NSHVC_LINE_M7_OUTSIDE, NSHVC_LINE_M7_FLIP_X, NSHVC_LINE_M7_FLIP_Y, NSHVC_LINE_M7_EXTBG,
    NSHVC_LINE_BLANK, NSHVC_LINE_BG3_PRIORITY, NSHVC_LINE_HIRES,
    NSHVC_LINE_SUBTRACT, NSHVC_LINE_HALF, NSHVC_LINE_USE_FIXED, NSHVC_LINE_BLEND_LAYERS, NSHVC_LINE_CLIP, NSHVC_LINE_PREVENT,
    NSHVC_LINE_MOSAIC = 64,         /* + layer */
    NSHVC_LINE_TILE_SIZE = 68,      /* + layer */
    NSHVC_LINE_WIN_USE = 72,        /* + BG1-4, sprites, colour */
    NSHVC_LINE_MASK_MAIN = 80,      /* + BG1-4, sprites */
    NSHVC_LINE_MASK_SUB = 88,       /* + BG1-4, sprites */
    NSHVC_LINE_SPRITES_MAIN = 96, NSHVC_LINE_SPRITES_SUB = 97,
    NSHVC_LINE_PALETTE = 256        /* + entry: a 15-bit BGR colour, as nshvc_palette_write's */
} nshvc_line_field;
void nshvc_line(uint32_t line, nshvc_line_field field, int32_t value);
void nshvc_line_from(uint32_t line, int32_t column, nshvc_line_field field, int32_t value);
void nshvc_lines(uint32_t first, uint32_t count, nshvc_line_field field, const int32_t *values);

/* ---- display and the window ------------------------------------------------------- */

typedef struct nshvc_display {
    int32_t width, height;          /* the window's pixels */
    float   safe_left, safe_top, safe_right, safe_bottom;   /* in pixels */
    float   refresh_hz;             /* 0: not known */
    uint8_t keyboard;               /* the player has a hardware keyboard */
} nshvc_display;
nshvc_display nshvc_display_info(void);
/* the game's UI over the picture: its image's corners top left, top right and
   bottom left, in window pixels; alpha 0-1; no image: off. RGBA images only. */
#define NSHVC_OVERLAYS 32
void nshvc_overlay_set(uint32_t id, nshvc_image i, const float corners[6], float alpha);
/* the game takes text: the system's on-screen keyboard where there is no
   keyboard; what is typed reads with nshvc_text_read */
void nshvc_text_input(int on);

#ifdef __cplusplus
}
#endif
#endif
