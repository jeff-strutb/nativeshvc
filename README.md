# nativeshvc: a native Super Nintendo for decompiled games

**A decompiled game is the cartridge; nativeshvc is the console it plugs into.** A game's
decompiled C, linked against nativeshvc, builds a native program that powers on, runs the
reset vector's code and plays as it did on the console: the same picture, the same sound,
the same pace, with no emulated processor and no ROM image of the code.

**Maintainer:** Jeffrey Wilbur, Strut B, LLC\
**Contact:** [retro@strutb.com](mailto:retro@strutb.com)

nativeshvc was created by Jeffrey Wilbur. It began as the machine layer of the native
[Stunt Race FX](https://github.com/jeff-strutb/stuntracefx) port, and was lifted out of it so
that every Super Nintendo decompilation can share one console instead of building its own.

## Status

Version 0.1.0.

**What is not done:**

- **Stunt Race FX does not yet match its cartridge.** All 21 of its plays still differ from
  the original cartridge on the reference. Every difference traced so far is in the game's
  C (timing it states in lumps that a DMA or HDMA can fall inside, being converted to
  cycle-by-cycle time), not in the console.
- **Stunt Race FX's unit tests can't match yet.** Its 325 unit scripts all start from
  saved states past frame 70, where the plain play already differs from the cartridge.
- **The sound unit and DSP** now have a test of their own against the reference's chip
  (every sample, every voice and every access), but no result from it is recorded yet. The
  earlier difference of 1 to 2 levels on one channel, from sample 126,206 of Stunt Race
  FX's attract mode, is not confirmed closed.
- **Parts of the script runner are untested:** Super FX program calls (`fx`) run but are
  not checked against the reference; SPC700 routine calls (`spccall`) need the game's
  sound driver C to mark where a call may start, and fail with an error until it does; and
  several call entry options (`frame=`, `php`, `phb`, `rti`, `vblank`, `press=`) are
  untested.
- **The renderer checks** (presented frames and overlays) have not been rerun since the
  latest console changes.
- **The Windows build** is cross-built with MinGW but has never been run, and saving and
  loading states, and calls that put the machine back, need `fork`, so they don't work on
  Windows.

**What is done and checked.** Every check below is against bsnes as the reference, with its
options pinned for accuracy and display gamma off. The full list of checks, and how to run
each one, is in [tests/README.md](tests/README.md).

- **Test cartridges:** all 1,060 console test cartridges match the reference, and none is
  listed as a known difference.
- **Cartridges:** LoROM, HiROM, ExLoROM, ExHiROM and Super FX mappings, including
  mirroring for ROM sizes that are not a power of two, and cartridge RAM; NTSC and PAL
  frame lengths.
- **Picture:** modes 0 to 7, 16x16 tiles, offset-per-tile (modes 2, 4, 6), direct colour,
  Mode 7 and EXTBG, hires and pseudo-hires (512 wide), overscan (239 lines), interlace (its
  fields woven, hires and OBJ interlace), mosaic, sprites in every size (with the vertical
  flip of tall sprites), their line limits and priority rotation, windows, colour maths,
  brightness, HDMA, the read side and open bus.
- **Timing:** mid-line register writes (Mode 7's matrix included) and each layer's tile
  fetch by mode, CGRAM writes during display landing on the entry being looked up, VRAM
  writes blocked during display, H and V IRQs with long dots, WAI, multiply and divide
  results read mid-operation, DMA and HDMA timing at every 2-clock phase and HDMA between a
  DMA's bytes, and the sound unit's handshake as the S-CPU sees it.
- **A real decompilation runs on it:** Stunt Race FX (S-CPU, Super FX and SPC700) runs
  unchanged on nativeshvc. A check compares its 21 plays with the original cartridge on
  the reference, byte for byte (every screenshot, work RAM and Super FX RAM dump and pacing
  record), and another finds the frame and the instruction where a play first differs.
  They do not all match yet (see above).
- **Unit tests from a game state:** a script can run one routine's C once, from any point
  the game reached, with its registers and inputs, and return the machine to where it was;
  it can also save and load states. Two hand-written Stunt Race FX scripts (six calls with
  staged bytes and a kept call; saving and loading across several states) match the
  reference exactly, frame by frame.
- **Renderers:** software, Metal, OpenGL and Vulkan each present the frames and draw
  overlays; they matched the CPU reference when last checked, before the latest console
  changes.
- **Hosts:** headless, macOS (windowed run confirmed) and iOS (run on the iPhone simulator:
  touch, buttons, the frame hook).

## Project Purpose

A decompilation that is byte exact and cycle exact recovers the game's code, but code alone
does not play. On the console it sat in a machine: a bus that answers its addresses, a
picture unit drawing what it wrote, a sound chip, DMA, interrupts, a controller, a battery,
a television's frame rate. Each native port has had to rebuild all of that around its game.

nativeshvc builds it once. Its purpose is the equivalent of putting a cartridge in the
console and switching it on: a decompilation describes its cartridge, hands over its C, and
gets a native program that plays on macOS, Windows, Linux and iOS, presented on the GPU at
any window size, with sound, controllers and saves, at the console's own pace.

It holds to the standard of the decompilations it serves. It runs no emulated CPU, does no
high-level emulation of the game, and has no per-game fixes. The game's own C makes every
access the original makes, in the original's order, and states the time the original spends
between them; nativeshvc answers each access as the hardware does, at the hardware's time.
A game whose C is cycle exact therefore runs on nativeshvc frame for frame as it ran on the
console.

Beyond the console, nativeshvc is also an unconstrained native Super Nintendo: the
console's numbers are its baseline, and a port may opt out of them (see
[Extensions](#extensions)).

## How it works

The full reference for the interface, the cartridge description, the hooks and the
extensions is [docs/nativeshvc.md](docs/nativeshvc.md).

### The split: processors in the game, everything else in the console

The processors are the game's C. Everything that is not a processor is nativeshvc:

| The game supplies | nativeshvc supplies |
|---|---|
| The S-CPU's (65816) code, as C | The S-CPU's bus and clock, work RAM, the CPU's registers (interrupts, timers, multiplier and divider, joypads) |
| An expansion chip's programs, such as the Super FX's, as C | The cartridge connector a chip plugs into, and the Super FX as a library a cartridge links |
| The SPC700's sound driver, as C | The sound unit: 64 KB of APU RAM, its ports and timers, the boot ROM protocol, the S-DSP |
| The ROM's data (or the whole ROM) | The cartridge's mapping, ROM and battery RAM |
| | DMA and HDMA, the PPU, the window, the GPU presentation, sound output, controllers, the save, the frame's pace |

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

### The cartridge connector

nativeshvc is the console, and implements no expansion chip itself. A cartridge's own
hardware (the Super FX, the SA-1, a DSP) is the cartridge's, as on the console, and plugs in
through the connector in [`cart.h`](include/nshvc/cart.h): the chip powers on with the
console, answers the S-CPU's accesses to the addresses it decodes, is brought up to the
console's clock before those accesses and once a line, drives the IRQ line, and may own the
cartridge's RAM. A cartridge with a chip names it in its description.

The Super FX is the first chip, built as its own library
([`src/chips/superfx/`](src/chips/superfx/), header [`superfx.h`](include/nshvc/superfx.h))
that a Super FX cartridge links.

### Time

Time is the console's master clock (21.477 MHz NTSC). Each S-CPU access moves it by what
that access takes on the bus, and `cyc` moves it by the time the original spends between
accesses. Where a DMA or HDMA can fall inside that time, the C states it cycle by cycle
instead (`cycs`, each hex digit one cycle's clocks), so the transfer lands on the right
cycle edge. As the clock passes each line's horizontal blank, that line is drawn and HDMA
runs; at the vertical blank's start come the NMI, the joypads' automatic read and the
frame's end. Interrupts are taken only where the S-CPU's C touches the bus, never between
the two bytes of one word, as an instruction boundary is the only place the hardware takes
them.

### Three processors, one thread

The Super FX's and the SPC700's C run on coroutines of their own
([`src/console/coro.c`](src/console/coro.c), a few instructions of assembly on arm64 and
x86-64, fibers on Windows), each with its own clock. Whenever the S-CPU could see what one
of them does (its registers, its RAM, the sound ports) and once a line, the S-CPU brings it
up to its own time, and it hands back when it gets there. One runs at a time, on a
deterministic schedule, with no threads: the same input gives the same run on every host.

- **Super FX** ([`superfx.c`](src/chips/superfx/superfx.c), [`gsu_clock.c`](src/chips/superfx/gsu_clock.c)).
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

The PPU ([`src/ppu/ppu.c`](src/ppu/ppu.c)) makes the picture in C, a line at a time as the
clock passes each line: every background mode, sprites, windows, colour maths, brightness,
the read side and open bus, with register writes landing mid-line at their own times.

The GPU only presents it. The renderer backends (Metal, OpenGL, Vulkan and software;
[`rr.h`](src/render/rr.h)) scale the frame to the window with sharp bilinear filtering:
whole pixels at any size, blended only across the one target pixel each pixel edge falls in.
They also draw a port's overlays. Each backend's output is checked against the CPU
reference.

### Sound

The S-DSP ([`mixer.c`](src/dsp/mixer.c): BRR decoding, Gaussian interpolation, envelopes,
noise, pitch modulation, echo) makes stereo, 16-bit samples at 32,040 a second as the
SPC700's clock runs. They are resampled to the device's rate by a 32-tap Kaiser-windowed
sinc ([`resample.c`](src/dsp/resample.c)), its ratio trimmed by a fraction of a percent to
keep a steady buffer, so the sound follows the game's pace without drifting or clicking.

### The host

Each operating system supplies one small interface ([`src/host/host.h`](src/host/host.h)):
time, files and the save folder, a window, input, audio.

| Host | Provides |
|---|---|
| `null` | no window, input or audio, for tests and headless comparisons |
| `macos` | Cocoa window, keyboard and game controllers, Core Audio |
| `windows` | Win32 window, keyboard, XInput controllers, WASAPI |
| `ios` | full-screen view, touch controls, keyboard and game controllers, RemoteIO |
| `none` | nothing: the app (a builder, a libretro core) supplies `host.h` itself |

### What a player gets

- A frame each 1/60.0988 s (NTSC), paced against the wall clock.
- Two controller ports: the keyboard on port 1, game controllers on ports 1 and 2, mapped
  to the SNES layout (B below, A right); touch controls on iOS.
- The battery save in the host's save folder, written when it changes.
- A headless mode with no window and no pacing that runs a script of inputs from power on
  and writes screenshots and memory dumps, for tests and for comparison with the original.

### Extensions

The console's numbers are baselines a port may raise, each off until the port asks for it:
any screen size up to 1024x480, sprite X and Y extension bits, sprite line limits off, VRAM
up to 256 KB in pages, a Super FX screen up to 1024x512 and its RAM up to 896 KB, an
untimed mode with a vertical blank wait, and held interrupts. The front end adds key, touch
and frame hooks, overlays, buttons the game holds itself, the safe area and the window's
size. They are described in [docs/nativeshvc.md](docs/nativeshvc.md#the-extensions).

## Using nativeshvc in a decompilation

### 1. Build it

CMake, C11, no dependencies for the core:

```bash
cmake -S . -B build/macos -DNSHVC_HOST=macos -DNSHVC_RENDER=metal && cmake --build build/macos
```

`NSHVC_HOST` picks the host (`null`, the default, `macos`, `windows`, `ios` or `none`) and
`NSHVC_RENDER` the renderer (`soft`, `metal`, `gl` or `vulkan`). `NSHVC_TESTS` and
`NSHVC_EXAMPLES` build the tests and the examples. For iOS, add
`-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64
-DNSHVC_HOST=ios`.

### 2. Write the C against the machine model

Write each routine's C against `machine.h`, `gsu.h` and `spc.h`: every bus access through
`rd8`/`wr8` (`gsu_rd8`/`gsu_wr8`, `spc_rd`/`spc_wr`) in the original's order, and the
original's time between them through `cyc` (`gsu_code`, `spc_cyc`). This is the same C the
decompilation's verification harness checks against the original, unchanged. Each routine
that is entered from an address (a vector, a computed call, a Super FX program, an SPC700
routine) gets an `abi_` wrapper that takes its inputs from the register file and leaves its
outputs there.

### 3. Describe the cartridge and switch it on

A cartridge is an `nshvc_cart`: its title and save name, its mapping (`NSHVC_LOROM`,
`NSHVC_HIROM`, `NSHVC_EXLOROM`, `NSHVC_EXHIROM`, `NSHVC_SUPERFX`) and region, its ROM (a
whole image, or only the data pages with a reader for the rest, so no copy of the code
ships), its RAM and battery, its expansion chip if it has one, and the tables of routines
with C by address, which a decompilation's tags already generate. A Super FX cartridge uses
the `NSHVC_SUPERFX` mapping, sets its chip to `nshvc_superfx` and links the
`nshvc_superfx` library. The game's main hands the cartridge to the console:

```c
NSHVC_MAIN(argc, argv)
{
    return nshvc_main(&cart, argc, argv);
}
```

`NSHVC_MAIN` is `main` everywhere but iOS, where the host keeps the process's own main and
runs the game's on a thread. `nshvc_main` opens the window, renderer, sound and controllers,
loads the save, powers on, runs the reset vector's C and plays until the window closes.
[examples/hello/hello.c](examples/hello/hello.c) is a whole cartridge in one file, and
[docs/nativeshvc.md](docs/nativeshvc.md#plugging-a-game-in) describes every field.

The same program runs headless, for tests and comparisons:

```bash
game --headless --frames 600 --script steps.txt --out shots --sound out.raw
```

A script plays the controllers and records what happens: `wait N`, `hold B+B N`,
`press B`, `shot NAME` (the frame, as `DIR/NAME.ppm`), `dump NAME` (work RAM, the
cartridge's RAM, the PPU's memories and a chip's RAM) and `repeat N ... end`; `--sound`
writes the DSP's raw output.

It also takes the steps of a decompilation's own test harness, so the same tests run on the
reference and on the native build:

- `poke` and `set` put bytes into work RAM or the chip's RAM, now or when the next call
  starts.
- `call` runs one routine once, from a stub in work RAM that the console runs cycle by
  cycle, with the registers and frames it names; afterwards the machine is as it was, unless
  `keep` says the game goes on from there. `fx` does the same for a Super FX program.
- `spcset` and `spccall` do the same for an SPC700 routine, started where the driver's C
  marks a safe point (`spc_at`).
- `save NAME` and `load PATH` keep and return to the machine's state. The running C is part
  of the machine and cannot be copied, so the run forks instead: one process holds the
  state while another goes on. A state that no process holds is rebuilt by replaying, from
  power on, the steps that saved it (the script's own, or a play in `NSHVC_STATES`).

Every step is listed in [`src/front/script.h`](src/front/script.h).

### 4. Or bring your own loop

An app with its own loop (a libretro core, an engine, a builder) calls `nshvc_power` once,
then `nshvc_run_frame` each frame, and takes the picture (`nshvc_picture`) and the sound
(`nshvc_audio_take`) itself, setting the buttons with `nshvc_set_joypad`. It can read the
machine's memory, peek at the bus with no side effects, and hook each frame and each S-CPU
write. See [docs/nativeshvc.md](docs/nativeshvc.md#running-it-yourself).

### 5. Check its timing

Two options show where a port's stated time and the console disagree, on any headless run:
`NSHVC_AUDIT=1` names each `cyc` stretch a DMA or HDMA falls inside (where `cycs` is
needed), and `NSHVC_CYCLOG=FILE` writes the C's time and accesses a line each, to pair
against the original's cycles. The C can drop markers into that record with `nshvc_mark`,
which takes no time, to line it up with the code that made it. Stunt Race FX's own check, which finds where a play first
differs from the cartridge, is described in [tests/README.md](tests/README.md).

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
