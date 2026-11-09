# The native engine: architecture and plan

**Status: proposed, 2026-11-08, awaiting the maintainer's approval and decisions A to J in section 9. Nothing here is built
or run.** The evidence it rests on is in appendix A.

## 1. What nativeshvc is

> nativeshvc is a native engine for decompiled SNES games. It provides what the SNES provided, natively, with zero
> emulation. A game uses it once its C is converted to native code calling nativeshvc's API. Games that are not converted
> can still be played from their original ROM in any emulator.

It is to decompiled SNES games what Unity is to its games: a comprehensive engine. Games write against one API and build
for every platform it supports.

## 2. The maintainer's direction (2026-11-06 to 08)

- Native, not emulated: graphics on the GPU, logic on the CPU, memory in RAM, audio on native audio, input native and mapped
  automatically, storage on the drive.
- "a cmprehsensive 'engine' like how Unity is a comprehensive engine."
- "If the games are having to do unique things for each platform, and they don't just work out of the box, then our
  architectur eis wrong."
- True multithreaded. No stops without a reason the game can see.
- "There would be no 'original' or 'enhanced'. It would just be the true native library, and the game would utilize it as
  it chooses too. No faked timing, no hardware emulated."
- No console speed: a game with speed limits in its code fixes its code. Anyone who wants the game as it was has the
  assembly and bsnes.
- The SNES's split stands: the console provides the pipeline, and the cartridge brings its own chips.
- nativeshvc is done first and completely, on a new branch, then SRFX on its own. Old and new never live side by side. No
  shims, no hacks, no shortcuts.

## 3. Principles

1. **The engine does what the console did. The game does what the cartridge did.**
2. **The game says what it means,** through plain calls. It never names a register, address or hardware port.
3. **The engine owns the loop** and calls the game at set points. It never interrupts the game.
4. **No platform code in a game.** If a game needs some, the engine lacks something, and it is added to the engine.
5. **Zero emulation.** No bus, registers, emulated processor, chip model, DMA or HDMA engine, interrupt or console clock.
   The engine follows the SNES's rules for how things look and sound, because those rules are the console's output.
6. **No console speed.** Time is real time and the game's chosen update rate.
7. **No modes.** The SNES's limits are settings a game chooses.
8. **Threads by design.** Work passes between threads as finished results and messages, with no shared clock. The same input
   always gives the same game state. This is how the engine always works, not a mode.
9. **The GPU makes every pixel.**

## 4. What the engine provides

| Area | Provides | Replaces today's |
|---|---|---|
| **Loop and time** | Calls the game's update at the rate it chooses. Calls the game's draw once per picture, as often as the display allows, telling it how far it is between two updates, so the game says where things are. The engine interpolates nothing itself, because only the game knows what moved | Coroutines, interrupts, waits, the master clock, the cost profile |
| **Threads** | System, game, render and audio threads, and a job pool for the game's own work, joined in a fixed order | One thread |
| **Picture** | Layers, tiles, maps, palettes, sprites, windows, colour maths, mosaic, Mode 7, hires, interlace, direct colour, offset-per-tile; any screen size; more layer and sprite space; high-resolution art; the SNES's limits as settings | PPU registers, VRAM ports, the line record, the extensions |
| **Per-line changes** | A table of values by line, with an optional split at a column | HDMA, raster interrupts, mid-line writes |
| **Drawing pipeline** | The way a game's chip work reaches the screen: polygons, lines and images, drawn on the GPU at the resolution the game chooses, with an exact-span fill for original-looking polygons. Composed with the rest of the picture like a layer | The chip connector, the lent GPU device, GPU canvases and handles |
| **Effects** | Engine features such as water, sky and haze, if decision B stands | Games' own shaders |
| **Display** | Any window size and shape, sharp scaling, Mode 7 anti-aliasing, on the render thread | Presenting on the game's thread |
| **Sound** | Sample loading; voices with pitch, volume, envelope, noise, echo and FIR; the SNES's sound rules, mixed at 32 kHz by default; an audio thread that calls the game's music code at the rate it chooses; output to the system's audio; a one-way queue from the game's logic to its music code; the 8-voice and 64 KB limits as settings. Music code on the audio thread must never wait, lock, allocate or block, or the sound breaks up, and the engine reports any call that would |  The SPC700, ports, timers, boot protocol, sound RAM, register-level DSP |
| **Input** | The game declares its controls. The engine maps every keyboard, controller, mouse and touch screen to them, adds on-screen controls where needed, and names each control as it is labelled. Joypad, mouse, multitap and light gun as native inputs | Ports, latches, bit protocols, each game's touch code |
| **Saves and states** | Named data blocks in the system's save folder, written off the game's thread. Save states as ordinary data: the engine's state, the game's declared blocks, and the music code's declared state, taken from the audio thread at a point where it is whole. An import and export hook for a game that converts its saves to and from the original's format (decision J) | Battery RAM mapping, `fork` and replay states |
| **Game data** | Loads the game's art, samples and data from its package on every platform, including inside app bundles, in the form decision G sets | `rom`, `rom_pages` and `rom_read` |
| **Hosts** | Every supported platform. A game never sees which | |
| **Build** | One command naming a platform builds and packages a game, with its icon and resources | Each game's build and packaging scripts |
| **Integration kit** | A conversion guide, a hardware-touch finder, a checking setup against bsnes, and a complete example (section 6) | The single-prompt integration |

## 5. What the game brings

- **Its logic,** as native C with typed data and plain arguments.
- **Its art, samples and data.**
- **Its music code and songs,** in its own format, running on the audio thread and calling voices.
- **Its chip's logic,** as native C feeding the drawing pipeline.
- **Its choices:** update rate, screen size, which limits to keep, and which parts of the engine to use.

The engine requires a game's hardware-facing code to be converted: hardware access, interrupts, sound driver and chip code.
The maintainer's zero-emulation rule also requires its internal register style to go: the `cpu` register file, flag
helpers and data at SNES addresses.

## 6. Integration

How a decompiled game becomes a native game on the engine:

1. **Find:** the hardware-touch finder lists every register access, DMA, interrupt, wait and port use in the game's C.
2. **Translate:** the conversion guide maps each to what it meant and the engine call for it. For example: a scroll register
   write becomes a layer scroll; an HDMA colour table becomes a per-line table; an interrupt that blanks and uploads becomes
   work in the engine's frame call; a DMA to video memory becomes a tile load; port traffic to the sound driver becomes a
   message to the game's music code.
3. **Convert sound and chips:** the music code drives voices, and the chip code feeds the drawing pipeline.
4. **Clean up:** the register style goes from the game's internal code.
5. **Check, after every area:** the checking setup compares what the game shows, plays and stores with the original running
   in bsnes.

SRFX, once converted, is the worked example of every kind of change.

## 7. What goes, and what it costs

**Goes:** the bus, registers, DMA and HDMA engines, modelled interrupts, the master clock, the SPC700 and its ports and boot
protocol, the chip connector, coroutines, the lent GPU device, the cost profile, Original and Enhanced modes, `fork`
states, and the checks of what goes. A tag, `exact-console-3.0`, marks the last register-level console, for history. Nothing
builds against it and it is not maintained.

**Costs:**

- No decompiled C runs without converting it. The single-prompt integration goes.
- The claim of matching bsnes read for read, to the clock, goes. bsnes stays the reference for the rules: what a layer, a
  blend or a voice should produce.
- The original's slowdown is gone from native builds.
- No real game runs on the engine until SRFX is converted. Recorded game data reduces that risk, but an API gap could still
  show up late. Filling one is a change to the engine's API, made properly.
- The engine pays off only if more than one game uses it.

## 8. The stages

On one branch, `native-engine`, cut from `main`. `main` stays as it is until the maintainer approves a merge. Each stage
deletes the old code it replaces in the same change, so the branch never holds two versions of anything. Every test run and
push needs the maintainer's typed approval, with its length stated first. Approving this plan requests the new tooling in
stages 10 and 11, and nothing else.

### 0. Set up

Tag `exact-console-3.0` and cut the branch. Measure today's release-build frame costs for comparison (a run, on approval).
Set the speed targets in decision H from that baseline.

**Done when:** the branch exists, the baseline is recorded here, and the targets are set.

### 1. The API

Write every call in section 4 as the contract, from the current feature lists and SRFX's measured use (appendix A). No register name or address appears in it.

**Done when:** every current console and extension feature maps to a call, or is listed here as retired with the reason, and
the maintainer has reviewed it.

### 2. Loop, time and threads

**Build:** the loop, the two rates (the game's update and its draw, as section 4 defines them), interpolation (the draw
told how far it is between updates), the threads and job pool, and the deterministic mode, which is always on (principle 8).

**Delete:** `bus.c`, `dma.c`, `dispatch.c`, `coro.c` and its assembly, `jump.h`, `wide.h`, `snes.h`, `snes_internal.h`,
`machine.h`, `spc.h`, `cart.h`, `hw.h`, the cost profile and `tools/nshvc-profile`.

**Done when:** a test game runs at its chosen rate, no game code is entered except at the set points, and repeated runs under
varied thread timing give identical state.

### 3. Picture and display

**Build:** every picture feature in section 4, video memory held on the GPU with only changes crossing, the Mode 7 plane and
sampling on the GPU, and the description and submission on the render thread and jobs. Whether separate passes per layer are
faster is decided here, by measuring.

**Keep and adapt:** `compose_body.h`'s rules, the backends, sharp bilinear and Mode 7 anti-aliasing.

**Delete:** `ppu.c`'s registers, ports, line record, STAT77 and `cgram_target`; `rr_image`, `rr_mode7`, `host_present`;
`rr_m7_plane` and `rr_m7_tables`.

**Done when:** no code in `src` makes a picture pixel on the CPU, the display cannot hold up the game, and every feature scene
matches bsnes's picture on all three backends.

### 4. Drawing pipeline and effects

**Build:** the drawing pipeline in section 4, and the effects if decision B stands. Lists are built with jobs and drawn on the
GPU on every backend.

**Delete:** the lent GPU device, GPU canvases, and overlay handles.

**Done when:** test scenes match their definition on all three backends with no CPU pixel, and a recorded SRFX frame's
polygons (stage 10) fill identically to the original's bitmap.

### 5. Sound

**Build:** everything in section 4's sound row.

**Keep and adapt:** `mixer.c`'s arithmetic, without the read-clock tables; `resample.c`.

**Delete:** `apu.c`, its ports, timers and boot protocol, sound RAM as an address space, and register-level DSP access.

**Done when:** given the same voice calls, output matches the DSP sample for sample; a test game's music code runs on the audio
thread at its chosen rate with no platform code; and the audio thread never waits on the game's logic.

### 6. Input

**Build:** everything in section 4's input row.

**Delete:** `ports.c`'s latches and bit protocols.

**Done when:** a game with no platform or device code plays with every device on every host.

### 7. Saves, states and game data

**Build:** everything in section 4's saves and game data rows.

**Delete:** battery RAM mapping, `fork` and replay states, and the ROM reading in `nshvc_cart`.

**Done when:** saves and states restore identical game state on every host, with the music resuming exactly where it was,
and a test game loads its data on every platform in the form decision G sets.

### 8. Hosts

Every platform in decision C, on the new core, including a new windowed Linux host.

**Done when:** the same test game runs unchanged on each, and has been seen to on each.

### 9. Build

**Done when:** one test game's source, with no platform files, builds and packages for every platform with one command.

### 10. Checks

**Delete:** `tests/rom` replay, `ipl`, `waits`, `chip`, `chip_irq`, `hdma_bulk`, `scaling_check`, and every check of what was
deleted above.

**Build:**

- Each feature scene through the API, against bsnes running the same scene as a test ROM.
- A recorder that saves SRFX's picture state, voice events and polygons per frame from `sfxbox`, as plain data.
- A replay of those recordings through the API, compared with bsnes's pictures and sound. It is test code, never in a game's
  build.

Each check is shown able to fail first.

**Done when:** every check passes, on approved runs, on all three backends.

### 11. Integration kit

**Build:** the conversion guide, mapping every SNES hardware use to its engine call with examples, and setting out the audio
thread's rules for music code; the hardware-touch finder; a
reusable checking setup against bsnes for any game; and a complete example game in `examples/`, using every part and replacing
`hello`.

**Done when:** a reader with a decompiled game and the kit knows every change to make and how to check each one, and the
finder lists every hardware touch in SRFX's current C.

### 12. Documents

Rewrite the README, contract and `nativeshvc.md` as the engine's, opening with section 1's statement. The preservation
standard becomes: the assembly and proven C are the record, and the native game is built from them on the engine. Remove
`lazy-console-design.md` and `cost-profile.md`, and update the playbook.

**Done when:** the documents describe only what exists.

### 13. Finish

Measure release-build speed per thread on each platform. Audit `src` against principles 1 to 9.

**Done when:** the audit is clean, the numbers meet decision H's targets on every reference machine, and the maintainer approves
the merge.

## 9. Decisions needed

| # | Question | Recommendation |
|---|---|---|
| A | Does the engine own the loop, as Unity does? The maintainer has said nativeshvc is an engine like Unity, and principle 3 assumes yes, but a direct yes is still pending. | Yes |
| B | May games write GPU programs? No keeps weaker GPUs possible, but every effect has to be an engine feature. | No |
| C | Which platforms count for "complete"? | Windows, macOS, iOS, Linux, headless |
| D | OpenGL 3.3 has no compute shaders. Does it stay? | Keep it if stages 3 and 4 fit in fragment passes; otherwise bring the cost to the maintainer |
| E | Must the game's internal register style go in the same conversion as its hardware-facing code? | Yes, as the zero-emulation rule says, area by area |
| F | Where does a native game's code live? SRFX's rule 20 says fixes go into the decomp's C, with no separate copy, but conversion changes every file. | The decomp's C stays untouched as the record, and the native game is a new tree in the same repository, checked against it |
| G | Where does a game's data come from? Today a build carries no ROM code, and its data comes from the ROM or the player's own copy. | As today: the game ships no ROM code or data, and its build step extracts the data it declares from the player's own ROM into its package |
| H | What speed must the engine reach, and on which machines? | Name a reference machine per platform, among the slowest each supports. Set a time budget per frame for the engine's own work on each thread, from stage 0's baseline, before stage 2 |
| I | nativeshvc is AGPL-3.0, so every game linking it must publish its full source under the AGPL. Is that right for an engine meant for every decomp? | The maintainer's call: keep AGPL; add a linking exception; or choose a licence that allows any licence for a game |
| J | Native saves are typed data, so they no longer match the original's `.srm`. Must saves move between a native build and an emulator? | The engine provides an import and export hook. Each game decides whether to write a converter, because only it knows its data's layout |

## 10. After the engine: SRFX

SRFX gets its own branch, with a plan of its own, only after stage 13. Its record, the assembly and the proven C, stays
untouched, and the native game is converted from it (decision F). It is not playable on the engine
until its conversion is complete enough, so that plan sets out how each area is checked before then. Its conversion is the
first full use of the integration kit.

## 11. What is not known

- Whether the API covers a real game before one runs on it.
- Whether the voice calls cover every game's sound driver. SRFX's drivers are the first test, and their data has not been read.
- How much the threads gain, until stage 13.
- How much of a game's logic can split across threads. That is game work, after this plan.
- How much of a conversion the tools can automate.
- The size of the work. No duration is claimed.

## Appendix A. The evidence

Read-only findings from 2026-11-06, from four separate reviews of the code and checked directly where cited. Nothing was
run.

### The maintainer's earlier written rules

The maintainer's standing rules, already written down in the game's repository
(`ports/stuntracefx/PLAN.md:23-32`, `HANDOFF.md:62-79`, headed "The rules (Jeff's, all standing)"):

> **Zero emulation when done.** The finished port is the game's own source as native C over native parts:
> no bus (rd8/wr8), no console register files or register-number interfaces, no CPU/SPC700/Super FX
> register state, no DMA/HDMA engines, no chip models [...] Register-shaped steps are allowed one verified
> move at a time and must be removed before the work is done.

> Each native part takes calls that say what the game means (a layer's tiles and scroll, a palette, a
> sprite list; a voice's sample, pitch, envelope; the echo) and does it natively

Two later decisions also stand:

- **Behaviour exact, not cycle exact** (2026-10-28, `docs/stuntracefx/PLAN.md:78-88`).
- **Fixes go into the decomp's C, with no separate copy** (rule 20, `docs/stuntracefx/PLAN.md:275`). This
  one conflicts with the zero-emulation rules. See decision F.

### Where the code stands

#### The console

| Part | Today | Evidence |
|---|---|---|
| Bus | **A 24-bit bus with regions, timing and open bus** | `machine.h:74-161`, `bus.c` |
| Register interface | **Register numbers, down to the fetch rules** | `bus.c:1127-1135`, `ppu.c:409-568` |
| DMA and HDMA | **Engines, stepped per channel and byte, except for some bulk paths** | `dma.c:203-253`, `:318-405` |
| Interrupts | **Modelled at their clocks, with the hardware's sequence** | `bus.c:846-873`, `dispatch.c:148-175` |
| Clock | **A 21.477 MHz master clock, including refresh** | `bus.c:35`, `:660-673` |
| Chip | **A connector for a chip model, run by coroutine** | `cart.h` |
| Sound | **SPC700 ports, timers and boot protocol, and the DSP's read order** | `apu.c`, `mixer.c` |
| Drawing | Mostly GPU, with CPU work below | Below |
| Threads | **One, for the game, the console, the sound, a chip and the drawing submission** | `console.c:1229-1234` |

This matches what `ports/stuntracefx/PLAN.md:13-15` set out to remove: "a model of the console under it
(bus, PPU, DMA and HDMA, Super FX registers, sound unit, DSP, coroutines)".

**So by the maintainer's own definition, today's console is mostly a hardware model.** It has no emulated
processor and runs no machine code. Its internals often compute results directly where bsnes would step
through them. But it recreates the hardware's interface and timing, closely enough that a difference from
bsnes counts as a defect.

#### The game

The source is about 140,000 lines. Recounted from `src/stuntracefx`, leaving out sound and Enhanced for the
first four rows:

| What | Count |
|---|---|
| `rd8`, `wr8`, `rd16`, `wr16`, `rd24` calls | 9,169 |
| `cpu.` register file uses | 16,025 |
| `abi_` routines | 3,225 |
| 65816 instruction helpers (`op_*`) | 3,428 |
| Super FX register state uses (`gsu.`, `R[`, whole tree) | 21,076 |
| Sound driver lines, instruction by instruction | about 17,000 |
| Threads, jobs or atomics in the game | 0 |

**Runs natively, but is not native in shape.** The C is compiled and no machine code runs, but much of it
carries the original's registers and flags (`cpu_ops.h:1-5`, `race_frame.c:1150-1185`), and the sound
driver is translated line by line from SPC700 instructions (`spc_race_driver.c:495-510`).

Some of it has moved further. `remaster/` is mostly native C. A GPU scene path also exists that makes no
pixel on the CPU, but it needs `SC_GPU`, which nothing in the build sets, and its documents record no run.

#### Drawing

The console's picture is computed per pixel in shaders, from one source. These are CPU picture work:

1. The Mode 7 plane: 1,048,576 texels each time that VRAM changes (`rr_mode7.h:223`).
2. Sampling tables per window column and row (`rr_mode7.h:162`).
3. One pixel's rules, run on the CPU per mid-line CGRAM access, with a whole VRAM copy each time
   (`ppu.c:1950-2031`, `:1986`).
4. The 3D in the default build: polygons become spans on the CPU, the GPU or CPU fills them, and the picture
   comes back as CPU texels (`fullwindow.c:4716-4729`). Without a usable GPU the fill is CPU too.
5. Whole VRAM moved three times a frame on the CPU: live, record, description, then GPU memory
   (`ppu.c:1377`, `:1812`, `rr_vulkan.c:1016`).

#### What the hardware model's exactness actually delivers

The check that holds the console to bsnes "to the clock" (`rom/run.py`) replays **bsnes's own CPU accesses at
bsnes's clocks** (`tests/README.md`). A real game does not run that way. Its C makes other accesses at
other times. The game's README says it makes 0.70 times the reads, 0.60 the writes and 0.14 the I/O that the
weights were fitted to (`README.md:155-160`, a claim not rechecked). So in play, clock-level exactness is only
real just after a wait or an interrupt. Most of what the check proves cannot reach a game.

#### What the game needs from the hardware

Static reading, nothing run:

| Need | Found |
|---|---|
| Interrupts | 3 a frame in a one-player race, 4 in two-player, each at a line and H position. Bit 7 of `$4200` is never set |
| Mid-line writes | After an H interrupt or an H-blank wait. No dot-exact effect found |
| HDMA | Per line, on 7 channels at most: TM, windows, scroll, colours. Tables almost all in work RAM |
| DMA | Channel 0, in blanks, and at load |
| Reads that depend on time | No H counter. No STAT77. One V counter wait, plus one more use its comments call dead. Blank flags as gates only |
| Sound to game | No port value used as game state. Echoes only tested for zero, and equality during uploads |
| Sound timing | Three places rely on it: a word store's tear during reload, fixed settle delays after uploads, and overwriting a port with no acknowledgement. All are load or queue plumbing |
| DSP read-back | ENVX, OUTX and ENDX never read. One read of EDL |
| Plotted pixels | Reach the screen by DMA. Other reads of them: none found, not fully enumerated |
| Pacing | The race loop never waits, so race speed is how long the work took. This is the one real time dependence. Under principle 6 the engine has no console speed, so the game fixes this in its own code |

This is one game. Other games may need more, which the per-line table's column split allows for (section 4).

#### How it got here

- 2026-10-26: the first port's plan was zero emulation, with native parts per game.
- 2026-10-26 to 28: the game moved onto nativeshvc as a cartridge, so every game could share one console.
- 2026-10-28: the maintainer decided behaviour exact, not cycle exact. In the same commit that documented this
  (`56eebdaa`), the zero-emulation plan was marked superseded.
- No record was found of the maintainer dropping the zero-emulation rules. `HANDOFF.md` still calls them
  standing.

Sharing a console was a good change. Keeping that console's interface as hardware registers is what carried
the hardware model forward.
