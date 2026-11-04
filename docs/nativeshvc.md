# nativeshvc: the console, its interface and its extensions

What a game must do and what the console promises it are fixed in
[the game contract](contract.md) (2.13); this page describes the interface
around it.

## What it is

nativeshvc is a behaviour-exact implementation of the console, not an
emulator: the hardware does what the console's does, at the console's time;
the processors are the game's C, each access it makes costing the bus's
time and its cost profile's weight. A game whose pace is how much work fits
in a frame has an Original mode, paced on its work by a cost profile
(approximate), and an Enhanced mode made independent of processor speed
(exact) (README.md, Enhancement support; cost-profile.md).

A decompiled game is the cartridge; nativeshvc is the console. The
processors are the game's own C, written against the machine model
(`include/nshvc/machine.h`, `spc.h`; a cartridge chip's processor, the
game's own model with its chip). Everything else is
nativeshvc: the S-CPU's bus and clock, work RAM, the cartridge's mapping,
ROM and RAM, the CPU's registers (interrupts, timers, multiplier, divider,
joypads), DMA and HDMA, the PPU, the sound unit (RAM, ports, timers, boot
ROM protocol) and the DSP, and the cartridge connector an expansion chip
plugs into (`include/nshvc/cart.h`); and the machine's surroundings (window, renderer, sound out,
controllers, touch, the battery save, the frame's pace) on macOS, Windows,
iOS and headless.

The console's own numbers are the baseline, not hard limits: at its own
settings nativeshvc is the console, its hardware checked against the
reference read for read and pixel for pixel, to the clock; past them, the extensions lift each limit the
same way the console's own registers work, and a game asks for each.

## Plugging a game in

```c
#include "nshvc/nshvc.h"

NSHVC_MAIN(argc, argv)
{
    static nshvc_cart cart = {
        .title = "Game", .map = NSHVC_LOROM, .rom = rom, .rom_size = sizeof rom,
        .sram_size = 0x2000, .battery = 1,
        .routines = routines, .routine_count = ROUTINE_COUNT,
    };
    return nshvc_main(&cart, argc, argv);
}
```

`examples/hello/hello.c` is a whole cartridge in 70 lines. Build:

```bash
cmake -S . -B build/macos -DNSHVC_HOST=macos -DNSHVC_RENDER=metal && cmake --build build/macos
```

| host (`NSHVC_HOST`) | renderer (`NSHVC_RENDER`) |
|---|---|
| `null` (headless) | `soft` |
| `macos` | `metal` (default), `gl`, `vulkan` |
| `windows` | `gl` (default), `vulkan` |
| `ios` (`-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64`) | `metal` |
| `none` (the app supplies `src/host/host.h`) | any |

The picture comes from the PPU in C (the implementation the checks hold to
the reference); the renderers draw it at the window's own resolution, at any
size and shape: the pixel art with sharp bilinear filtering, the Mode 7
plane looked up at each window pixel and anti-aliased, a layer's canvas (an
extension) at the window's resolution (The picture, below), and the game's
overlays over it.

## The cartridge (`nshvc_cart`)

| field | |
|---|---|
| `map` | `NSHVC_LOROM`, `HIROM`, `EXLOROM`, `EXHIROM`; `NSHVC_CHIP` for a board its chip decodes (the chip's `rom_at` says where its ROM is) |
| `region` | `NSHVC_NTSC`, `NSHVC_PAL` |
| `rom`, `rom_size` | the ROM image; or `rom_pages` and `rom_read` for a build that holds only the game's data (the code is the C) |
| `sram_size`, `battery` | the cartridge's RAM (a chip's, when the chip owns it), kept in the save folder; `save_from`, `save_size` narrow what is written |
| `routines` | every S-CPU routine reached by address (vectors, computed calls and jumps) and any a chip runs by address, sorted by address |
| `spc_routines` | the SPC700 routines by APU address, with the ROM bytes each stands for (`uses`) |
| `spc_run` | optional: the game's own entry in place of the lookup |
| `chip` | the cartridge's expansion chip, or none: the cartridge's own hardware, which the game brings and owns, on the connector (`nshvc/cart.h`). nativeshvc implements no chip |
| `ports` | what is plugged into the controller ports at power on (`NSHVC_PORT_*`; a joypad when 0) |

An interrupt vector's code is a routine with C (the console makes the
interrupt's sequence and the RTI), wherever it is: a short stub in RAM or
ROM (a JML to the handler) is code like any other, with C of its own.
nativeshvc runs no machine code.

## Running it yourself

`nshvc_power(&cart)`, then `nshvc_run_frame()` once a frame; the picture
(`nshvc_picture`), the sound (`nshvc_audio_take`, 32,040 frames a second),
the controllers (`nshvc_plug` and `nshvc_plugged` for what is in each port;
`nshvc_set_joypad` for each player's joypad, `nshvc_set_mouse` for a mouse's
motion this frame and its buttons, `nshvc_set_gun` for where a light gun points
and its buttons), the memories (`nshvc_wram`,
`nshvc_sram`, `nshvc_aram`, `nshvc_vram`, `nshvc_cgram`, `nshvc_oam`,
`nshvc_peek`) and hooks (`nshvc_set_frame_hook`, `nshvc_set_write_hook`,
`nshvc_set_read_hook`).

## The front end's hooks

`nshvc_set_hooks` with a key callback (return 1 to keep a key from the
console's controls), a touch callback (id, phase, window points), a
per-frame callback, an input callback (a player's native device
changed) and a pointer callback (the mouse in window points; return 1 to
keep it from the console's mouse and guns); `nshvc_player_source` (the device driving a player now: keyboard,
game controller with its family and name, mouse, touch screen, script) and
`nshvc_player_label` (what a control is called on it: "Z", "Cross", "A",
"Left button"); `nshvc_save_dir`, `nshvc_safe_area`, `nshvc_window_size`;
`nshvc_ext_text_input` (the system's on-screen keyboard on a touch screen
with no hardware keyboard, while on: letters, digits, space, Backspace and
Return reach the key hook as virtual keys, a press then a release) and
`nshvc_keyboard_attached`;
`nshvc_game_buttons` (buttons the game holds for a player, joined with the
controllers');
`nshvc_overlay_image`, `nshvc_overlay_place` and `nshvc_overlay_update` (the
game's pictures over the console's, turned, scaled and faded, redrawn, on
every renderer); `nshvc_ext_pause` (the machine stopped between frames, its
sound silent, the last frame shown blurred and tinted under the game's
pictures, which with input and the hooks go on) and `nshvc_ext_paused`;
`nshvc_ext_key_for`, `nshvc_ext_key_bind` and `nshvc_ext_key_name` (player
0's keyboard keys as the game lays them out).

## The extensions

Each is off by default.

| | |
|---|---|
| `nshvc_ext_screen(w, h)` | any screen size, 256 to 8192 by 224 to 8192: the console's picture at the centre, exact; the rest drawn by the same rules (backgrounds and Mode 7 go on at their scroll, windows outside, a column its line's state, a row the nearest line's) |
| `nshvc_ext_screen_origin(x, y)` | the console's picture at column x and row y of that screen instead of its centre (-1: the centre), held at its right and bottom edges; sprites from the picture's column 0 and line 1 |
| `nshvc_ext_sprite_page(s, x, y)` | a sprite 512 columns (pages -16 to 16) and 256 rows (pages -32 to 32) from its place, reaching the whole of the largest screen |
| `nshvc_ext_sprite_x`, `_y` | X or Y page 1: sprites past the 9-bit X (+512) and the 256-row Y (+256) |
| `nshvc_ext_layer_outside(layer, map, size)` | a background's map past the console's 256 columns and lines, its entries in BGnSC's layout and sizes, or 128 x 32 and 128 x 64 in rows of 128 (copied, from the next frame); everything else the layer's |
| `nshvc_ext_canvas_screen(layer, texels, w, h, k, priority, palette)` | a background's picture over the whole screen from the game's texels by the screen's pixels, one priority and palette, at the window's resolution (one canvas at a time) |
| `nshvc_ext_layer_clip(layer, on)` | a background or the sprites kept to the console's picture on a larger screen: transparent past its 256 columns and lines, a sprite across the edge cut at it |
| `nshvc_ext_window_edges(on)` | the windows on a larger screen: a window whose left edge is 0 reaches every column left of the console's picture, one whose right edge is 255 every column right of it, so a window spanning the console's line spans the screen (off: past the console's edges a column is outside both windows) |
| `nshvc_ext_window_map(on, scale_q16, offset)` | the windows placed on a larger screen: each line's edges (WH0-WH3, HDMA's included) mapped by console column c to c * scale_q16 / 65536 (rounded down) + offset, a window from L to R covering L's column up to the one before R + 1's, on every layer, the sprites and the colour window; (65536, (W - 256) / 2) is the console's windows, (W * 256, 0) stretches them over the screen; over `nshvc_ext_window_edges` while on |
| `nshvc_ext_window_columns(window, left, right)` | window 1 or 2 from screen column left to right on every line, in place of its registers, the map and the edges (left past right: none; left -1: the registers again) |
| `nshvc_ext_sprite_wrap(0)` | Y page 0's sprites nowhere above the console's line 1 on a taller screen (in its own 256 rows they wrap as ever) |
| `nshvc_ext_sprite_limits(0)` | no 32-sprite or 34-tile line limit (outside the console's columns there is none in any case) |
| `nshvc_ext_vram(kb)`, `_vram_page`, `_layer_page` | VRAM to 256 KB in 64 KB pages: the data port's page, each layer's |
| `nshvc_ext_vram_copy(to, from)`, `_vram_direct(page)` | pages 1-3 written outside the console's bus and time, at any moment, as a board whose extra VRAM is the cartridge's too: a whole page copied, or the data port's writes (by the S-CPU or a DMA) landing in a page at once, while lines are drawn too, a DMA of only those taking no time. Page 0 is the console's, written only through the bus. A change to a page a layer reads shows from the next line drawn; one to a page no layer reads, from when a layer is switched to it (so a game builds its next screen in a page not shown, and switches) |
| `nshvc_ext_canvas_chars(layer, word_addr, count, texels, k)`, `_canvas_scale()` | a canvas: layer's (BG1-4) characters whose data starts in VRAM at word_addr (below `$8000` in the layer's page; from `$8000`, `page << 15 \| word` in VRAM's whole space, so one canvas covers a layer whose lines read several pages), count (to 4,096) of them at the layer's colour depth, drawn from the game's texels, k a console pixel along each axis (each character 8k x 8k, a texel a pixel value as a tile's, 0 transparent). Wherever the layer's map names one, the console places it by its own rules (map, scroll, flips, priority, palette, windows, colour maths, brightness), and at the window's resolution each window pixel takes the texel under its centre in its console pixel; the console's own picture takes each pixel's centre texel. `_canvas_scale`: the k for the window now (its pixels a console pixel, rounded up). A frame shows the canvas as it stood when the frame began; a hires line or the layer under mosaic shows its tiles. NULL turns it off |
| `nshvc_hold_interrupts` | no interrupt taken while held (an app's own work in the game's machine, from the frame hook); the console's time does not pass there, so a wait while held is a fatal error |

The game owns what shows past the console's screen: sprites it hides off the
console's screen may come into view on a larger one.

## Time

The master clock is the console's (21.477 MHz NTSC). Each access the
S-CPU's C makes costs the bus's time for it (8 clocks for SlowROM and work
RAM, 6 for FastROM and most registers, 12 for $4000-$41FF, a read taking
its value 4 clocks before its end, a write landing at its end), and before
it the weight of its kind from the cartridge's cost profile (0 with none:
cost-profile.md); while `nshvc_set_cpu_costs(0)` has the costs off, its
accesses take no time at all. Each access the SPC700's C makes costs its one
bus cycle and its kind's weight from the profile. The clock also moves on in the
C's waits (`nshvc_wait_until`, `nshvc_wait_vblank`,
`cpu_wai`, `cpu_loop_forever`; `spc_wait_until`), in steps of a poll
loop's few cycles cut short wherever what a wait looks at changes (a line's
parts, the horizontal blank, an edge of the joypads' read, an IRQ, a chip's
IRQ), and in a DMA. The beam's lines are 1,364 clocks,
dots 323 and 327 6 clocks each (NTSC's line 240 in field 1 1,360 when not
interlaced; an interlaced frame's field 0 a line longer); the work RAM's
refresh stops the CPU 40 clocks a line. A DMA starts at its $420B write
(the write's end);
HDMA (the frame's setup in line 0, a transfer in each shown line's
horizontal blank) is taken up at the start of a wait's next cycle after it
comes due and made at the one after, or between a DMA's bytes, where HDMA
stops a DMA on its own channel. The unit waits for its 8-clock step, takes
8 clocks to start, 8 a channel and 8 a byte (read 4 clocks in, written at
the end) and an HDMA table byte each line, and gives the bus back on the
CPU's cycle. Interrupts are taken in a wait at the clock they come due, or
at the end of the access whose DMA made them due, the IRQ unless the I
flag masks it (README.md, Time).
The sound unit runs
from power on with the console's clock (the S-CPU starts after its reset);
each side sees an access of the other's made at or before its own time, a
port read taking its value half an SPC700 cycle before its cycle ends.

## The picture

Each line is recorded as the beam passes its horizontal blank. A register
write made while a line is being drawn shows from the pixel the beam has
reached, as on the console: the output stage's registers (brightness, the
screens, windows, colour maths, colours) from pixel (clock - 52) / 4; the
backgrounds' maps, characters and scroll from the first tile whose map
entry is fetched at or after it (a tile each 32 clocks, each layer's entry
in its own slot of them by the mode), the scroll's fine part as the line
began. Mode 7's
matrix and scroll follow the output stage's rule. A CGRAM access made while
the screen is drawn goes to the entry the PPU is looking up (the main
screen's pixel at (clock - 55) / 4). VRAM writes during the display are
lost. An interlaced frame (SETINI bit 0 as it began) draws its field's rows
of a picture twice as tall, woven with the frame before's; hires lines read
two map rows a line (the field's), and OBJ interlace draws each sprite in
half its lines, every other row. These were measured against the reference
with probe cartridges (`tests/rom/cases.py`'s `probe_*`); nothing is known
to differ (`tests/README.md`).

On the window, a frame with Mode 7 is drawn at the window's own
resolution, the same in a game's Original and Enhanced modes. Each window
pixel over a Mode 7 line looks the plane up through that line's matrix at
the pixel's own position, and the console pixel under it is composed again
by its line's rules with that texel (its sprites, windows, colour maths,
brightness); the sprites, the other backgrounds and the windows stay the
console pixel's. The plane is anti-aliased by the pixel's footprint on it
(the texels it spans, from the matrix): within a texel, the texel under the
pixel's centre, blended with the next only across the one window pixel a
texel edge falls in, by the part of the pixel's footprint past the edge;
beyond, 4, 8 or 16 samples at the standard positions. Then the console's
pixels are blended across their edges with sharp bilinear, as the pixel art
is. The definition is `src/render/rr_mode7.h`, the software backend's
drawing; the Metal, OpenGL and Vulkan shaders compute the same, in integers
but the blends (`NSHVC_RRCHECK` holds each to it). `nshvc_picture` stays the
console's picture. A layer's canvas (`nshvc_ext_canvas_chars`, an
extension) is drawn the same way: each window pixel over one of its
characters takes the canvas's texel under its centre in its console pixel,
composed by that pixel's rules, and `nshvc_picture` takes each pixel's
centre texel. A headless run's shots are the console's picture, or with `NSHVC_SCALE=S`
(or `SXxSY`) the frame as a window that many pixels a console pixel shows
it, the canvas's scale (`nshvc_ext_canvas_scale`) that window's from power
on.
