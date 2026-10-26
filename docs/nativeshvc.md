# nativeshvc: the console, its interface and its extensions

## What it is

A decompiled game is the cartridge; nativeshvc is the console. The
processors are the game's own C, written against the machine model
(`include/nshvc/machine.h`, `gsu.h`, `spc.h`). Everything else is
nativeshvc: the S-CPU's bus and clock, work RAM, the cartridge's mapping,
ROM and RAM, the CPU's registers (interrupts, timers, multiplier, divider,
joypads), DMA and HDMA, the PPU, the sound unit (RAM, ports, timers, boot
ROM protocol) and the DSP, the Super FX's registers, RAM, bus, caches and
clock; and the machine's surroundings (window, renderer, sound out,
controllers, touch, the battery save, the frame's pace) on macOS, Windows,
iOS and headless.

The console's own numbers are the baseline, not hard limits: at its own
settings nativeshvc is the console, checked against the reference pixel for
pixel and cycle for cycle; past them, the extensions lift each limit the
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
| `map` | `NSHVC_LOROM`, `HIROM`, `EXLOROM`, `EXHIROM`, `SUPERFX` |
| `region` | `NSHVC_NTSC`, `NSHVC_PAL` |
| `rom`, `rom_size` | the ROM image; or `rom_pages` and `rom_read` for a build that holds only the game's data (the code is the C) |
| `sram_size`, `battery` | the cartridge's RAM (the Super FX's on a Super FX cartridge), kept in the save folder; `save_from`, `save_size` narrow what is written |
| `routines` | every S-CPU routine reached by address (vectors, computed calls and jumps) and every Super FX program, sorted by address |
| `spc_routines` | the SPC700 routines by APU address, with the ROM bytes each stands for (`uses`) |
| `spc_run`, `gsu_run` | optional: the game's own entry in place of the lookup |

An interrupt vector's code is a routine with C (the console makes the
interrupt's sequence and the RTI), or a short stub in RAM or ROM (NOPs, JMP,
JML, JSL to a routine with C, RTI), which the console runs as the CPU does.

## Running it yourself

`nshvc_power(&cart)`, then `nshvc_run_frame()` once a frame; the picture
(`nshvc_picture`), the sound (`nshvc_audio_take`, 32,040 frames a second),
the controllers (`nshvc_set_joypad`), the memories (`nshvc_wram`,
`nshvc_sram`, `nshvc_aram`, `nshvc_vram`, `nshvc_cgram`, `nshvc_oam`,
`nshvc_peek`) and hooks (`nshvc_set_frame_hook`, `nshvc_set_write_hook`).

## The front end's hooks

`nshvc_set_hooks` with a key callback (return 1 to keep a key from the
console's controls), a touch callback (id, phase, window points) and a
per-frame callback; `nshvc_save_dir`, `nshvc_safe_area`, `nshvc_window_size`;
`nshvc_game_buttons` (buttons the game holds, joined with the controllers');
`nshvc_overlay_image` and `nshvc_overlay_place` (the game's pictures over the
console's, turned, scaled and faded, on every renderer).

## The extensions

Each is off by default; off, the console is exact.

| | |
|---|---|
| `nshvc_ext_screen(w, h)` | any screen size, 256 to 1024 by 224 to 480: the console's picture at the centre, exact; the rest drawn by the same rules (backgrounds and Mode 7 go on at their scroll, windows outside, a column its line's state, a row the nearest line's) |
| `nshvc_ext_sprite_x`, `_y` | sprites past the 9-bit X (+512) and the 256-row Y (+256) |
| `nshvc_ext_sprite_limits(0)` | no 32-sprite or 34-tile line limit (outside the console's columns there is none in any case) |
| `nshvc_ext_vram(kb)`, `_vram_page`, `_layer_page` | VRAM to 256 KB in 64 KB pages: the data port's page, each layer's |
| `nshvc_ext_gsu_screen(w, h)`, `gsu_plot_at`, `gsu_rpix_at` | the Super FX's screen to 1024 x 512, at 16-bit coordinates, in its own character layout; its RAM to 896 KB (`sram_size`) at $70-$7D |
| `nshvc_set_untimed`, `nshvc_wait_vblank` | the S-CPU's work taking no time (a frame's work at once); interrupt handlers stay timed; a wait on time untimed is a fatal error |
| `nshvc_hold_interrupts` | no interrupt taken while held (an app's own work in the game's machine, from the frame hook) |
| `cpu_wai` (machine.h) | WAI |

The game owns what shows past the console's screen: sprites it hides off the
console's screen may come into view on a larger one.

## Time

The master clock is the console's (21.477 MHz NTSC). Each access takes the
bus's time for it (8 clocks for SlowROM and work RAM, 6 for FastROM and
most registers, 12 for $4000-$41FF); the C states the time between (`cyc`,
`gsu_code`, `spc_cyc`). The beam's lines are 1,364 clocks, dots 323 and 327
6 clocks each; the work RAM's refresh stops the CPU 40 clocks a line; DMA
takes 8 clocks a byte, HDMA runs in each line's horizontal blank. Interrupts
are taken where the S-CPU's C touches the bus, never between the two bytes
of one word; an H-IRQ past a line's end comes early in the next.

## The picture

Each line is recorded as the beam passes its horizontal blank. A register
write made while a line is being drawn shows from the pixel the beam has
reached, as on the console: the output stage's registers (brightness, the
screens, windows, colour maths, colours) from pixel (clock - 52) / 4; the
backgrounds' maps, characters and scroll from the next tile fetched (a tile
each 32 clocks from clock 6), the scroll's fine part as the line began. A
CGRAM write made while the screen is drawn lands at the entry the PPU is
looking up (the main screen's pixel at (clock - 60) / 4). VRAM writes during
the display are lost. These were measured against the reference with probe
cartridges (`tests/rom/cases.py`'s `probe_*`); the one residual is in
`tests/README.md`.
