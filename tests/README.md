# nativeshvc's checks

Every claim nativeshvc makes about matching the console is held by one of these.
Each was shown able to fail (a planted error caught) before it was relied on.

| check | holds | run |
|---|---|---|
| `rom/run.py` | the console, pixel for pixel and read for read, against the reference (bsnes, accuracy settings, display gamma off): every background mode, tile size, offset-per-tile, Mode 7, sprites and their limits, windows, colour maths, brightness, mosaic, HDMA, the read side, overscan, hires, every cartridge mapping, IRQ timing and mid-line changes. 308 test cartridges; 28 more listed in `rom/KNOWN` | `python3 tests/rom/run.py [--only NAME,...]` (about 2 minutes) |
| `rom/ext_check.py` | the extensions: a larger screen's centre is exactly the console's picture, its extra columns the scene scrolled, VRAM pages the same as page 0 | `python3 tests/rom/ext_check.py` (about 1 minute) |
| `srfx/same.py` | a real decompilation, unchanged, on nativeshvc: Stunt Race FX's 21 plays against its own port's reference build, every shot, work RAM and Super FX RAM dump and the pacing record, byte for byte | `sh tests/srfx/build.sh && python3 tests/srfx/same.py` (about 1 minute) |
| `unit/untimed.c` | untimed mode, the vertical blank wait, the untimed wait guard | `build/null/tests/untimed` |
| `unit/gsu_ext.c` | the Super FX's extended screen and RAM | `build/null/tests/gsu_ext` |
| `unit/overlay_check.c` | every renderer's overlays against the reference drawing | `RR_SIZE=1100x800 build/null-RENDER/tests/overlay_check` |
| `NSHVC_RRCHECK=1` | every renderer's presented frames (one to one and scaled, sharp bilinear) against the reference scaling | `NSHVC_RRCHECK=1 RR_SIZE=1000x750 build/null-RENDER/tests/testcart --rom ROM --headless --frames 4` |
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

An IRQ that wakes the CPU from WAI starts its handler on a grid of 6 master
clocks whose phase differs from the reference's by 2 to 4 clocks. Measured
three ways, no single wake cost fits all three:

- the counters the handler latches (`irq_latch_*`): 36 of 38 dots exact
  with the wake at 16 clocks, the setting kept, as it is the CPU-visible one;
- a brightness split the handler makes mid-line (`probe_scroll_00_*`): a
  3-step cycle of 0 or 1 pixel off;
- where a mid-line colour write lands (`probe_cgcol_*`): 1 to 2 entries off.

So a mid-line effect timed from a WAI-woken IRQ lands up to two pixels from
where the reference puts it. Nothing else is known to differ. `rom/run.py`
reports these as KNOWN, never as passing, and reports any that start
passing, so the list is kept true.
