# The PSP, shelved

The PSP is no longer a targeted platform. This branch, `psp-shelved`, holds its host, its GE
renderer, its checks and what was learned, as they stood when it was removed from `main`. It is
`main` at `1ecf196` merged with `contract-2.22` at `a056a76`, so it has the newest PSP code,
contract 2.22 included.

## Why it was shelved, and how sure anyone was

It was never settled by measurement. Do not read this as proof that the architecture was the
blocker.

**Suspected, by judgement, never tested directly:**

- The GE has no compute and no shaders, so "all rendering on the GPU" meant workarounds there, not
  a design that suits it.
- Exact behaviour through the console layer, one renderer shape, and full speed at 333 MHz may not
  all be achievable together on this machine.

**Measured, in PPSSPP only (below), with the limits that carries:**

- Stunt Race FX's README gave, by PPSSPP's clock against 16.7 ms: the title about 16 ms a frame, an
  Original race about 30 ms, an Enhanced race several seconds. These may have been stale when the
  decision was made.
- Most test cartridges ran each frame within 1/60 s; some of the heaviest (many windows, HDMA
  scrolling every line) did not.

**Never measured:**

- No frame was ever split into game, Super FX, console and rendering, so nobody knows which of
  them was the problem.
- Nothing ever ran on a real PSP.

## What the GE could not do, and what was done instead

Everything here was checked against the reference in PPSSPP with its software GE, not on hardware.

| The limit | What was done |
|---|---|
| No shaders, no compute | Each picture made from tiles and sprites as textured draws: depth for layer and priority (`cp_zbg`, `cp_zobj`), stencil for colour maths and windows, channel lookups through palettes for brightness and 5 bits to 8, fixed blend factors of 255 to pass a value through exactly |
| Bilinear weights in sixteenths, truncated | Sharp bilinear in two stages (`scale_ge`): across by lookups, down by a fixed blend. The blend was measured as `((2s + 1)(2A + 1) >> 10) + ((2d + 1)(2B + 1) >> 10)` over 256 alphas and three channels; with A = round(256 t), within 1 of the reference for every pair at every weight used (456,964 checked). This is why the contract's blend is in whole 256ths (`rr_mix_w`, which stays on `main`) |
| Overlays filtered in sixteenths, vertices in sixteenths of a pixel | A looser bound in `overlay_check` |
| No arithmetic for a box blur | The pause's backdrop made by repeated halving: within 35% of the blur's spread, 3% of its light, 4 a channel |
| Textures at most 512 x 512 | `rr_canvas_max`: a canvas past it fatal, `nshvc_ext_canvas_scale` capped to it, a whole k only (a 320-wide screen gets 1). Stripes of rows for the picture |
| 2 MB of VRAM, a 480 x 272 display | Three screen buffers, 512 pixels a row (557,056 bytes each); `rr_screen_max`: a picture of at most 122,880 pixels, 512 x 240, or 256 x 480 interlaced, so a hires or interlaced frame is 256 across and 512 x 448 is fatal. A hires picture held at 16 bits a pixel, which is exact as every channel is 5 bits |
| No 8-bit render target | A game's GE-drawn characters as 32-bit words with the value in the low byte, or 4444 for layers of 4 bits or fewer, read through a lookup's shift |
| Reading CPU texels every frame was expensive | Screen canvas texels read by the GE as the game gave them (`rr_compose_canvas` 0), bytes through an identity palette, a word's index in its low byte; `nshvc_ext_canvas_screen_ge` for VRAM the game drew, with no copy |
| Mode 7 per pixel lookups | Shown at the console's resolution and scaled as any picture (`rr_mode7_window`, `rr_mode7_plain`), a line drawn as spans over one plane texel each, a sprite of that texel |
| Mosaic | Each block a sprite of its one texel, magnified over the block |
| The GE's picture of a canvas exists only at the target's resolution | `nshvc_picture` fatal for a frame showing a canvas |
| 24 MB of memory on a PSP-1000; about 30 MB needed by the console's machine | PSP-2000 or later (`MEMSIZE 1`), stacks of 1 MB and 256 KB, checked by a canary once a frame |
| 32-bit MIPS, no 128-bit integer, no double FPU | `coro_mips.S`; `wide.h`; the resampler's position in fixed point. The last two stay on `main` |
| EABI's callee keeps `$f20`-`$f31`, newlib's `setjmp` only the even ones | GCC's builtin pair in `jump.h` |
| One program at a time, no new process | A script's `load` is fatal |

## The PPSSPP figures

**All of these are instruction-count estimates, not hardware times.** PPSSPP does not model
Allegrex timing: its clock counts an instruction as a cycle at 333 MHz, and it does not charge the
GE realistically. A number from it is an estimate of the CPU's list building. Compare them with
each other, and with nothing else.

The console's cost, from `docs/lazy-console-design.md` before the removal. Measured at `d3c0a26`,
the best of 5 batches of 200 frames, a frame's cost, **in builds with no optimisation: the ratios to
idle mean something, the times do not.** A Release build a commit or two before it took 43.0 us idle
on the desktop and 3,869.8 us in PPSSPP, with display, NMI and IRQ at 0.96-1.01 of idle; `d3c0a26`
itself has not been timed in Release:

| scene | desktop (Linux x86-64, headless) | PPSSPP, an instruction-count estimate |
|---|---|---|
| idle | 187.4 us | 24,262.0 us |
| display | 202.4 us, 1.08 of idle | 24,245.0 us, 1.00 of idle |
| NMI and joypads | 197.1 us, 1.05 | 24,307.3 us, 1.00 |
| IRQ masked | 198.9 us, 1.06 | 24,260.8 us, 1.00 |
| IRQ | 190.1 us, 1.01 | 24,276.1 us, 1.00 |
| an HDMA byte each line | 220.6 us, 0.139 us a line over the display's | 27,671.3 us, 15.322 us a line |
| a frame alone, among 100 | 203.4 us, 197.1 us | 24,224.5 us, 24,222.7 us |

Moves in a frame's cost, from the commit messages (`git log -- src/render/psp`), each the same
estimate:

| what | before | after |
|---|---|---|
| A screen canvas under two layers, windows, colour maths, brightness (present) | 544 ms | 8.3 ms |
| Canvas colour maths scenes | 630-760 ms | 15-18 ms (46 ms with HDMA's fixed colour) |
| Canvas scenes without colour maths | 650-875 ms | 18-20 ms |
| A canvas frame once on the CPU | 642 ms | 52 ms |
| Mode 1 interlaced test cartridge | 464 ms | 15.5 ms |
| Windows changing each line | 79 ms | 20 ms |
| Windows each line and CGRAM each line | 105 ms | 40 ms |
| A fixed colour changing each line (fixedsub, present) | 40 ms | 15 ms |
| CGRAM rewritten each line under a canvas | 40 ms | 31 ms |
| A plain canvas's present | 12.5 ms | 7.4 ms |
| OBJ interlace test cartridge | 20.8 ms | 16.7 ms |
| Mode 7 scene's present | 8.6 ms | 8.1 ms |

## Building it and running its checks

The toolchain is pspdev (built here with `psp-gcc` 15.2.0). `references/`, which is gitignored and
so is not on this branch, holds `psp_sdk_660`.

```bash
cmake -S . -B build/psp -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/psp/share/pspdev.cmake -DNSHVC_HOST=psp -DNSHVC_TESTS=ON -DNSHVC_EXAMPLES=ON
```

```bash
cmake --build build/psp
```

The checks run in PPSSPP's headless runner, which has to be fetched again; the copy used was
deleted when the work stopped. See "The PSP's programs in PPSSPP" in `tests/README.md`:

```bash
PPSSPPHeadless ELF -l --graphics=software --timeout=SECONDS
```

A program's arguments go in `args.txt` beside it (`tests/psp/args.c`). `-l` is needed: without it
the output reaches nothing and the run still exits 0. Frames: `canvas32_psp` 230, `canvas_check`
120, `present_check` 60, `woven_check` 60, each with `NSHVC_RRCHECK=1`; `scaling_check` takes none.
`ge_vram_check`, `host_check` and `bus_cost` are in `tests/psp`. `bus_cost` times accesses by the
PSP's own clock, and is the nearest thing here to a hardware timer, though it has only ever run in
PPSSPP.

This branch as cut was compiled with that toolchain, 0 errors and 0 warnings. **It was not run:** the
merge of main's newest 8 commits with 2.22 has not been through PPSSPP.

## What was never done

- Anything on a real PSP.
- A frame's time split into game, Super FX, console and rendering.
- Text entry (the system's on-screen keyboard).
- Contract 2.22's GE forms in a shipping game path: Stunt Race FX's recorded hand-off runs were on
  the GE under PPSSPP alone, and its GE code has since left that game's `main`.

## Picking it up against a moved-on main

1. **Measure first.** On a real PSP, split a frame into game, Super FX, console and rendering. Until
   then, the reason for shelving remains a suspicion, and any fix is a guess. `bus_cost` and
   `NSHVC_FPS_LOG` are a start.
2. **Merge `main` in and take the deletions back.** `main` removed these: the files under
   `src/render/psp`, `src/host/psp`, `tests/psp` and `src/console/coro_mips.S`; the CMake options; and
   the conditionals in `coro.h`, `coro.c`, `jump.h`, `script.c`, `nshvc.h`, `present_check.c`,
   `backdrop_check.c`, `overlay_check.c`, `tests/ref/check.c`.
3. **Put back what only the GE used.** These carry no PSP name on `main`, so a search will not find
   them missing:
   - the renderer hooks `rr_mode7_window`, `rr_mode7_plain`, `rr_compose_canvas`, `rr_screen_max`,
     `rr_canvas_max`, with `canvas_fits`, `canvas_k_max`, `ppu_canvas_k`, `ppu_set_mode7_plain`,
     `ppu_set_compose_canvas`;
   - in the description, `CP_VCOPY`, `CP_VGEN` and `vram_gen`, and each sprite entry's place in the
     priority order (bits 24-30). `rr_spv.h` is regenerated after the header changes;
   - the 64-byte alignment of the canvas's buffers (`cv_fill`), which lets the GE read them in place;
   - `NSHVC_SCALE=target`, for a host with one fixed target.
4. **The public API comes back as an addition.** `main` removed `nshvc_ext_ge_vram`,
   `nshvc_ext_ge_vram2`, `nshvc_ext_canvas_screen_ge`, `nshvc_ext_canvas_chars_ge` and the VRAM form
   of `nshvc_ext_overlay_gpu` in contract 3.0, so they would be a new 3.x.
5. **The game's side.** Stunt Race FX keeps its GE renderer at its tag `psp-tabled` (8041c535), which
   linked against `contract-2.22` for the hand-off.
6. **Rethink the standard if the numbers say so.** The renderer shape here was chosen to keep one
   description and exactness on a GPU with no programmability. Whether that is right for this machine
   is exactly the question that was never answered.
