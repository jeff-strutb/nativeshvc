# nativeshvc: a native Super Nintendo for decompiled games

**A decompiled game is the cartridge; nativeshvc is the console it plugs into.** A game's
decompiled C, linked against nativeshvc, builds a native program that powers on, runs the
reset vector's code and plays as it did on the console: the same picture, the same sound,
the same pace, with no emulated processor and no ROM image of the code.

**Maintainer:** Jeffrey Wilbur, Strut B, LLC\
**Contact:** [retro@strutb.com](mailto:retro@strutb.com)

nativeshvc was created by Jeffrey Wilbur. It began as the machine layer of the native
[Stunt Race FX](https://github.com/jeff-strutb/stuntracefx) port, and is being lifted out of
it so that every Super Nintendo decompilation can share one console instead of building its
own.

## Status

Version 0.1.0. **The library is not yet usable on its own.** What is not done:

- **No build.** The tree has no Makefile or CMake project yet; it does not compile by itself.
- **The public API is declared, not implemented.** [`include/nshvc/nshvc.h`](include/nshvc/nshvc.h)
  sets out the cartridge description and the calls (`nshvc_main`, `nshvc_power`,
  `nshvc_run_frame` and the rest), but none of them is defined in `src/` yet.
- **Still shaped by Stunt Race FX.** The console code comes from that port and still includes
  its generated headers (`cart.h`, `routines.h`, `dispatch.h`, `ppu.h`, the render's
  `fill_src.h`, Windows' `game_rc.h`), which are not in this tree. `src/front/main.c` is
  still Stunt Race FX's front end, and `src/render/check/check.sh` still uses the port's
  paths.
- **One cartridge kind so far.** The bus (`src/console/bus.c`) answers only the Super FX's
  LoROM layout, NTSC, SlowROM timing. LoROM, HiROM, ExLoROM, ExHiROM and PAL, all named in
  `nshvc.h`, are still to be written.

What is in the tree and was proven in the Stunt Race FX port: the S-CPU's bus and clock, DMA
and HDMA, the Super FX and its clock, the sound unit and its boot ROM protocol, the PPU's
reference drawing and the GPU compositor, the mixer and resampler, the renderer backends
and the OS hosts.

## Project Purpose

A decompilation that is byte exact and cycle exact recovers the game's code, but code alone
does not play. On the console it sat in a machine: a bus that answers its addresses, a
picture unit drawing what it wrote, a sound chip, DMA, interrupts, a controller, a battery,
a television's frame rate. Each native port has had to rebuild all of that around its game.

nativeshvc builds it once. Its purpose is the equivalent of putting a cartridge in the
console and switching it on: a decompilation describes its cartridge, hands over its C, and
gets a native program that plays on macOS, Windows, Linux and iOS, drawn on the GPU at any
window size, with sound, controllers and saves, at the console's own pace.

It holds to the standard of the decompilations it serves. It runs no emulated CPU, does no
high-level emulation of the game, and has no per-game fixes. The game's own C makes every
access the original makes, in the original's order, and states the time the original spends
between them; nativeshvc answers each access as the hardware does, at the hardware's time.
A game whose C is cycle exact therefore runs on nativeshvc frame for frame as it ran on the
console.

## How it works

### The split: processors in the game, everything else in the console

The processors are the game's C. Everything that is not a processor is nativeshvc:

| The game supplies | nativeshvc supplies |
|---|---|
| The S-CPU's (65816) code, as C | The S-CPU's bus and clock, work RAM, the CPU's registers (interrupts, timers, multiplier and divider, joypads) |
| The Super FX's (GSU) programs, as C | The Super FX's registers, RAM, bus, code cache and pixel caches, and its clock |
| The SPC700's sound driver, as C | The sound unit: 64 KB of APU RAM, its ports and timers, the boot ROM protocol, the S-DSP |
| The ROM's data (or the whole ROM) | The cartridge's mapping, ROM and battery RAM |
| | DMA and HDMA, the PPU, the window, the GPU renderer, sound output, controllers, the save, the frame's pace |

The C is written against the machine model in three headers, the same model the
decompilations' verification harnesses provide. Inside a harness these calls reach an
emulated machine and are checked against the original; under nativeshvc they reach the
native console.

- [`machine.h`](include/nshvc/machine.h), the S-CPU: `rd8`/`wr8` (and `rd16`, `wr16`,
  `rd24`) for every access to the 24-bit address space; `cyc(clocks)` for the original's
  fetches and internal cycles between two accesses, in master clocks; the 65816 register
  file `cpu` for routines entered through `abi_` wrappers; `call_routine` and
  `jump_routine` for calls and jumps through computed addresses; `cpu_loop_forever`,
  `cpu_restart` and `cpu_idle` for the code no C returns from.
- [`gsu.h`](include/nshvc/gsu.h), the Super FX: the register file `gsu`; `gsu_rd8`,
  `gsu_wr8`, `gsu_plot` and `gsu_rpix` for its bus and its plotting; `gsu_code` naming the
  original code bytes each piece of C stands for, so their fetches are priced through the
  code cache as the GSU prices them; helpers for its loads, stores, ROM buffer, arithmetic
  and flags.
- [`spc.h`](include/nshvc/spc.h), the SPC700: the register file `spc`; `spc_rd`/`spc_wr`
  for every APU RAM and I/O access; `spc_cyc` for the original's cycles; `spc_call` and
  `spc_ret` for its calls.

[`hw.h`](include/nshvc/hw.h) names the hardware registers by their standard names.

### Time

Time is the console's master clock (21.477 MHz NTSC). Each S-CPU access moves it by what
that access takes on the bus (8 clocks for SlowROM and work RAM, 6 for most registers, 12
for the joypads' serial ports), and `cyc` moves it by the time the original spends between
accesses. As the clock passes each line's horizontal blank, that line is recorded for the
picture and HDMA runs; at line 225 the vertical blank starts (the NMI, the joypads'
automatic read, the frame's end). Interrupts are taken only where the S-CPU's C touches the
bus, never between the two bytes of one word, as an instruction boundary is the only place
the hardware takes them.

### Three processors, one thread

The Super FX's and the SPC700's C run on coroutines of their own
([`src/console/coro.c`](src/console/coro.c), a few instructions of assembly on arm64 and
x86-64, fibers on Windows), each with its own clock. Whenever the S-CPU could see what one
of them does (its registers, its RAM, the sound ports) and once a line, the S-CPU brings it
up to its own time, and it hands back when it gets there. One runs at a time, on a
deterministic schedule, with no threads: the same input gives the same run on every host.

- **Super FX** ([`superfx.c`](src/console/superfx.c), [`gsu_clock.c`](src/console/gsu_clock.c)).
  A program starts when the S-CPU writes R15; its C, found by its address (PBR:R15), runs
  until its STOP, then GO clears and the IRQ rises. Its clock follows the GSU-1's: the code
  cache and its fills, ROM and RAM buffer waits, multiplier times, pixel-cache flushes, and
  waits for the bus while the S-CPU holds it. PLOT and RPIX go through the two pixel caches
  as on the cartridge.
- **Sound unit** ([`apu.c`](src/console/apu.c)). At power on, the boot ROM's work is done
  instruction for instruction (not from the ROM's bytes), so the S-CPU's upload sees the
  same replies at the same pace. Code the S-CPU uploads and starts is the game's SPC700 C,
  chosen by which program's bytes the APU RAM holds at that address (two drivers may share
  addresses). Every 32 SPC700 cycles the DSP makes a sample, 32,040 a second.

### Dispatch

[`dispatch.c`](src/console/dispatch.c) connects addresses to C: the routine with C that
starts at an address a call, a jump, an interrupt vector or a Super FX R15 reaches; the
SPC700 routine whose code the APU RAM holds. A game that reaches an address with no C is a
fatal error (`nshvc_fatal`, exit status 70), never a silent fallback.

### Picture

[`src/ppu/ppu.c`](src/ppu/ppu.c) records each line's screen state, palette, tiles and
sprite table as the line's horizontal blank comes, and draws the reference picture from that
record: modes 0 to 7 (2, 4 and 8 bits a pixel, 8x8 and 16x16 tiles, mosaic,
offset-per-tile, direct colour, Mode 7 with its wrap modes and EXTBG, hires modes 5 and 6),
sprites with the 32-sprite and 34-tile line limits and priority rotation, windows on main
and sub screens, colour maths, brightness and forced blank.

The GPU composes the same picture from the same record, one pixel at a time
([`src/render/compose/core.glsl`](src/render/compose/core.glsl), one source for GLSL and
Metal, SPIR-V for Vulkan), step for step with `ppu.c`, so the two agree on every pixel.
The renderer backends (Metal, OpenGL, Vulkan, software, null; [`rr.h`](src/render/rr.h))
then scale it to the window with sharp bilinear filtering: whole pixels at any size,
blended only across the one target pixel each pixel edge falls in. The software backend is
the reference the GPU backends are checked against ([`src/render/check/`](src/render/check/)).

### Sound

The DSP's output (stereo, 16-bit, 32,040 frames a second) is resampled to the device's rate
by a 32-tap Kaiser-windowed sinc ([`resample.c`](src/dsp/resample.c)), its ratio trimmed by
up to 0.5% to keep 50 ms buffered, so the sound follows the game's pace without drifting or
clicking. [`tests/dsp/resample_check.c`](tests/dsp/resample_check.c) holds its error 70 dB
below the signal. A native mixer ([`mixer.c`](src/dsp/mixer.c): BRR decoding, Gaussian
interpolation, envelopes, noise, pitch modulation, echo), held sample for sample to a
reference DSP's output by [`tests/dsp/mix_replay.c`](tests/dsp/mix_replay.c), is the path for
drivers that talk to the mixer directly.

### The host

Each operating system supplies one small interface ([`src/host/host.h`](src/host/host.h)):
time, files and the save folder, a window, input, audio.

| Host | Provides |
|---|---|
| `host/posix` | time and files on macOS, Linux and iOS |
| `host/win32` | time and files on Windows |
| `host/macos` | Cocoa window, keyboard and game controllers, Core Audio |
| `host/windows` | Win32 window, keyboard, XInput controllers, WASAPI |
| `host/ios` | full-screen view, touch controls, keyboard and game controllers, RemoteIO |
| `host/null` | no window, input or audio, for tests and headless comparisons |

An app that brings its own host (a builder, a libretro core) links none of the window hosts
and supplies these functions itself.

### What a player gets

- A frame each 1/60.0988 s (NTSC), paced against the wall clock.
- Two controller ports: the keyboard on port 1, game controllers on ports 1 and 2, mapped
  to the SNES layout (B below, A right).
- The battery save in the host's save folder, written when it changes.
- A headless mode with no window and no pacing that runs a script of inputs from power on
  and writes screenshots and memory dumps, for tests and for comparison with the original.

## Using nativeshvc in a decompilation

This is the interface `nshvc.h` declares. As the Status section says, it is not
implemented yet; this is the contract a decompilation builds to.

### 1. Write the C against the machine model

Write each routine's C against `machine.h`, `gsu.h` and `spc.h`: every bus access through
`rd8`/`wr8` (`gsu_rd8`/`gsu_wr8`, `spc_rd`/`spc_wr`) in the original's order, and the
original's time between them through `cyc` (`gsu_code`, `spc_cyc`). This is the same C the
decompilation's verification harness checks against the original, unchanged. Each routine
that is entered from an address (a vector, a computed call, a Super FX program, an SPC700
routine) gets an `abi_` wrapper that takes its inputs from the register file and leaves its
outputs there.

### 2. Describe the cartridge

```c
#include "nshvc/nshvc.h"

extern const nshvc_routine game_routines[];        /* sorted by address */
extern const int game_routine_count;
extern const nshvc_spc_routine game_spc_routines[];   /* sorted by APU address */
extern const int game_spc_routine_count;
extern const uint8_t game_rom[];                   /* the ROM, or see rom_pages below */

static const nshvc_cart cart = {
    .title       = "Stunt Race FX",
    .save_name   = "stuntracefx",
    .map         = NSHVC_SUPERFX,
    .region      = NSHVC_NTSC,
    .rom         = game_rom,
    .rom_size    = 0x100000,
    .sram_size   = 0x10000,     /* the Super FX's RAM */
    .battery     = 1,
    .routines          = game_routines,
    .routine_count     = 0,     /* set from game_routine_count */
    .spc_routines      = game_spc_routines,
    .spc_routine_count = 0,     /* set from game_spc_routine_count */
};
```

- `map` and `region` say how the cartridge answers addresses (LoROM, HiROM, ExLoROM,
  ExHiROM, Super FX) and whether it is NTSC or PAL.
- `rom`/`rom_size` give the whole ROM image. A build that ships only the game's data (its
  code is the C) gives `rom_pages` instead, the 256-byte pages one data block holds whole,
  and `rom_read` for the rest; the reader is told who is reading (`NSHVC_ROM_CPU`, `_DMA`,
  `_GSU`, `_PEEK`).
- `sram_size` and `battery` give the cartridge's RAM and whether it is kept between runs;
  `save_from`/`save_size` narrow what is written.
- `routines` lists every S-CPU routine reached by address (the reset and interrupt vectors,
  every computed call's and jump's target) and every Super FX program, each with its `abi_`
  entry. `NSHVC_RT_GSU_CACHE_FIRST` marks a Super FX program whose first instruction is
  CACHE.
- `spc_routines` lists the SPC700 routines; `uses` tells which ROM bytes each stands for,
  so the right one runs when two sound programs put code at one address.
- `spc_run` and `gsu_run` optionally replace the lookup with the game's own entry.

The routine tables are what a decompilation's tags already generate; they are not written
by hand.

### 3. Switch it on

```c
int main(int argc, char **argv)
{
    return nshvc_main(&cart, argc, argv);
}
```

`nshvc_main` is the console as an application: it opens the window, renderer, sound and
controllers, loads the save, powers on, runs the reset vector's C and plays until the window
closes. The same program runs headless:

```bash
game --headless --frames 600 --script steps.txt --out shots --sound out.raw
```

The script's steps are `wait N`, `hold B+B N`, `press B`, `shot NAME`, `dump NAME` and
`repeat N ... end`; shots are written as `DIR/NAME.ppm`, dumps as `DIR/NAME.wram` and
`DIR/NAME.sram`, and `--sound` writes the DSP's raw output.

### 4. Or bring your own loop

An app with its own loop (a libretro core, an engine, a builder) uses the lower calls:

```c
nshvc_power(&cart);
for (;;) {
    nshvc_set_joypad(0, buttons);          /* NSHVC_B, NSHVC_A, NSHVC_START ... */
    nshvc_run_frame();                     /* runs to the next vertical blank */

    int w, h;
    const uint32_t *px = nshvc_picture(&w, &h);   /* 0xFFRRGGBB, 256 or 512 by 224 or 239 */
    int16_t lr[2 * 1024];
    int n = nshvc_audio_take(lr, 1024);    /* stereo at NSHVC_DSP_RATE (32,040 Hz) */
    /* show px, play n frames of lr */
}
```

The S-CPU's C runs on a coroutine of its own; `nshvc_run_frame` returns between two of its
accesses, at the vertical blank's start. For checks and saves an app can read the machine's
memory (`nshvc_wram`, `nshvc_sram`, `nshvc_aram`, `nshvc_vram`, `nshvc_cgram`, `nshvc_oam`),
peek at the bus with no side effects (`nshvc_peek`), and hook each frame and each S-CPU
write (`nshvc_set_frame_hook`, `nshvc_set_write_hook`).

### What the decompilation does not have to do

It does not write a renderer, a mixer, a resampler, a window, controller handling, a save
system, frame pacing, DMA, HDMA, interrupt delivery or a sound-unit boot. It does not ship
the ROM's code: only its C, and the ROM's data or the player's own copy.

## Licence

Copyright (C) 2026 Jeffrey Wilbur. nativeshvc is licensed under the GNU Affero General
Public License, version 3 ([LICENSE](LICENSE)). Anyone who distributes nativeshvc or a work
based on it, or lets users interact with a modified version over a network, must keep the
copyright and [NOTICE](NOTICE) and publish the complete source under the same licence.

nativeshvc contains no game code or data. The games that run on it, their code and their
assets belong to their rights holders, and a decompilation that links nativeshvc carries
its own terms for its own source.
