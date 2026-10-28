# nativeshvc's checks

Every claim nativeshvc makes about matching the console is held by one of these.
Each was shown able to fail (a planted error caught) before it was relied on.

| check | holds | run |
|---|---|---|
| `rom/run.py` | the console's hardware, pixel for pixel and read for read, against the reference (bsnes, accuracy settings, display gamma off), to the clock: the reference runs each test cartridge and records every read, write and code fetch its S-CPU makes and every byte its DMA moves, with its clock (`oracle --cpulog`); `rom/replay.c` makes the same accesses on nativeshvc, each starting its bus cycle so that it is made at the reference's clock through the console's public calls (its own chip's IRQ line stopping a wait at each), and every read and every picture must match, and each DMA's length (its end 0 to 6 clocks before the reference's last byte, by how the two starts fall on the unit's 8-clock steps). An access may come up to 36 clocks late after the DMA unit's work, the reference CPU's cycle alignment, which the replay cannot reproduce; later is a failure. Every background mode, tile size, offset-per-tile, Mode 7, sprites and their limits, windows, colour maths, brightness, mosaic, HDMA, the read side, overscan, hires, every cartridge mapping, the IRQ's and NMI's flags and latches at every dot, DMA and HDMA at every phase and HDMA between a DMA's bytes, the multiplier and divider, the counters and frame lengths, interlace, mid-line changes (each layer's tile fetch by mode), the sound unit's ports, the joypads' automatic read at every phase of the CPU's counter and every controller. 1,164 test cartridges: 1,108 match; 42 read the multiplier mid-operation (done when read: the contract), and 14 differ by a latch or a pixel after an access 2 clocks late (a refresh's start, not yet explained), each listed in `rom/KNOWN` | `python3 tests/rom/run.py [--only NAME,...]` (about 3 minutes) |
| `rom/ext_check.py` | the extensions: a larger screen's centre is exactly the console's picture, its extra columns the scene scrolled, VRAM pages the same as page 0 | `python3 tests/rom/ext_check.py` (about 1 minute) |
| `dsp/fuzz.py` | the DSP on its own: a test cartridge whose SPC700 program writes every DSP register at random on every clock of the frame, plays random sample data, runs the timers and the ports, run on the reference; the mixer given the reference's writes at the reference's clocks: every sample, every voice's state, every DSP register read and every APU RAM read | `python3 tests/dsp/fuzz.py [SEED...]` (seeds 1-4, about 2 minutes) |
| `dsp/keys.py` | the notes a sound driver plays, its work taking no time: a driver keying voices on and off on timer 0's beat (`dsp/keys_spc.c` its C), on the reference and on nativeshvc; both runs' writes played through the mixer (`dsp/key_events`), every voice's key events the same, in order, each key-on with the same sample, each within 32 samples of the reference's (they come within 14) | `python3 tests/dsp/keys.py` (about 30 seconds) |
| `dsp/mix_replay` | any run's sound against the reference's: the mixer given the writes the reference's SPC700 made (`oracle --spclog L [--spcreads] --sound S --voices V`), every sample, voice state and DSP read (and APU RAM read, with `--spcreads`) | `build/null/tests/mix_replay L S V` |
| `dsp/resample_check` | the resampler against the exact signal: 70 dB or more below a tone up to 12 kHz | `build/null/tests/resample_check` |
| `tools/profile_check.py` | `tools/nshvc-profile`'s fit (docs/cost-profile.md): frames and a chip's jobs made from known weights, the S-CPU's eight and the SPC700's four weights and each program's fixed part and weights given back exactly, the S-CPU's fit least squares on each frame's error in clocks, each SPC700 kind's weight its own mean gap though two kinds come together, the chip's tie-break (a count the same in every job: its work in fixed) and columns a billion apart in scale, a kind never made 0, a program with too few jobs its multiplier, the profile file naming the chip's ops, the C table building against `nshvc.h` and carrying the weights | `python3 tests/tools/profile_check.py` |
| `unit/input.c` | the native controls onto the console's controllers (keyboard, game controllers and their buttons' names, the mouse, the multitap, the pointer's place on the picture), read back through the ports; the device each player is using | `build/null/tests/input` |
| `unit/states.py` | the script's save and load, with `fork` and without (as on Windows): every dump the same both ways, byte for byte, and each the machine it should be | `python3 tests/unit/states.py` (seconds) |
| `unit/pointer_macos.m` | a light gun aimed with the real pointer, in a window: AppKit's own mouse events at known places on the picture, the counters the console latched read back by the cartridge | `build/macos/tests/pointer_macos` (a window, seconds) |
| `unit/pointer_ios.c` | a light gun aimed by touch on the iOS simulator: each touch's place against the counters latched | `sh tests/unit/pointer_ios.sh build`, install and touch it, then `sh tests/unit/pointer_ios.sh check pointer.txt` |
| `unit/vram_direct.c` | VRAM pages 1-3 outside the bus (`nshvc_ext_vram_copy`, `nshvc_ext_vram_direct`): a page copied whole; while lines are drawn the port's writes to page 0 lost, to a direct page landing; a DMA of 2,000 bytes to it taking no time, its words in order, and with a CGRAM channel only that channel's time; the picture page 0's while page 1 changes unshown, page 1's once BG1 is switched to it, and a write to the shown page from its line on, with no frame out of copies; page 0 as a target fatal (`--copy0`, `--direct0`, exit 70) | `build/null/tests/vram_direct [--copy0 \| --direct0]` |
| `unit/waits.c` | the S-CPU's time: each access its bus's time (no profile), a loop polling with no wait ending as the clock moves; with a cost profile (`--allowance`) each kind's weight exactly, over 10,000 accesses, then with `nshvc_set_cpu_costs(0)` no time at all (bytes, words, longs and read-modify-writes) while a wait and a DMA still take theirs, and with `(1)` the bus's time and weights again; with the costs off, a loop polling with no wait must stop the run (`--offpoll`, exit 70); a wait ends at the first read that sees its condition (the vertical and horizontal blank, the joypads' read) and returns the clocks waited; the NMI and IRQ taken in a wait at their clocks; a wait in a handler, the IRQ held by the I flag and taken as the handler returns; WAI, masked and not; a DMA's length; the multiplier and divider done when read; an interrupt taken in code that left through its caller's frame (`cpu.unwound` kept, no fatal); a wait, or a polling loop, with interrupts held, fatal | `build/null/tests/waits` (`--allowance`; `--held`, `--heldpoll`: exit 70) |
| `unit/ipl.c` | an upload sent as games send it, a word to $2140 (the index a cycle before the byte), 4,096 bytes, taken by the sound unit's boot ROM and (`--driver`) by a driver's own C with the boot ROM's protocol; with a cost profile (`--profile`: the S-CPU's and the SPC700's weights; `--cpu-profile`: the S-CPU's alone), and (`--sweep`) with the S-CPU's work before each word varied so some straddle the refresh: each word's bytes 6 clocks apart (46 across the refresh), and every byte where it was sent; with the S-CPU's weights alone, only the split words' bytes land wrong. The weights are a real game's, as the tool measures them. It shows each fix: a weight between a word's bytes lost 2,106 of 4,096; a driver with no weights of its own, with words straddling the refresh, 13 | `build/null/tests/ipl [--driver] [--profile \| --cpu-profile] [--sweep]` |
| `unit/chip.c` | the cartridge connector: a board its chip decodes (`NSHVC_CHIP`, the chip's `rom_at`), the chip's registers and RAM, its IRQ line, open bus, the ROM read for the chip with its own bits | `build/null/tests/chip` |
| `unit/chip_irq.c` | a chip's IRQ at its own clock (`nshvc_cart_irq_at`): a chip on its own coroutine stops at a clock and raises it; the S-CPU takes it at that clock in a wait on its status, or at the end of the status read it rises in, the handler's acknowledge and RTI before the wait's read that sees the chip stopped; masked, the wait ends with the first read after the chip's clock; in WAI; at a refresh's end when it rises in one | `build/null/tests/chip_irq` |
| `unit/overlay_check.c` | every renderer's overlays against the reference drawing | `RR_SIZE=1100x800 build/null-RENDER/tests/overlay_check` |
| `unit/present_check.c` | Mode 7 at the window's resolution (`src/render/rr_mode7.h`), on scenes of rotation and scale, the wrap modes, flips, a matrix by line, EXTBG, sprites, colour maths, windows, brightness, mosaic, pseudo-hires, direct colour, a larger screen and a minified plane, at one to one, scales up, down, of another shape and 6x: against the anti-aliasing's definition written again in the check, exactly; a frame without Mode 7, and a plane of one colour under sprites, windows and colour maths, exactly the picture scaled (only the plane is anti-aliased); against the plane's ground truth (16 x 16 point samples a pixel), the mean error at most a quarter of one point sample's, and half where the plane is minified. With `--headless --frames N` and `NSHVC_RRCHECK=1`, the scenes through a renderer | `build/null/tests/present_check` (about 5 seconds); `NSHVC_RRCHECK=1 RR_SIZE=1067x800 build/null-RENDER/tests/present_check --headless --frames 60` |
| `NSHVC_RRCHECK=1` | every renderer's presented frames against the reference: one to one and scaled with sharp bilinear, and a frame with Mode 7 at the target's resolution, anti-aliased (rr_mode7.h rr_ref_mode7), within 1 a channel | `NSHVC_RRCHECK=1 RR_SIZE=1000x750 build/null-RENDER/tests/testcart --rom ROM --headless --frames 4` |
| `NSHVC_SPCLOG=FILE` | the SPC700's accesses, to pair with the reference's (`oracle --spclog`): each write and each read of $F0-$FF, and the S-CPU's port writes, 8 bytes each (format in `tests/oracle/oracle.c`) | any headless run |
| `NSHVC_PPUCHECK=1` | each frame drawn from its record against the frame drawn a line at a time | any headless run |

## Setting up

```bash
cmake -S . -B build/null -DNSHVC_TESTS=ON -DNSHVC_EXAMPLES=ON && cmake --build build/null
```

`rom/run.py` needs the reference core and the assembler:

- `NSHVC_ORACLE_CORE`: a bsnes libretro core. The reference here is
  bsnes v115, built from its source with two
  changes to its libretro front end (none to the console it models): the
  light guns' inputs passed through (`target-libretro/program.cpp`'s
  `pollInputDevices` returns nothing for them as shipped), and the libretro
  target's display gamma set to 1.0, so the core's
  frames carry the PPU's colours as made (its default gamma of 1.5 cannot be
  undone at the dark end). Build it into `build/oracle-core` (about 5
  minutes).
- `NSHVC_ASAR`: asar.

The core's own options are pinned by `oracle/oracle.c`: accuracy PPU and
DSP, no overclock, no blur, no randomised power-on state, no hacks.

## The test cartridges

`rom/snestest.py` builds each test as a real ROM: a small interpreter in
65816 assembly that runs a table of steps (register writes, DMA, waits for
a line or a vertical blank, reads logged to work RAM, WAI, NMI and IRQ
handlers that run tables of their own). `rom/cases.py` lists the tests;
most fill VRAM, CGRAM and OAM with random data so every path of a feature
is reached, and the probes (`probe_*`) use labelled data that shows exactly
what the console did at each pixel. The reference runs the ROM; on
nativeshvc, `rom/replay.c` makes the reference's own S-CPU accesses at the
reference's clocks, so the reference's CPU timing places every write and
read and the console's hardware is what is held. `rom/testcart.c` is the
same interpreter as C written against the machine model, its
polls waits: what `rom/ext_check.py` and the sound check run.

## The known differences (`rom/KNOWN`)

The 56 tests that differ. 42 read the multiplier or divider while it works,
which the contract makes done when read (README.md, Time). 14 differ by one
read of the latched H counter or one pixel, after an access the replay
started 2 clocks late: the work RAM's refresh before it ended 2 clocks
later than the reference's, though the reference's own formula gives
nativeshvc's clock for it; not yet explained. A test that differs goes in
`rom/KNOWN` with the reason; `rom/run.py` reports it as KNOWN, never as
passing, and reports any that start passing, so the list is kept true.
