# How the engine is built

- `src/engine`: the loop and threads (`loop.c`), jobs (`jobs.c`), the picture calls (`picture.c`), sound (`sound.c`), input
  (`input.c`), saves and game data (`files.c`), inspection (`inspect.c`).
- `src/render`: `picture_body.h` holds the SNES's rules for one pixel, in one source for every shading language. Each backend
  compiles it and draws two fragment passes: the picture at its own size, then the window's pixels.
- `src/host`: what each operating system supplies, through `host.h`.
- `src/dsp`: the resampler.

**A new platform** implements `host.h`, and `rr.h` if it needs a new GPU API. Nothing else changes.

**What crosses to the GPU.** Tiles, maps and images live in one store on the GPU, and a picture sends only what changed since
the last. `draw` sets the whole picture and nothing carries over but those objects and the palette.

**The preservation standard.** The assembly and proven C are the record, and the native game is built from them on the
engine.
