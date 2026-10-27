# nativeshvc's checks

Every claim nativeshvc makes about matching the console is held by one of these.
Each was shown able to fail (a planted error caught) before it was relied on.

| check | holds | run |
|---|---|---|
| `rom/run.py` | the console, pixel for pixel and read for read, against the reference (bsnes, accuracy settings, display gamma off): every background mode, tile size, offset-per-tile, Mode 7, sprites and their limits, windows, colour maths, brightness, mosaic, HDMA, the read side, overscan, hires, every cartridge mapping, the CPU's interrupt, DMA and HDMA timing (a DMA's own time at every phase, HDMA's, HDMA between a DMA's bytes, and both inside stretches the C states cycle by cycle), the multiplier's and divider's results as they work, the counters and frame lengths, interlace (its fields woven, hires and OBJ interlace), mid-line changes (each layer's tile fetch by mode) the sound unit's replies as the S-CPU sees them (the boot ROM's handshake, to 2 clocks), the joypads' automatic read (when it ends, at every phase of the CPU's counter) and every controller (joypads, multitap, mice and their speed, Super Scope, Justifiers, an empty port: the automatic read, a strobe and while latched, with the script's input each frame, the same on both). 1,164 test cartridges: 1,060 match, and the 104 that measure interrupt latency differ by it alone (README.md, Time), each listed in `rom/KNOWN` | `python3 tests/rom/run.py [--only NAME,...]` (about 3 minutes) |
| `rom/ext_check.py` | the extensions: a larger screen's centre is exactly the console's picture, its extra columns the scene scrolled, VRAM pages the same as page 0 | `python3 tests/rom/ext_check.py` (about 1 minute) |
| `dsp/fuzz.py` | the sound unit on its own: a test cartridge whose SPC700 program (`dsp/fuzz_spc.c` its C, cycle for cycle) writes every DSP register at random on every clock of the frame, plays random sample data, runs the timers and the ports. The mixer given the reference's writes at the reference's clocks: every sample, every voice's state, every DSP register read and every APU RAM read; then the cartridge on nativeshvc: every SPC700 access (time, address, value), every sample, every S-CPU port read | `python3 tests/dsp/fuzz.py [SEED...]` (seeds 1-4, about 30 seconds) |
| `dsp/mix_replay` | any run's sound against the reference's: the mixer given the writes the reference's SPC700 made (`oracle --spclog L [--spcreads] --sound S --voices V`), every sample, voice state and DSP read (and APU RAM read, with `--spcreads`) | `build/null/tests/mix_replay L S V` |
| `dsp/resample_check` | the resampler against the exact signal: 70 dB or more below a tone up to 12 kHz | `build/null/tests/resample_check` |
| `unit/input.c` | the native controls onto the console's controllers (keyboard, game controllers and their buttons' names, the mouse, the multitap, the pointer's place on the picture), read back through the ports; the device each player is using | `build/null/tests/input` |
| `unit/states.py` | the script's save, load and calls, with `fork` and without (as on Windows): every dump the same both ways, byte for byte, and each the machine it should be | `python3 tests/unit/states.py` (seconds) |
| `unit/pointer_macos.m` | a light gun aimed with the real pointer, in a window: AppKit's own mouse events at known places on the picture, the counters the console latched read back by the cartridge | `build/macos/tests/pointer_macos` (a window, seconds) |
| `unit/pointer_ios.c` | a light gun aimed by touch on the iOS simulator: each touch's place against the counters latched | `sh tests/unit/pointer_ios.sh build`, install and touch it, then `sh tests/unit/pointer_ios.sh check pointer.txt` |
| `unit/untimed.c` | untimed mode, the vertical blank wait, the untimed wait guard | `build/null/tests/untimed` |
| `unit/chip.c` | the cartridge connector: a board its chip decodes (`NSHVC_CHIP`, the chip's `rom_at`), the chip's registers and RAM, its IRQ line, open bus, the ROM read for the chip with its own bits | `build/null/tests/chip` |
| `unit/overlay_check.c` | every renderer's overlays against the reference drawing | `RR_SIZE=1100x800 build/null-RENDER/tests/overlay_check` |
| `NSHVC_RRCHECK=1` | every renderer's presented frames (one to one and scaled, sharp bilinear) against the reference scaling | `NSHVC_RRCHECK=1 RR_SIZE=1000x750 build/null-RENDER/tests/testcart --rom ROM --headless --frames 4` |
| `NSHVC_AUDIT=1` | the C's time against the DMA unit: each `cyc` stretch a DMA or HDMA falls inside, by C function and offset (where `cycs` wants using) | any headless run of a game |
| `NSHVC_CYCLOG=FILE` | the C's time and accesses, a line each (`[NSHVC_CYCLOG_FRAMES=A-B]`; format in `src/console/bus.c`): each `cyc` and `cycs` by call site, the time the console states, waits, reads, writes and an interrupt's pushes, to pair against the original's cycles. | any headless run of a game |
| `NSHVC_SPCLOG=FILE` | the SPC700's accesses, to pair with the reference's (`oracle --spclog`): each write and each read of $F0-$FF, and the S-CPU's port writes, 8 bytes each (format in `tests/oracle/oracle.c`) | any headless run |
| `NSHVC_PPUCHECK=1` | each frame drawn from its record against the frame drawn a line at a time | any headless run |

## Setting up

```bash
cmake -S . -B build/null -DNSHVC_TESTS=ON -DNSHVC_EXAMPLES=ON && cmake --build build/null
```

`rom/run.py` needs the reference core and the assembler:

- `NSHVC_ORACLE_CORE`: a bsnes libretro core. The reference here is the
  Stunt Race FX project's pinned bsnes, built from its source with two
  changes to its libretro front end (none to the console it models): the
  light guns' inputs passed through (`target-libretro/program.cpp`'s
  `pollInputDevices` returns nothing for them as shipped), and the libretro
  target's display gamma set to 1.0, so the core's
  frames carry the PPU's colours as made (its default gamma of 1.5 cannot be
  undone at the dark end). Build it into `build/oracle-core` (about 5
  minutes), never into the other project's tree.
- `NSHVC_ASAR`: asar.

The core's own options are pinned by `oracle/oracle.c`: accuracy PPU and
DSP, no overclock, no blur, no randomised power-on state, no hacks.

## The test cartridges

`rom/snestest.py` builds each test as a real ROM: a small interpreter in
65816 assembly that runs a table of steps (register writes, DMA, waits for
a line or a vertical blank, reads logged to work RAM, WAI, NMI and IRQ
handlers that run tables of their own). The same ROM runs on the reference
and on nativeshvc, where `rom/testcart.c` is the same interpreter written
against the machine model, cycle for cycle as the assembly runs, as a cycle
exact decompilation's C is. `rom/cases.py` lists the tests; most fill VRAM,
CGRAM and OAM with random data so every path of a feature is reached, and
the probes (`probe_*`) use labelled data that shows exactly what the console
did at each pixel.

## The known residual (`rom/KNOWN`)

The 104 tests that measure interrupt latency (`irq_fixed`, `irq_entry`,
`frame_clock`): the handler starts 1 to 20 dots earlier than the
reference's, because the C marks no instruction ends (README.md, Time).
Every other test matches. A test that differs goes in `rom/KNOWN` with
the reason;
`rom/run.py` reports it as KNOWN, never as passing, and reports any that
start passing, so the list is kept true.
