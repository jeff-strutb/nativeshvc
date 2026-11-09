# nativeshvc

nativeshvc is a native engine for decompiled SNES games. It provides what the SNES provided, natively, with zero emulation.
A game uses it once its C is converted to native code calling nativeshvc's API. Games that are not converted can still be
played from their original ROM in any emulator.

It is to decompiled SNES games what Unity is to its games: a game writes against one API and builds for every platform the
engine supports.

**Maintainer:** Jeffrey Wilbur, Strut B, LLC\
**Contact:** [retro@strutb.com](mailto:retro@strutb.com)\
**Licence:** AGPL-3.0

## What it provides

- **A loop.** The engine calls the game's `update` at the rate it chooses and its `draw` once per picture, telling it how far
  it is between two updates.
- **Threads.** System, game, render and audio threads, and a job pool. Game state depends only on the updates run and their
  input.
- **The picture on the GPU.** Layers, tiles, maps, palettes, sprites, windows, colour maths, mosaic, Mode 7, hires, direct
  colour and offset-per-tile by the SNES's rules, at any screen size. Per-line changes come from a table of values. Mode 7 is
  sampled and anti-aliased at the window's resolution. Backends: Vulkan, Metal and Direct3D 12.
- **A drawing pipeline.** Triangles, lines and images on the GPU, composed as a layer.
- **Sound.** Voices, envelopes, noise, pitch modulation and echo by the SNES's rules at 32,040 Hz, with the game's music code
  on the audio thread.
- **Input.** Keyboards, controllers, mice and touch screens mapped to the game's controls.
- **Saves and game data.** Native files, written off the game's thread, and packaged assets.

## Using it

Read [docs/api.md](docs/api.md) for the contract and [docs/conversion-guide.md](docs/conversion-guide.md) to convert a game.
`tools/nshvc-find-hardware` lists what a game's C must change.

```bash
tools/nshvc-build macos path/to/game
```

Platforms: `headless`, `macos`, `ios`, `windows`, `windows-d3d12`, `linux`.

## Running

```
game                                  in a window
game --headless --updates N           N updates, one picture each, no real time
     --input FILE                     input by update number
     --sound FILE                     16-bit stereo at 32,040 Hz (headless)
     --inspect FILE                   each picture's state, each time F12 is pressed
```

## The SNES's limits

Limits are settings a game chooses: `nshvc_limits_set` for sprites per line, `nshvc_sound_limits_set` for voices and sample
memory. By default there are none.
