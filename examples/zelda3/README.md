# zelda3 on the engine

[zelda3](https://github.com/snesrev/zelda3) is a decompilation in C. This folder runs it on nativeshvc, as a full game to test the
engine with. `docs/conversion-guide.md` section 8 explains how it works.

It is a bridge at the game's hardware layer, not a finished conversion: the game keeps its register-style state and the bridge
turns that into engine calls. The game's code and data are not in this repository, and you need your own ROM.

## Build

Use zelda3 at commit `45a149d`. You need Python 3 with Pillow and PyYAML, and SDL2's headers.

```bash
git clone https://github.com/snesrev/zelda3
```

```bash
git -C zelda3 apply path/to/nativeshvc/examples/zelda3/zelda3.patch
```

Put your USA ROM at `zelda3/assets/zelda3.sfc`, then extract the game's data from it, from `zelda3/assets`:

```bash
python3 restool.py --extract-from-rom --rom zelda3.sfc
```

Copy `zelda3/zelda3_assets.dat` to `examples/zelda3/data/`, then build:

```bash
ZELDA3_DIR=path/to/zelda3 tools/nshvc-build macos examples/zelda3
```

## What is not finished

- The game's own save code writes `saves/sram.dat` under the folder it runs from; it does not use the engine's saves yet.
- Only macOS has been built and run. The other platforms use the same code and have not been tried.
- Music follows the game's own sound chip in loudness and shape, not sample for sample.
