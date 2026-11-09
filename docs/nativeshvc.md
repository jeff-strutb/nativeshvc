# How the engine is built

- `src/engine`: the loop and threads (`loop.c`), jobs (`jobs.c`), the picture calls (`picture.c`), sound (`sound.c`), input
  (`input.c`), saves and game data (`files.c`), inspection (`inspect.c`).
- `src/render`: `picture_body.h` holds the SNES's rules for one pixel, in one source for every shading language. Each backend
  compiles it and draws two fragment passes: the picture at its own size, then the window's pixels.
- `src/host`: what each operating system supplies, through `host.h`.
- `src/dsp`: the resampler.

![How the engine is built](architecture.svg)

**Threads.** The system thread owns the window and events. The game thread runs `update` and `draw`, the render thread draws
each finished picture, the audio thread mixes voices and runs the game's music code, and jobs run on a pool. `update` runs at the game's rate, catching up at most 0.25 seconds; `draw` runs when the renderer is free.

**A new platform** implements `host.h`, and `rr.h` if it needs a new GPU API. Nothing else changes.

**What crosses to the GPU.** Tiles, maps and images live in one store on the GPU, and a picture sends only what changed since
the last. `draw` sets the whole picture and nothing carries over but those objects and the palette.

**The picture's shape.** The second pass places the picture in the window: 4:3 by default, centred with black margins, or the
shape `nshvc_screen.aspect` names. Overlays are placed in window pixels and can use the margins.

**Checks live outside the engine.** `tests/` and the bsnes oracle hold the test machinery; `src/` holds none of it. A check that
finds the engine wrong is fixed in the engine for every game, never by a change for one game.

**The preservation standard.** The assembly and proven C are the record, and the native game is built from them on the
engine.
