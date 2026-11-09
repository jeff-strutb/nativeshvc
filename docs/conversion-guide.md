# Converting a decompiled game to the engine

The steps are in order. `docs/api.md` is the full contract.

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
| TM, TS | `enabled_main`, `enabled_sub` |
| M7A to M7D, M7X, M7Y, M7SEL | `nshvc_mode7_set` |
| Window registers | `nshvc_windows_set` |
| CGWSEL, CGADSUB, COLDATA | `nshvc_blend_set` |
| INIDISP | `nshvc_screen_set`: `brightness`, `blank` |
| DMA to VRAM | `nshvc_tiles_write` (`NSHVC_TILES_PLANAR` takes the data as it is), `nshvc_map_write_snes` |
| DMA to CGRAM | `nshvc_palette_write` |
| OAM buffer | `nshvc_sprites_set` |
| HDMA tables | `nshvc_lines` |
| Joypad registers | The `nshvc_input` passed to `update` |
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
nshvc_game(mygame SOURCES src/a.c src/b.c ASSETS data TITLE "My Game")
```

Then run this, or name `ios`, `android`, `windows`, `windows-d3d12` or `linux` instead:

```bash
tools/nshvc-build macos path/to/game
```

## 7. Check

Run the game headless and compare what it shows and plays with the original running in bsnes:

```bash
game --headless --updates 600 --input inputs.txt --sound out.raw
```

`inputs.txt` has one line per change: the update number, then each pad's buttons in hex.
