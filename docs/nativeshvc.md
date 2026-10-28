# nativeshvc: the console, its interface and its extensions

What a game must do and what the console promises it are fixed in
[the game contract](contract.md) (2.0); this page describes the interface
around it.

## What it is

nativeshvc is a behaviour-exact implementation of the console, not an
emulator: the hardware does what the console's does, at the console's time,
and the processors' work (the game's C) takes none (README.md, Time).

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
the reference); the renderers show it at any window size and shape with
sharp bilinear filtering, and draw the game's overlays over it.

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
`nshvc_peek`) and hooks (`nshvc_set_frame_hook`, `nshvc_set_write_hook`).

## The front end's hooks

`nshvc_set_hooks` with a key callback (return 1 to keep a key from the
console's controls), a touch callback (id, phase, window points), a
per-frame callback and an input callback (a player's native device
changed); `nshvc_player_source` (the device driving a player now: keyboard,
game controller with its family and name, mouse, touch screen, script) and
`nshvc_player_label` (what a control is called on it: "Z", "Cross", "A",
"Left button"); `nshvc_save_dir`, `nshvc_safe_area`, `nshvc_window_size`;
`nshvc_game_buttons` (buttons the game holds for a player, joined with the
controllers');
`nshvc_overlay_image` and `nshvc_overlay_place` (the game's pictures over the
console's, turned, scaled and faded, on every renderer).

## The extensions

Each is off by default.

| | |
|---|---|
| `nshvc_ext_screen(w, h)` | any screen size, 256 to 1024 by 224 to 480: the console's picture at the centre, exact; the rest drawn by the same rules (backgrounds and Mode 7 go on at their scroll, windows outside, a column its line's state, a row the nearest line's) |
| `nshvc_ext_sprite_x`, `_y` | sprites past the 9-bit X (+512) and the 256-row Y (+256) |
| `nshvc_ext_sprite_limits(0)` | no 32-sprite or 34-tile line limit (outside the console's columns there is none in any case) |
| `nshvc_ext_vram(kb)`, `_vram_page`, `_layer_page` | VRAM to 256 KB in 64 KB pages: the data port's page, each layer's |
| `nshvc_hold_interrupts` | no interrupt taken while held (an app's own work in the game's machine, from the frame hook); the console's time does not pass there, so a wait while held is a fatal error |

The game owns what shows past the console's screen: sprites it hides off the
console's screen may come into view on a larger one.

## Time

The master clock is the console's (21.477 MHz NTSC). The processors' C
takes none of it: each access is made at the clock as it stands, and the
clock moves on in the C's waits (`nshvc_wait_until`, `nshvc_wait_vblank`,
`cpu_wai`, `cpu_loop_forever`; `spc_wait_until`), in steps of a poll
loop's few cycles cut short wherever what a wait looks at changes (a line's
parts, the horizontal blank, an edge of the joypads' read, an IRQ, a chip's
IRQ), and in a DMA. The beam's lines are 1,364 clocks,
dots 323 and 327 6 clocks each (NTSC's line 240 in field 1 1,360 when not
interlaced; an interlaced frame's field 0 a line longer); the work RAM's
refresh stops the CPU 40 clocks a line. A DMA starts at its $420B write
(the C's work taking no time, the S-CPU's next cycle is the write's end);
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
