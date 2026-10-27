# nativeshvc's checks

Every claim nativeshvc makes about matching the console is held by one of these.
Each was shown able to fail (a planted error caught) before it was relied on.

| check | holds | run |
|---|---|---|
| `rom/run.py` | the console, pixel for pixel and read for read, against the reference (bsnes, accuracy settings, display gamma off): every background mode, tile size, offset-per-tile, Mode 7, sprites and their limits, windows, colour maths, brightness, mosaic, HDMA, the read side, overscan, hires, every cartridge mapping, the CPU's interrupt, DMA and HDMA timing (a DMA's own time at every phase, HDMA's, HDMA between a DMA's bytes, and both inside stretches the C states cycle by cycle), the multiplier's and divider's results as they work, the counters and frame lengths, interlace (its fields woven, hires and OBJ interlace), mid-line changes (each layer's tile fetch by mode) and the sound unit's replies as the S-CPU sees them (the boot ROM's handshake, to 2 clocks). 1,060 test cartridges, every one matching; `rom/KNOWN` lists any that differ | `python3 tests/rom/run.py [--only NAME,...]` (about 3 minutes) |
| `rom/ext_check.py` | the extensions: a larger screen's centre is exactly the console's picture, its extra columns the scene scrolled, VRAM pages the same as page 0 | `python3 tests/rom/ext_check.py` (about 1 minute) |
| `srfx/same.py` | a real decompilation, unchanged, on nativeshvc: Stunt Race FX's 21 plays against the original cartridge on the reference (`oracle --script`), every shot, work RAM and Super FX RAM dump and the pacing record, byte for byte; where a play differs, the decompilation's C and the cartridge part (the console is held by `rom/run.py`) | `sh tests/srfx/build.sh && python3 tests/srfx/same.py` (about 5 minutes) |
| `srfx/first.py` | where a play that differs first does: the first frame whose RAM differs from the cartridge's, the bytes, and the cartridge's instruction that wrote each (its routine): the decompilation's C or the console | `python3 tests/srfx/first.py [PLAY...]` (about 10 minutes) |
| `dsp/fuzz.py` | the sound unit on its own: a test cartridge whose SPC700 program (`dsp/fuzz_spc.c` its C, cycle for cycle) writes every DSP register at random on every clock of the frame, plays random sample data, runs the timers and the ports. The mixer given the reference's writes at the reference's clocks: every sample, every voice's state, every DSP register read and every APU RAM read; then the cartridge on nativeshvc: every SPC700 access (time, address, value), every sample, every S-CPU port read | `python3 tests/dsp/fuzz.py [SEED...]` (seeds 1-4, about 30 seconds) |
| `dsp/mix_replay` | any run's sound against the reference's: the mixer given the writes the reference's SPC700 made (`oracle --spclog L [--spcreads] --sound S --voices V`), every sample, voice state and DSP read (and APU RAM read, with `--spcreads`) | `build/null/tests/mix_replay L S V` |
| `dsp/resample_check` | the resampler against the exact signal: 70 dB or more below a tone up to 12 kHz | `build/null/tests/resample_check` |
| `unit/untimed.c` | untimed mode, the vertical blank wait, the untimed wait guard | `build/null/tests/untimed` |
| `unit/gsu_ext.c` | the Super FX's extended screen and RAM | `build/null/tests/gsu_ext` |
| `unit/overlay_check.c` | every renderer's overlays against the reference drawing | `RR_SIZE=1100x800 build/null-RENDER/tests/overlay_check` |
| `NSHVC_RRCHECK=1` | every renderer's presented frames (one to one and scaled, sharp bilinear) against the reference scaling | `NSHVC_RRCHECK=1 RR_SIZE=1000x750 build/null-RENDER/tests/testcart --rom ROM --headless --frames 4` |
| `NSHVC_AUDIT=1` | the C's time against the DMA unit: each `cyc` stretch a DMA or HDMA falls inside, by C function and offset (where `cycs` wants using) | any headless run of a game |
| `NSHVC_CYCLOG=FILE` | the C's time and accesses, a line each (`[NSHVC_CYCLOG_FRAMES=A-B]`; format in `src/console/bus.c`): each `cyc` and `cycs` by call site, the time the console states, waits, reads, writes and an interrupt's pushes, to pair against the original's cycles. Sites map to lines with `SRFX_CFLAGS=-g sh tests/srfx/build.sh` and `atos -o build/srfx/src_stuntracefx_g/srfx -l BASE -i SITE-1` | any headless run of a game |
| `NSHVC_SPCLOG=FILE` | the SPC700's accesses, to pair with the reference's (`oracle --spclog`): each write and each read of $F0-$FF, and the S-CPU's port writes, 8 bytes each (format in `tests/oracle/oracle.c`) | any headless run |
| `NSHVC_PPUCHECK=1` | each frame drawn from its record against the frame drawn a line at a time | any headless run |

## Setting up

```bash
cmake -S . -B build/null -DNSHVC_TESTS=ON -DNSHVC_EXAMPLES=ON && cmake --build build/null
```

`rom/run.py` needs the reference core and the assembler:

- `NSHVC_ORACLE_CORE`: a bsnes libretro core. The reference here is the
  Stunt Race FX project's pinned bsnes, built from its source with one
  change: the libretro target's display gamma set to 1.0, so the core's
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

None: every test matches the reference. A test that differs for a reason
measured and not yet closed goes in `rom/KNOWN` with that reason;
`rom/run.py` reports it as KNOWN, never as passing, and reports any that
start passing, so the list is kept true.
