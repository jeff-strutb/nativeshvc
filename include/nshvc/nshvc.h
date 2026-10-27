/*
 * nshvc.h: nativeshvc, a native Super Nintendo for decompiled games.
 *
 * A decompiled game is the cartridge; nativeshvc is the console it plugs
 * into. The console is everything but the processors: the S-CPU's bus and
 * clock, work RAM, the cartridge's ROM and RAM in its mapping, the CPU's
 * registers (interrupts, timers, multiplier and divider, joypads), DMA and
 * HDMA, the PPU, the sound unit (its RAM, ports, timers, boot ROM protocol)
 * and the S-DSP, the Super FX's registers, RAM, bus and pixel caches; and
 * the machine's surroundings: a window, a GPU renderer, sound out, the
 * controllers, the battery save, the frame's pace. The processors are the
 * game's own C, written against the machine model in machine.h (the
 * S-CPU), gsu.h (the Super FX) and spc.h (the SPC700): every access the
 * original makes, through rd8/wr8, gsu_rd8/gsu_wr8/gsu_plot and spc_rd/
 * spc_wr, and the time between them (cyc, gsu_code, spc_cyc).
 *
 * A game describes its cartridge (nshvc_cart: its ROM and mapping, its RAM,
 * the routines with C by address) and calls nshvc_main, which runs it as
 * the console would: power on, the reset vector's code, a frame each
 * 1/60.0988 s (NTSC) shown on the GPU, the sound at the DSP's rate
 * resampled to the device's. An app that brings its own loop uses the
 * lower calls below (nshvc_power, nshvc_run_frame) instead.
 */
#ifndef NSHVC_H
#define NSHVC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NSHVC_VERSION_MAJOR 0
#define NSHVC_VERSION_MINOR 1
#define NSHVC_VERSION_PATCH 0

/* ---- the cartridge ------------------------------------------------------------ */

/* How the cartridge answers the S-CPU's 24-bit addresses. */
typedef enum {
    NSHVC_LOROM = 1,   /* ROM in 32 KB banks at $00-$7D:8000 (and $80-$FF); RAM at $70-$7D and
                          $F0-$FF:0000-$7FFF (all of each bank with 2 MB of ROM or less);
                          $40-$6F:0000-$7FFF open bus */
    NSHVC_HIROM,       /* ROM in 64 KB banks at $40-$7D, $C0-$FF (and $00-$3F:8000); RAM at $20-$3F:6000-$7FFF */
    NSHVC_EXLOROM,     /* LoROM past 4 MB: $80-$FF the first 4 MB, $00-$7D the rest */
    NSHVC_EXHIROM,     /* HiROM past 4 MB: $C0-$FF the first 4 MB, $40-$7D the rest; RAM as HiROM's */
    NSHVC_SUPERFX      /* the Super FX's board: LoROM at $00-$3F:8000, all of ROM at $40-$5F; the rest (its
                          registers, its RAM) the chip's (nshvc_cart.chip: nshvc/superfx.h) */
} nshvc_map;

typedef enum { NSHVC_NTSC = 0, NSHVC_PAL = 1 } nshvc_region;

/* Who reads a byte of ROM (for a cartridge that answers reads itself,
   nshvc_cart.rom_read). */
enum {
    NSHVC_ROM_CPU = 1, NSHVC_ROM_DMA = 2, NSHVC_ROM_PEEK = 4, NSHVC_ROM_GSU = 8,
    /* with NSHVC_ROM_GSU: the ROM buffer's fill, a byte the program may
       never use: a byte the cartridge does not hold reads 0, and sets
       nshvc_rom_unheld (gsu.h's romdr_unheld) */
    NSHVC_ROM_FETCH = 16
};
extern int nshvc_rom_unheld;

/* A routine with C, at the 24-bit address its code starts at: abi is its
   entry (machine.h's register file in, its results there). For a Super FX
   program (an address its R15 starts at, PBR:R15), flags says whether its
   first instruction is CACHE. */
enum { NSHVC_RT_GSU_CACHE_FIRST = 1 };
typedef struct {
    uint32_t addr;
    void (*abi)(void);
    const char *name;
    uint32_t flags;
} nshvc_routine;

/* An SPC700 routine with C, at an APU RAM address. Sound programs are
   uploaded, and two programs may put code at one address: uses lists the
   ROM bytes the C stands for (runs of len bytes at APU address apu, from
   ROM offset rom, ending with len 0), and the routine is the one whose
   bytes the APU's RAM holds when the SPC700 starts there. NULL: always. */
typedef struct {
    uint16_t apu, len;
    uint32_t rom;
} nshvc_spc_use;
typedef struct {
    uint16_t apu;
    void (*abi)(void);
    const char *name;
    const nshvc_spc_use *uses;
} nshvc_spc_routine;

/* the cartridge's own hardware, an expansion chip (cart.h) */
typedef struct nshvc_chip nshvc_chip;

typedef struct nshvc_cart {
    const char *title;          /* the window's and dialogs' */
    const char *save_name;      /* the save folder's and the save file's name (title when NULL) */
    nshvc_map map;
    nshvc_region region;

    /* The ROM: an image of rom_size bytes (rom); or, for a build that holds
       only the game's data (the code is the C), rom_pages[off >> 8] the 256
       bytes of each page one block holds whole (NULL elsewhere) and rom_read
       for the rest. A size that is not a power of two mirrors as the
       console's decoding does. */
    const uint8_t *rom;
    uint32_t rom_size;
    const uint8_t *const *rom_pages;
    uint8_t (*rom_read)(uint32_t off, int who);

    /* The cartridge's RAM: sram_size bytes (0 none); a chip that owns the
       RAM (its ram member: the Super FX's) takes its size from here.
       battery: kept in the save folder between runs. */
    uint32_t sram_size;
    int battery;
    /* optional: the part of the RAM the battery keeps worth writing (a
       write is made when it changes); 0, 0 for all of it */
    uint32_t save_from, save_size;

    /* The routines with C (sorted by address): the reset and interrupt
       vectors' code, every computed call's and jump's target (machine.h's
       call_routine, jump_routine), every Super FX program. */
    const nshvc_routine *routines;
    int routine_count;
    const nshvc_spc_routine *spc_routines;   /* sorted by APU address */
    int spc_routine_count;

    /* Optional: where the SPC700 starts at APU address addr after the boot
       ROM (default: spc_routines); where a Super FX program starts
       (default: routines). Neither returns until that code ends (the
       SPC700's never does; a Super FX program's at its STOP). */
    void (*spc_run)(uint16_t addr);
    void (*gsu_run)(uint8_t bank, uint16_t addr);

    /* The cartridge's expansion chip (cart.h), or NULL: the cartridge's
       own hardware, which the console's bus gives the addresses it decodes
       (the game's: it brings its chip, as nshvc/superfx.h's nshvc_superfx). */
    const nshvc_chip *chip;

    /* What is plugged into the controller ports at power on
       (NSHVC_PORT_*; 0, a joypad, when not set). */
    int ports[2];
} nshvc_cart;

/* ---- running it -------------------------------------------------------------- */

/* The console as an application: the window, renderer, sound and
   controllers, the save, and the frame's pace, until the window closes.
 *
 *   GAME                       play
 *   GAME --headless [--frames N] [--script FILE] [--out DIR] [--sound FILE]
 *                              no window and no pacing: a script of steps
 *                              (wait N, hold B+B N, press B, shot NAME,
 *                              dump NAME, repeat N ... end) from power on,
 *                              shots as DIR/NAME.ppm, dumps as DIR/NAME.wram
 *                              and DIR/NAME.sram; --sound the DSP's output
 *                              (raw 16-bit stereo, 32,040 frames a second)
 *
 * Returns the process's exit status. */
int nshvc_main(const nshvc_cart *cart, int argc, char **argv);

/* The game's main: write NSHVC_MAIN(argc, argv) { ... nshvc_main(...) } in
   place of int main(...). On iOS the host keeps the process's main (UIKit's)
   and runs the game's on a thread of its own as nshvc_game_main; elsewhere
   it is main. */
#ifdef NSHVC_HOST_IOS
#define NSHVC_MAIN(argc, argv) int nshvc_game_main(int argc, char **argv)
#else
#define NSHVC_MAIN(argc, argv) int main(int argc, char **argv)
#endif

/* The console without its surroundings, for an app that has its own loop
   (a libretro core, an engine): power on, then each call to
   nshvc_run_frame runs the game until its next frame is finished (the
   vertical blank's start). The S-CPU's C runs on a coroutine of its own;
   nshvc_run_frame returns between two of its accesses. */
void nshvc_power(const nshvc_cart *cart);
void nshvc_run_frame(void);

/* the finished frame: width x height pixels, 0xFFRRGGBB, top row first:
   256 by 224 (239 with overscan), or the size nshvc_ext_screen asked for;
   twice as wide in a frame with a hires line */
const uint32_t *nshvc_picture(int *width, int *height);
/* frames finished since power on */
uint32_t nshvc_frames(void);

/* The controller ports (0 and 1, the console's 1 and 2) and what is
   plugged into each: a joypad in each unless the cartridge says otherwise
   (nshvc_cart.ports), or nshvc_plug changes it. Each device answers the
   ports' lines as the device does. */
enum {
    NSHVC_PORT_JOYPAD,          /* the standard controller */
    NSHVC_PORT_NONE,            /* nothing: the data lines read 0 */
    NSHVC_PORT_MOUSE,           /* the SNES mouse */
    NSHVC_PORT_MULTITAP,        /* four joypads on one port */
    NSHVC_PORT_SUPERSCOPE,      /* the Super Scope (its aim latches the counters on port 1) */
    NSHVC_PORT_JUSTIFIER,       /* one Justifier (the same) */
    NSHVC_PORT_JUSTIFIERS       /* two Justifiers, chained */
};
void nshvc_plug(int port, int device);
int nshvc_plugged(int port);
/* What a joypad's player holds, the joypad's own bits. The players are
   the controllers in port order, each port's device one player (a
   multitap four): port 0's first, so with a joypad in each, 0 and 1. */
enum {
    NSHVC_B = 0x8000, NSHVC_Y = 0x4000, NSHVC_SELECT = 0x2000, NSHVC_START = 0x1000,
    NSHVC_UP = 0x0800, NSHVC_DOWN = 0x0400, NSHVC_LEFT = 0x0200, NSHVC_RIGHT = 0x0100,
    NSHVC_A = 0x0080, NSHVC_X = 0x0040, NSHVC_L = 0x0020, NSHVC_R = 0x0010
};
void nshvc_set_joypad(int pad, uint16_t buttons);
/* A mouse on `port`: this frame's motion (in the mouse's counts, x right
   and y down), which it reports at each latch until the frame ends;
   buttons bit 0 left, bit 1 right. */
void nshvc_set_mouse(int port, int dx, int dy, int buttons);
/* A light gun: where it points, in the console's dots and lines (outside
   0-255 and the picture's lines: off the screen), and its buttons. Gun 0
   is the Super Scope or the first Justifier, gun 1 the second. */
enum {
    NSHVC_GUN_TRIGGER = 1, NSHVC_GUN_CURSOR = 2, NSHVC_GUN_TURBO = 4, NSHVC_GUN_PAUSE = 8,
    NSHVC_GUN_START = 16
};
void nshvc_set_gun(int gun, int x, int y, unsigned buttons);

/* The native device driving a player's controller now: the last one the
   player used (the console's front end maps keyboards, game controllers,
   mice and touch screens onto every controller above), so a game can
   name its controls as the player sees them. nshvc_player_source: 0 when
   there is no such player. nshvc_player_label: the name of the control
   that makes one of the player's controls (a joypad's NSHVC_B..., a light
   gun's NSHVC_GUN_..., a mouse's button 1 or 2) on that device ("Z",
   "Cross", "A", "Left button"; "" none). nshvc_hooks.input hears each
   change. */
enum {
    NSHVC_INPUT_NONE, NSHVC_INPUT_KEYBOARD, NSHVC_INPUT_GAMEPAD, NSHVC_INPUT_MOUSE, NSHVC_INPUT_TOUCH,
    NSHVC_INPUT_SCRIPT
};
enum { NSHVC_PAD_GENERIC, NSHVC_PAD_XBOX, NSHVC_PAD_PLAYSTATION, NSHVC_PAD_NINTENDO };
typedef struct {
    int kind;                   /* NSHVC_INPUT_* */
    int style;                  /* a game controller's family, NSHVC_PAD_* */
    char name[64];              /* its product's name, or the kind's ("Keyboard") */
} nshvc_input_source;
int nshvc_player_source(int player, nshvc_input_source *src);
const char *nshvc_player_label(int player, unsigned control);

/* The sound: the DSP's output, stereo 16-bit (left, right) at
   NSHVC_DSP_RATE frames a second, made as the SPC700's clock runs; up to
   max frames into lr, the number taken. */
#define NSHVC_DSP_RATE 32040
int nshvc_audio_take(int16_t *lr, int max);
int nshvc_audio_queued(void);

/* The machine's memory, for an app's checks and saves: work RAM (128 KB),
   the cartridge's RAM (nshvc_cart.sram_size), the APU's RAM (64 KB), the
   PPU's VRAM (64 KB), CGRAM (512 B) and OAM (544 B). */
uint8_t *nshvc_wram(void);
uint8_t *nshvc_sram(void);
const uint8_t *nshvc_aram(void);
const uint16_t *nshvc_vram(void);
const uint16_t *nshvc_cgram(void);
const uint8_t *nshvc_oam(void);

/* A read of the S-CPU's bus with no time and no side effects. */
uint8_t nshvc_peek(uint32_t addr);

/* Hooks for an app's checks (NULL: none). frame: each finished frame,
   inside the S-CPU's run (before nshvc_run_frame returns); write: each
   S-CPU write, before it lands. */
void nshvc_set_frame_hook(void (*fn)(void));
void nshvc_set_write_hook(void (*fn)(uint32_t addr, uint8_t value));

/* ---- extensions: past the console's limits, each off by default ---------------- */

/* Untimed: the S-CPU's code runs without spending the console's time (its
   accesses and stated cycles take none), so a stretch of the game's work
   takes no frames at all; interrupt handlers stay timed, but for code run
   in one with interrupts held (nshvc_hold_interrupts: from the frame hook). A Super FX
   program started meanwhile runs to its STOP at once (the S-CPU's next
   look at it). A restart (machine.h cpu_restart: the stack reset, every
   call dropped) ends it. Waiting on time while untimed is a fatal error:
   20,000,000 reads with no write between, the Super FX waiting for its
   bus, a loop for ever. Off (the default), the console is exact. */
void nshvc_set_untimed(int on);
int nshvc_untimed(void);   /* untimed as it stands now */
/* The clock run on, timed, to the next vertical blank's start (the NMI,
   the joypads' read and the frame's end come on the way as they would),
   then untimed put back as it was. */
void nshvc_wait_vblank(void);

/* The screen the game asks for, from the next frame: width 256 to 1024 and
   height 224 to 480, both even; 256 x 224 (or 239 with SETINI's overscan)
   is the console's own. The console's picture sits at its centre (its
   column 0 at (width - 256) / 2, its line 1 at (height - 224) / 2, or 239
   with overscan), drawn exactly as on the console; the rest of the screen
   is drawn by the same rules, past the console's edges: the backgrounds
   and Mode 7 go on at their scroll, windows are outside both windows,
   colour maths and brightness as on the line; a column carries its line's
   state (a change in the line included), a row above or below the
   console's lines the nearest line's. Sprites: their 9-bit X covers -256
   to 255, nshvc_ext_sprite_x adds 512; their Y wraps in 256 rows as the
   console's does, and rows past those 256 show only the sprites
   nshvc_ext_sprite_y puts 256 lower. The game owns what shows there: the
   sprites it hides off the console's screen may come into view on a
   larger one, so it hides them past this one. No timing changes: the
   frame has the console's lines and time. */
void nshvc_ext_screen(int width, int height);
void nshvc_ext_sprite_x(int sprite, int on);
void nshvc_ext_sprite_y(int sprite, int on);
/* The line's sprite limits, the console's 32 sprites and 34 tiles in its
   256 columns: on (the default) or lifted (every sprite in range drawn
   whole). STAT77's range and time over flags report the console's limits
   either way. */
void nshvc_ext_sprite_limits(int on);

/* VRAM past the console's 64 KB: kb 64 to 256, in 64 KB pages (from now;
   64, the default, is the console's). The data port ($2118-$2119, $2139-
   $213A, DMA to them) reads and writes the page nshvc_ext_vram_page names;
   each layer (0-3 BG1-4, Mode 7 BG1's; 4 the sprites) reads every address
   it makes in the page nshvc_ext_layer_page names, from the next line
   drawn. Page 0 throughout is the console. */
void nshvc_ext_vram(int kb);
void nshvc_ext_vram_page(int page);
void nshvc_ext_layer_page(int layer, int page);
/* The same pages as registers on the B bus, for the game's code and its
   HDMA (a page per line, as any of the PPU's registers): $21C0-$21C3
   BG1-4's, $21C4 the sprites', $21C5 the data port's. With VRAM at the
   console's 64 KB they take no notice of writes (the console's open bus
   there). */

/* Interrupts held: while on, none is taken; those that come due (the
   NMI, the IRQ) wait, and are taken at the first place one can be after
   the hold ends. For code an app runs in the game's machine outside the
   game's own flow (from the frame hook: an in-between frame drawn, the
   machine put back after), which the game's handlers must not see. The
   Super FX's interrupt (SFR's IRQ bit, the cartridge's IRQ line) is put
   back as the hold ends to what it was as the hold began. */
void nshvc_hold_interrupts(int on);

/* ---- the front end's hooks, for the game's own additions ------------------------- */
/* A touch on a touch screen: its id, phase (0 began, 1 moved, 2 ended), and
   where, in the window's points (0, 0 its top left). */
typedef struct {
    int id, phase;
    float x, y;
} nshvc_touch;
typedef struct {
    /* a key (a Win32 virtual key code on every host): 1 if the game took
       it (the console's controls then do not see it) */
    int (*key)(int vk, int down);
    /* the touches that changed, n of them */
    void (*touch)(const nshvc_touch *t, int n);
    /* once a frame, after it is shown, on the front end's side */
    void (*frame)(void);
    /* the device driving `player` changed (nshvc_player_source) */
    void (*input)(int player, const nshvc_input_source *src);
} nshvc_hooks;
void nshvc_set_hooks(const nshvc_hooks *hooks);
/* the host's folder for the player's saves and settings */
const char *nshvc_save_dir(void);
/* the window's edges a screen's notch or rounded corners cover, in points
   (0 where nothing does) */
void nshvc_safe_area(float *left, float *top, float *right, float *bottom);
/* the window's size in points (headless: the renderer's target, a point a
   pixel) */
void nshvc_window_size(float *w, float *h);
/* Buttons the game holds for a joypad's player (a touch scheme's), the
   joypad's bits: kept until changed, joined with the console's own
   controls (keyboard, game controllers, a script) at each frame. */
void nshvc_game_buttons(int player, uint16_t buttons);
/* Pictures the game draws over the console's (a touch screen's controls):
   an image with alpha (0xAARRGGBB, straight alpha), kept by the console;
   its id, or -1 when NSHVC_OVERLAYS are made. Placed by its centre (cx,
   cy) in window points, scale (points an image pixel), turned by angle
   (radians, clockwise), its alpha times alpha (0-1), shown or not; drawn
   after the picture, in the order made, from the next frame shown. */
#define NSHVC_OVERLAYS 16
int nshvc_overlay_image(const uint32_t *argb, int w, int h);
void nshvc_overlay_place(int id, float cx, float cy, float scale, float angle_rad, float alpha, int visible);

/* A fatal error in the port (the game reached code with no C, a state the
   console cannot be in): reported, then the process exits (status 70). */
void nshvc_fatal(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif
