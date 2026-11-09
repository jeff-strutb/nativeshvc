# nativeshvc

nativeshvc is a native engine for decompiled SNES games. It provides what the SNES provided, natively, with zero emulation.
A game uses it once its C is converted to native code calling nativeshvc's API. Games that are not converted can still be
played from their original ROM in any emulator.

It is to decompiled SNES games what Unity is to its games: a game writes against one API and builds for every platform the
engine supports.

**Maintainer:** Jeffrey Wilbur, Strut B, LLC\
**Contact:** [retro@strutb.com](mailto:retro@strutb.com)\
**Licence:** AGPL-3.0

The engine is at API 4.2. `docs/native-engine-plan.md` opens with what has and has not been run on each platform.

## What it provides

- **A loop.** The engine calls the game's `update` at the rate it chooses and its `draw` once per picture, telling it how far
  it is between two updates.
- **Threads.** System, game, render and audio threads, and a job pool. Game state depends only on the updates run and their
  input.
- **The picture on the GPU.** Layers, tiles, maps, palettes, sprites, windows, colour maths, mosaic, Mode 7, hires, direct
  colour and offset-per-tile by the SNES's rules, at any screen size. Per-line changes come from a table of values. Mode 7 is
  sampled and anti-aliased at the window's resolution. Backends: Vulkan, Metal and Direct3D 12.
- **The picture's shape in the window.** 4:3 by default, centred with black margins. A game can name another shape, such as
  8:7, or fill the window. A wider screen keeps the original's pixel shape.
- **A drawing pipeline.** Triangles, lines and images on the GPU, composed as a layer, with game shaders compiled for every
  backend at build time.
- **Sound.** Voices, envelopes, noise, pitch modulation and echo by the SNES's rules at 32,040 Hz, with the game's music code
  on the audio thread.
- **Input.** Keyboards, controllers, mice and touch screens mapped to the game's controls.
- **Saves and game data.** Native files, written off the game's thread, and packaged assets.
- **Profiling and inspection.** Time per thread and per zone, and each picture's state on request.

## Using it

Read [docs/api.md](docs/api.md) for the contract and [docs/conversion-guide.md](docs/conversion-guide.md) to bring a game to
the engine, with zelda3 as a worked example. [examples/comet](examples/comet) is a complete small game that uses each part of
the engine.

| Tool | What it does |
|---|---|
| `tools/nshvc-find-hardware` | Lists what a game's C must change |
| `tools/nshvc-build PLATFORM GAME_DIR` | Builds and packages a game |
| `tools/nshvc-shaders SRC OUT` | Compiles a game's shaders for every backend (`nshvc_game` runs it) |
| `tools/nshvc-check` | Compares a converted game with its ROM in bsnes, picture and sound |

```bash
tools/nshvc-build macos path/to/game
```

Platforms: `headless`, `macos`, `ios`, `android`, `windows`, `windows-d3d12`, `linux`.

Android needs `ANDROID_HOME` and `ANDROID_NDK_HOME` and produces a signed APK. Building a game with shaders needs
`glslangValidator`, `spirv-cross` and `dxc` on the build machine.

## Running

```
game                                  in a window
game --headless --updates N           N updates, one picture each, no real time
     --updates N                      end after N updates
     --input FILE                     input by update number
     --shots DIR                      each picture, as DIR/NNNNNN.ppm
     --sound FILE                     16-bit stereo at 32,040 Hz (headless)
     --inspect FILE                   each picture's state, each time F12 is pressed
```

## Default keys

| SNES | Key | SNES | Key |
|---|---|---|---|
| D-pad | Arrow keys | Start | Return |
| B | Z | Select | Shift |
| A | X | L | Q |
| Y | A | R | W |
| X | S | | |

Game controllers work without setup, and a game can rebind any key with `nshvc_control_bind`.

## The SNES's limits

Limits are settings a game chooses: `nshvc_limits_set` for sprites per line, `nshvc_sound_limits_set` for voices and sample
memory. By default there are none.

## Checks

Build with `-DNSHVC_TESTS=ON`. `tests/scenes/scenes.py` makes each feature scene twice from one description, as a test ROM run
in bsnes and through the API, and compares them byte for byte. `tests/scenes/feature.c` checks draw lists, shaders, overlays,
saves, input replay and jobs.
