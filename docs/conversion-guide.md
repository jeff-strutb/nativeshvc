# Bringing a decompiled game to the engine

The steps are in order. `docs/api.md` is the full contract, `examples/comet` a complete small game on it, and section 8 is a
full game's conversion.

## 1. Find every hardware touch

`tools/nshvc-find-hardware src/` lists each one with its replacement.

## 2. The hardware and the calls that replace it

| On the SNES | On the engine |
|---|---|
| Reset vector, main loop | `nshvc_game`, `nshvc_run` |
| NMI handler's game logic | `update` |
| NMI handler's uploads | `draw`, or object writes when the data changes |
| Waiting for vertical blank | Return from `update` |
| IRQ on a line, H-blank write | `nshvc_line`, `nshvc_line_from` |
| BGMODE, BGnSC, BGnNBA, scroll | `nshvc_mode_set`, `nshvc_layer_set` |
| TM, TS | `enabled_main`, `enabled_sub`; bit 4, the sprites, is `nshvc_sprites_show` |
| M7A to M7D, M7X, M7Y, M7SEL | `nshvc_mode7_set` |
| Window registers | `nshvc_windows_set` |
| CGWSEL, CGADSUB, COLDATA | `nshvc_blend_set` |
| INIDISP | `nshvc_screen_set`: `brightness`, `blank` |
| The television's shape | `nshvc_screen.aspect`: 0 is 4:3 for 256 x 224 |
| DMA to VRAM | `nshvc_tiles_write` (`NSHVC_TILES_PLANAR` takes the data as it is), `nshvc_map_write_snes` |
| DMA to CGRAM | `nshvc_palette_write` |
| OAM buffer | `nshvc_sprites_set` |
| HDMA tables | `nshvc_lines` |
| Joypad registers | Your own controls, read from the devices in the `nshvc_input` passed to `update` |
| APU ports | `nshvc_music_send`, read by your music code with `nshvc_music_receive` |
| Sound driver | Music code in `nshvc_music.tick`, calling `nshvc_voice_*` |
| Super FX or other chip | C that fills an `nshvc_draw_list` |
| Battery RAM | `nshvc_save_write`, `nshvc_save_read` |
| ROM data | `nshvc_asset_open`, or data compiled in |

Multiplier, divider, H and V counters: use ordinary C.

## 3. Rules for music code

`nshvc_music.tick` runs on the audio thread. It must never wait, lock, allocate, do file work or block. It reaches the game
only through the one-way queue.

## 4. Clean up

Replace the `cpu` register file, flag helpers and data at SNES addresses with ordinary C in the same pass. The engine never
sees this code, but shared register state stops a game using threads.

## 5. Pacing

A game whose speed came from how long its work took must set that speed in its own code. The engine has no console speed.

## 6. Build

Your `CMakeLists.txt` adds the engine from `NSHVC_ROOT`, which `tools/nshvc-build` sets, and calls `nshvc_game()`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(mygame C)
add_subdirectory(${NSHVC_ROOT} nshvc)
include(${NSHVC_ROOT}/cmake/NshvcGame.cmake)
nshvc_game(mygame SOURCES src/a.c src/b.c ASSETS data SHADERS shaders ICON icon.png TITLE "My Game")
```

`ICON` is one square PNG. The build makes each platform's icon from it: an `.icns` on macOS, an asset catalog on iOS
(compiled by Xcode's `actool`; iOS shows no transparency), an `.ico` linked into the program on Windows, the window's icon
and a PNG for a desktop file beside the program on Linux, and the launcher's on Android. It needs Python 3.

Building a game with shaders needs `glslangValidator` and `spirv-cross` on the build machine, and `dxc` for D3D12; without `dxc` other builds leave the D3D12 shaders out, with a warning.

Then run this, or name `ios`, `android`, `windows`, `windows-d3d12` or `linux` instead:

```bash
tools/nshvc-build macos path/to/game
```

## 7. Check

Run the game headless and compare what it plays with the original running in bsnes, and look at what it shows:

```bash
game --headless --updates 600 --input keys.txt --sound out.raw
```

`keys.txt` gives the devices by update number (`docs/api.md` section 7), such as `120 key=0D` to hold Return from update 120.

`tools/nshvc-check` does the comparison: it plays the ROM in bsnes and the game headless for the same frames, and compares the sound sample by sample. Pictures are not compared with an emulator's: the engine draws natively at the window's resolution (`--shots` writes each picture to look at). Its `inputs.txt` gives the original's buttons, one line per change: the update number, then each pad's buttons in hex. `--keys` names the key your controls map to each button, so the game is given those keys:

```bash
tools/nshvc-check --oracle build/tests/oracle --core bsnes_libretro.dylib --rom game.sfc --game "build/game" --input inputs.txt --keys B=5A,A=58,START=0D --frames 1800
```

## 8. Worked example: a full game

A full game's decompilation, about 80,000 lines of C, was converted to the engine as its first full test. The converted
game is in [`examples/`](../examples), with credit to its authors and the steps to build it; you supply your own ROM, from
which it extracts its game data. No picture of it on 6.0's renderer has been taken yet.

The conversion removed the game's imitation PPU, DMA, DSP and APU, its SDL front end, its OpenGL code and its snapshot and comparison code. `tools/nshvc-find-hardware` finds nothing left in it. The game logic did not change.

**The game's entry.** One small file replaces the front end:

| What the game had | On the engine |
|---|---|
| `main`, an SDL window and loop | `nshvc_game` and `nshvc_run` |
| The frame function | `update`, at 60 a second |
| Drawing line by line | `draw` |
| Its joypad word, B in bit 0 | Its own controls (`src/controls.c`): keys, controller buttons, the left stick and on-screen touch discs, each turned into the joypad word |
| Files on disk | `nshvc_asset_open` for data, `nshvc_save_write` and `nshvc_save_read` for saves |

**Graphics.** A small video module replaces video memory. Tile sets and maps are engine objects, and the game writes to them when its graphics change, in `update`. Most writes name what they are, such as background or sprite tiles. The game's data tables name some destinations by number, and one function turns those into the right object. Mode 7's tiles and map are the game's own arrays, sent when they change.

**The picture.** `draw` describes the picture directly from the game's own settings: which layers are on, the scrolls, windows, blending and the palette. Sprites come from the game's own table.

**Changes during the picture.** The game's per-line effects are arrays with one value a line, such as window edges for the spotlight or Mode 7's zoom for the world map. A few are short runs of lines. `draw` sends each change with `nshvc_line`.

**Music.** The game's sound driver is already C, so it became the game's music code:

- `nshvc_music.tick` runs at 32,040 Hz and counts samples: one driver step every 64 samples, the rate of the driver's timer.
- The driver sets voices with `nshvc_voice_*` directly.
- The game thread loads a sound bank's samples with `nshvc_sample_load`, then sends a message saying the bank is ready. Commands reach the driver through the same queue. The audio thread never waits.

**Widescreen.** The game sets a wider screen in `nshvc_screen`, moves sprites and windows into it, and uses `show_left` and `show_right` to black out what a room doesn't have. The status bar stays in the original columns through `layer_clip`. How to use the extra width is the game's decision.

**How it was checked.** Under API 5's renderer, since replaced, the attract sequence matched the ROM in bsnes byte for byte on the frames that lined up, and dungeons, the world map, the item screen, game over, boss scenes and the credits matched the game's previous renderer, apart from the timing of its 3D polygons. API 6.0's native renderer compiles with it for every platform and has not yet been run with it. Its frame timing differs from the ROM's, because the C does not model the original's slow frames, so its sound is the same music but not aligned sample for sample.

The test found a real engine gap, sprites drawn on the sub-screen when the game had not asked. When your game shows a difference, find out whether the conversion or the engine is wrong, and fix it where it belongs. The engine is never changed for one game.
