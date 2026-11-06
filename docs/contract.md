# The game contract, version 3.0

This is what a decompiled game writes against and what nativeshvc promises in return.
nativeshvc is a behaviour-exact implementation of the console, not an emulator. The
contract is at 3.0: the console is behaviour exact, the game's C states no time, and each
access it makes costs what the console's bus takes for it, and its cost profile's weight
(Changes, below). A
game written to it plays on every later 3.x without a change.

## How it changes

- **Major version (4.0):** the only kind of release that may change or remove anything a
  game writes or relies on below.
- **Minor version (3.1):** may only add things: a new call, a new extension, or a new field
  at the end of a struct that a zeroed struct leaves off. A 3.0 game builds and plays the
  same.
- **Patch version (3.0.1):** brings the console closer to the hardware where it differed
  from the reference (bsnes, accuracy profile). The contract is the hardware's behaviour,
  so this is a fix, not a change of contract.
- `NSHVC_VERSION_MAJOR`, `_MINOR` and `_PATCH` in `nshvc.h` say which version a build is.

## What the contract is

| header | what it is |
|---|---|
| [`machine.h`](../include/nshvc/machine.h) | the S-CPU as the game's C sees it |
| [`spc.h`](../include/nshvc/spc.h) | the SPC700 as the sound driver's C sees it |
| [`nshvc.h`](../include/nshvc/nshvc.h) | the cartridge's description, running it, the controllers, the extensions, the front end's hooks |
| [`cart.h`](../include/nshvc/cart.h) | the cartridge connector, for a cartridge that brings its own chip |
| [`hw.h`](../include/nshvc/hw.h) | the registers' standard names (names only) |

Not in the contract (they may change in a minor version):

- The headless script's steps (`src/front/script.h`).
- The environment options (`NSHVC_STATES`, `NSHVC_SCRIPT_NOFORK`, `NSHVC_DEBUG`,
  `NSHVC_SPCLOG`).
- The host interface for `NSHVC_HOST=none` (`src/host/host.h`).
- Everything under `src/`.

## What the game does: the S-CPU

1. **Every bus access the original makes, in its order.** These go through `rd8` and
   `wr8`. A word or a long is one operation: `rd16`, `wr16` and `rd24`, low byte first. A
   read-modify-write instruction is `rd8_rmw` then `wr8`, or `rd16_rmw` then `wr16_rmw`
   (high byte written first). Dummy reads and the stack's pushes and pulls are accesses
   like any other.
2. **Its waits for the hardware, as waits.** A loop that waits for the hardware (a status
   bit, a counter, a chip's flag, the sound unit's reply) is `nshvc_wait_until(ready,
   ctx)`: `ready` makes the loop's reads and says whether it ends. The wait for the
   vertical blank is `nshvc_wait_vblank`. The clocks a wait returns let the C keep a
   counter the original ticked in each turn of the loop. The C states no other time.
3. **The register file at a routine's entry and exit.** A routine entered from an address
   (an interrupt vector, a computed call or jump, a chip's program) has an `abi_` wrapper
   that takes its inputs from `cpu` and leaves its outputs there. `cpu.unwound` says when
   C leaves through its caller's frame.
4. **The routine table** (`nshvc_cart.routines`, sorted by address). It lists every
   routine an address reaches: the reset and interrupt vectors' code (a stub in RAM
   included), every computed call's and jump's target, and any routine a chip runs by
   address. Reaching an address with no C is a fatal error; nativeshvc runs no machine
   code.
5. **Control the original's machine code has and C doesn't:**
   - `call_routine` and `jump_routine` (or `jump_routine_pushed`) for computed calls and
     jumps;
   - `cpu_loop_forever` for a branch to itself;
   - `cpu_restart` for code that resets the stack;
   - `cpu_wai` for WAI.

The game marks no instruction's end and says nothing about which instruction an access
belongs to.

## What the game does: the SPC700

1. **Every APU RAM and I/O access the driver makes, in its order.** These go through
   `spc_rd` and `spc_wr`.
2. **Its waits for the hardware, as waits:** `spc_wait_until(ready, ctx)` for a loop on a
   timer's counter or a port. The C states no other time.
3. **Calls and returns as the stack sees them** (`spc_call`, `spc_ret`).
4. **The routine table** (`nshvc_cart.spc_routines`, sorted by APU address). Each entry
   lists the ROM bytes it stands for (`uses`), so the console can pick the right routine
   when two uploaded programs share an address.
5. `spc_restart` for code that never returns to its caller, and `spc_boot_rom` for a jump
   to `$FFC0`.

The SPC700 takes no interrupts. The DSP, timers, ports and boot ROM are the console's.

## What the game does: the cartridge

- **The cartridge description** (`nshvc_cart`): its ROM (a whole image, or only the data
  pages with a reader for the rest), its mapping and region, its RAM and battery, the
  routine tables, and what is plugged into the controller ports.
- **Its own hardware.** An expansion chip is the cartridge's: the game brings it as an
  `nshvc_chip` (`cart.h`). For a board the chip decodes, the map is `NSHVC_CHIP` and the
  board's ROM decoding is the chip's `rom_at`. nativeshvc implements no chip.
- **Its main:** `NSHVC_MAIN` and `nshvc_main`. An app with its own loop calls `nshvc_power`
  and then `nshvc_run_frame` instead.

## What the console promises

- **Behaviour exact: everything but the processors and the cartridge's own hardware, as
  the hardware does, at the hardware's time.** This means:
  - the bus, work RAM, the mappings;
  - the CPU's registers (NMI, H/V IRQ, multiplier, divider, the joypads' automatic read);
  - DMA and HDMA, the PPU, the sound unit and DSP;
  - every controller in `NSHVC_PORT_*`.

  The reference is bsnes with its accuracy options. Where nativeshvc's hardware differs
  from it, that's a defect in nativeshvc, fixed in a patch release.
- **The S-CPU's time is its accesses'.** Each access the S-CPU's C makes takes the bus's
  time for it, as the console's memory speeds give it, and before it the weight of its
  kind from the cartridge's cost profile (`nshvc_cart.cost_profile`, docs/cost-profile.md),
  0 with none. The C states nothing; its own arithmetic between accesses takes no time. The
  clock also moves on in its waits (the waits above, WAI, a branch to itself) and while a
  DMA (a `$420B` write) stops it, for the DMA unit's real time. The multiplier and divider
  are done when read. With the S-CPU's costs off (`nshvc_set_cpu_costs(0)`, 2.2), its
  accesses take no time, and the clock moves only in its waits and while a DMA runs. Each
  access the SPC700's C makes costs its one bus cycle and the weight of its kind from the
  profile (0 with none); its clock also moves in its waits.
- **Pacing, in two modes.** A game whose pace is how much work fits in a frame has both
  (README.md, Enhancement support): an Original mode, paced on its work
  by a cost profile, which approximates the original's processor time, so its slowdown is
  approximate; and an Enhanced mode, made independent of processor speed (its pace set by
  the time that really passes and the display, as a properly built modern game's is, not
  by how much work fits in a frame), which nativeshvc runs exactly at any S-CPU cost, with
  its costs off (`nshvc_set_cpu_costs(0)`: its accesses take no time; the SPC700 keeps its
  own, and a cartridge chip is the game's to make speed independent). Both are
  behaviour correct and respect the original game. The original's slowdown was a side
  effect of the chips' speed, not something the game was written to do.
- **The sound as the original's.** After a KON or KOFF write, the SPC700's next DSP write
  waits until the DSP has taken that key event (every other sample), so every note the
  driver keys on or off reaches the DSP, in order, with the voice's registers as the
  original had them. A note sounds within the driver's own work in a beat (well under a
  millisecond) of when the original's did.
- **Interrupts in the console's time.** An interrupt comes due at the clock the hardware
  raises it; a chip's IRQ, at the clock the chip gives (`nshvc_cart_irq_at`). In a wait it
  is taken at that clock, its handler run before the wait looks again; one a DMA's time
  made due is taken at the end of the access that started it. No operation is split. The
  IRQ waits while `cpu`'s I flag masks it, and a handler may wait too (an NMI can still
  come in an IRQ's handler, as on the console).
- **A fatal error, never a guess,** when the game reaches something the contract doesn't
  cover: an address with no C (a vector's included), a wait while interrupts are held, or,
  where no time passes (interrupts held, the costs off), a loop that waits for the hardware:
  no frame for 30 seconds (`NSHVC_WATCHDOG=N` seconds, 0 none; headless, none unless set)
  ends the run, naming the last access made through the console. It's reported, then the
  process exits with status 70.
- **The console's numbers by default.** Every extension (`nshvc_ext_*`,
  `nshvc_hold_interrupts`) is off until the game asks for it, and with no cost profile an
  access costs the bus's time alone.
- **The same run on every host.** One thread, a deterministic schedule: the same input
  gives the same machine.

## Changes

- **3.0:** the PSP is no longer a platform. Removed: `nshvc_ext_ge_vram`,
  `nshvc_ext_ge_vram2`, `nshvc_ext_canvas_screen_ge`, `nshvc_ext_canvas_chars_ge`,
  `nshvc_ext_overlay_gpu`'s VRAM form, and `NSHVC_SCALE=target`. They were the GE's alone: on
  every other platform the loans returned NULL and the rest were fatal, so a game that calls
  none of them builds and plays as under 2.23. Every picture stays the platform's GPU's, and
  a frame its renderer cannot draw stays fatal. The screen's limit is the contract's 8192 x
  8192 everywhere, and no platform limit caps a canvas or `nshvc_ext_canvas_scale`. The
  entries below are the record as each version stood, the PSP's limits included.
- **2.23:** `nshvc_chip.next_change`, optional: when a register of the chip's that the S-CPU
  waits on could next answer otherwise, so the wait goes there and does not read it again every
  few clocks. A chip without it is waited on as before.
- **2.22:** pictures the GPU holds, each optional and reported by a query; a host without
  one goes on as before. No texel of them crosses to the CPU.
  - `nshvc_ext_gpu_device(&loan)`, the query on Metal and Vulkan: lends the host's device
    and queue (`NshvcGpuLoan`: Metal an `id<MTLDevice>` and `id<MTLCommandQueue>`; Vulkan
    the instance, physical device, device, queue and queue family) and returns
    `NSHVC_GPU_METAL` or `NSHVC_GPU_VULKAN`; 0 on a host with none (the PSP's query is
    `nshvc_ext_ge_vram2`). Vulkan's queue family has graphics and compute, `shaderInt64` is
    enabled where the device has it, and the game shares the console's one thread, so the
    queue needs no lock, a frame of the console's perhaps in flight on it.
  - `nshvc_ext_canvas_screen_gpu(layer, handle, w, h, format, priority, fence)`: a
    full-colour screen canvas at k 0, palette 0, from a texture in place of
    `nshvc_ext_canvas_screen32`'s texels. `handle` (`NshvcGpuHandle`) an `MTLTexture`, or a
    `VkImage` with its view in `SHADER_READ_ONLY_OPTIMAL`; `format` `NSHVC_GPU_FORMAT_METAL`
    or `_VULKAN`, 32-bit unsigned texels of the same encodings (0 transparent, `0x000000ii`
    an index, `0xFFrrggbb` a colour); `fence` (`NshvcGpuFence`) an `MTLSharedEvent` and
    value, or a binary `VkSemaphore`, waited on before the console reads it, 0 none. The
    console copies it on the GPU into the canvas's buffer once for each frame drawn from it,
    so every path of a frame (the presentation, the compositor, `nshvc_picture`) reads it as
    any full-colour canvas's: a Vulkan image needs `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`. Handed
    in the vertical blank or the frame hook for the frame about to begin, and kept through
    that frame's presenting until the next is handed or the frame hook after it ends; none
    handed by then, the canvas is off, so a game hands one each frame (its fence 0 for a
    picture that stands). A hand in the vertical blank after a hook that handed none counts.
    While paused no frame is made and it stays, handed or not, for the frame after the
    pause. An interlaced frame showing it is fatal.
  - `nshvc_ext_canvas_screen_ge(layer, vram_address, stride, w, h, psm, priority)`, the
    PSP's: the same canvas from VRAM in the loan, read straight by the console's GE list, no
    copy to the CPU. `GU_PSM_8888` alone, its words as the GE drew them (`0xttbbggrr`): tt
    (the GE's alpha, the stencil) not 0 a colour; 0 an index in rr, or transparent. The
    game's list synced before the frame hook returns. Kept alike. The loan's address now
    stands until the frame is presented (it could move on a vertical blank), and the console
    draws the frame into another of the screen's buffers, waiting a vertical blank when both
    others are on screen or waiting. A picture that stands is the same address handed again,
    nothing drawn over it. Paused, it is let go, its buffer wanted for the pause's overlay,
    so a picture is drawn and handed after the pause. A frame showing it has no
    `nshvc_picture`, as any canvas's there (2.21).
  - `nshvc_ext_canvas_chars_gpu(layer, word_addr, count, handle, per_row, k, fence)` and, the
    PSP's, `nshvc_ext_canvas_chars_ge(layer, word_addr, count, vram_address, stride, per_column, psm, k)`: a
    canvas of characters from the GPU in place of `nshvc_ext_canvas_chars`'s texels, the
    console placing them as ever (the layer's map and scroll, each tile's flips, priority and
    palette). The texture: 8-bit unsigned texels (`R8Uint`), the characters in a grid, `per_row`
    of them a row, each `8k x 8k`, copied on the GPU, needing `TRANSFER_SRC` on Vulkan. VRAM,
    read straight: by column, `per_column` characters a column, the columns past what a row holds
    (at most 512 texels) going on in a band below. The GE draws no 8-bit target, so the value is
    the low bits of a pixel it drew: `GU_PSM_8888`'s red, any depth; `GU_PSM_4444`'s red, a layer
    of 4 bits or fewer, for more room (at a row of 512, 8888 holds 512 x 272 texels, 4444 512 x
    512). Past the loan's buffer or 512 texels down is fatal. Handed, kept and let go as the
    screen forms.
  - `nshvc_ext_overlay_gpu(handle, w, h, fence)`: an overlay from a texture (8 bits a
    channel, unsigned normalized, straight alpha, sampled), or on the PSP from VRAM (`handle`'s
    image its address, its view its stride; `GU_PSM_8888`, its alpha the stencil, the column
    and row past w x h read by the filter being the game's), in place of an overlay's CPU
    texels. It returns the overlay's id for `nshvc_overlay_place`, the same each call (one
    such overlay; -1 when `NSHVC_OVERLAYS` are made). Kept as the canvas's, and shown while
    paused.
  - `nshvc_ext_ge_vram2(&bytes, &depth, &depth_stride)`: the loan of `nshvc_ext_ge_vram` with
    a 16-bit depth buffer of the screen's size (the PSP: 512 pixels a row, 272 rows, 278,528
    bytes, 8 KB aligned), its contents not kept; `depth` NULL while paused, where the
    backdrop is. Set by the GE's commands (ZBP, ZBW), the graphics library's kept state left
    as found.
- **2.21:** every picture is the platform's GPU's. `nshvc_picture` is drawn by the GPU and
  read back, and fatal on the PSP for a frame showing a canvas, which is drawn at the
  target's resolution alone. A canvas past what the platform's GPU spans is fatal, and
  `nshvc_ext_canvas_scale` is capped to it (the PSP: 512 x 512 texels, a whole k). The CPU's
  drawing is the checks' alone (`tests/ref`): the library's `soft` or `none` renderer has no
  picture, and `NSHVC_COMPOSE` is gone. A loop that waits for the console's time with its
  accesses taking none is ended by a watchdog, not a count of 20,000,000 reads: with the costs
  off an inline access is a load or a store and no more. An app bringing its own host
  (`NSHVC_HOST=none`) supplies `host_every`.
- **2.20:** each platform draws every picture on its own GPU, within the platform's limits:
  `nshvc_ext_screen` past a platform's screen is a fatal error naming it (the PSP: 512 x 240,
  a hires or interlaced frame 256 across, and an interlaced hires frame (512 x 448) fatal there;
  elsewhere the contract's 8192 x 8192). Overlays are
  drawn by the platform's own filtering and blend (the PSP's GE: bilinear in sixteenths of a
  texel).
- **2.19:** `nshvc_set_register_write_hook`: each S-CPU write to the console's own registers
  (the B bus and the CPU's, $2100-$21FF and $4000-$43FF of banks $00-$3F and $80-$BF), for an
  app that needs only those; unlike a write hook, it leaves the S-CPU's other accesses inline.
  The presentation's sharp bilinear takes its weights in whole 256ths (each channel a +
  floor(((b - a) W + 128) / 256)) on every host, so a GPU with no shaders (the PSP's GE) makes
  it exactly; before, in floats.
- **2.18:** a cartridge's chip may give the S-CPU its memory directly (`cart.h`
  `direct_read`, `direct_write`, `nshvc_cart_direct_changed`): the 8 KB ranges where the
  S-CPU's accesses are plain memory while the chip says so, made inline like work RAM's.
  The sound driver's C takes no time with the costs off, as the S-CPU's does, and its waits
  (`spc_wait_until`) look again where what they read can next change, not a cycle at a
  time; a write to the DSP after KON or KOFF waits to the clock the DSP takes it.
- **2.17:** nothing in the console runs unless something looks at it. The beam's line and
  dot, HVBJOY, the counters and the refresh are worked out from the clock where they are
  read; the console stops only at its deadlines (an interrupt, the NMI's flag, the
  vertical blank's and the frame's start, HDMA while it runs). A wait
  (`nshvc_wait_until`) looks again where what its condition read can next change, not
  every few of the S-CPU's cycles: what it sees, and the clock it sees it at, are the
  same as before for a status bit, a counter, an interrupt's flag and memory; a port the
  sound unit writes and a chip's register are still seen within 64 clocks. The
  cartridge's chip is brought up to the console's clock where the S-CPU or a DMA looks
  at it, where its IRQ could be taken and once a frame (`cart.h`), no longer once a line;
  the sound unit at its ports and once a frame. With `NSHVC_BUS_INLINE` (the library's
  target sets it), `rd8`, `wr8`, `rd16` and `wr16` to work RAM, and ROM a chip does not
  answer, are made inline (`machine.h`).
- **2.16:** the frame is presented over the whole window on every host, each axis at its
  own scale, sharp bilinear (and Mode 7 and a canvas at the window's resolution as
  before), with no bars: a game that wants a shape sizes its screen with
  `nshvc_ext_screen`. Before, the frame kept a television's 4:3 for the console's
  256 x 224 (a wider screen the same shape of pixel), centred. `nshvc_ext_canvas_presented`
  gives the window's size.
- **2.15:** a screen canvas at no whole scale: `nshvc_ext_canvas_screen` and
  `nshvc_ext_canvas_screen32` with k 0 take w x h texels (each up to 65,535) over the
  whole W x H screen: a point x console pixels across it (y down) takes texel
  floor(x w / W) (floor(y h / H)), a console pixel's centre floor((2c + 1) w / 2W), in the
  console's picture, the compositor and every renderer's presentation alike; k 1 to 64 as
  before. `nshvc_ext_canvas_presented(&w, &h)`: the window pixels the console's screen was
  last presented over, the size such a canvas is presented at a texel a window pixel.
  `nshvc_ext_ge_vram(&bytes)`: GPU memory lent to the game's own GPU drawing until the
  frame is next presented, through its run of the frame and the frame hook (the PSP:
  VRAM, one screen buffer, 557,056 bytes; NULL elsewhere), its work on it done by then;
  the GPU's state put back after, the graphics library's kept buffers left as found.
- **2.14:** `nshvc_ext_canvas_screen32(layer, texels, w, h, k, priority, palette)`:
  `nshvc_ext_canvas_screen` with texels of 32 bits: 0 transparent; `0x000000ii` (ii not 0)
  CGRAM index ii, resolved as that canvas's texel ii (each line's palette, so HDMA's
  CGRAM writes show; `palette` and the layer's colour depth as there); a top byte not 0,
  the colour `0xrrggbb` (`0xFFrrggbb` as written), opaque. The rest is
  `nshvc_ext_canvas_screen`'s: k, the window's resolution, the texels copied and a frame
  showing the canvas as it stands when the frame begins, one canvas at a time (this,
  `nshvc_ext_canvas_screen` or `nshvc_ext_canvas_chars`, the last asked for). A colour
  goes the layer's way on every renderer, in the console's picture and the presentation
  alike: the layer's priority, the windows (`nshvc_ext_window_columns`, `_window_map` and
  `_window_edges` included), colour maths with the other screen or the fixed colour (a
  per-line COLDATA included), brightness. Its precision, each channel v counted as
  z = v - v / 32 eighths of a console step (0 to 248; a 5-bit c widened, c << 3 | c >> 2,
  is exactly 8c, so a CGRAM colour widened gives exactly that CGRAM colour's pixel on
  every path), and a 15-bit colour (the other screen's, the fixed colour) as 8 times its
  5 bits:
  - neither colour maths nor brightness takes the pixel: v itself, all 8 bits;
  - colour maths adding or subtracting: z plus or minus the other's, clamped to 0-248,
    kept in eighths (249 levels a channel);
  - colour maths halving: the step's whole part, z / 16 * 8, the console's 5 bits;
  - brightness below 15: the console's 5 bits too, z / 8 = q to (q * b + 7) / 15.
  A result z shows as z + z / 32 (8c: c << 3 | c >> 2). Off until asked for.
- **2.13:** `nshvc_ext_pause(on, blur, tint)`: the game's pause; the machine stops between
  two frames and its sound goes silent, the window shows the last frame presented blurred by
  `blur` points and tinted, under the game's overlays, which with input and the hooks go on;
  off, the machine goes on as though it had not stopped. `nshvc_ext_paused()`.
  `nshvc_hooks.pointer(x, y, buttons)`: the mouse in the window's points, for the game's
  own controls; 1 takes it from the console's mouse and guns.
  `nshvc_ext_key_for(control)`, `nshvc_ext_key_bind(control, vk)`, `nshvc_ext_key_name(vk)`:
  player 0's keyboard keys, laid out by the game.
  `nshvc_overlay_update(id, argb)`: an overlay's image drawn again.
- **2.12:** `nshvc_ext_window_map(on, scale_q16, offset)`: on a larger screen, each line's
  window edges (WH0-WH3, HDMA's included) mapped onto the screen, console column c to
  c * scale_q16 / 65536, rounded down, + offset: a window from L to R covers L's column up
  to the one before R + 1's, for every layer, the sprites and the colour window, with the
  console's masks, logic and inversion; (65536, (W - 256) / 2) is the console's windows and
  (W * 256, 0) stretches them across the screen; it overrides `nshvc_ext_window_edges`
  while on.
  `nshvc_ext_window_columns(window, left, right)`: window 1 or 2 from screen column left
  to right (inclusive) on every line, in place of its registers, the map and the edges;
  left past right covers none, and left -1 gives the window back to its registers. Both
  from the next line drawn, off by default.
- **2.11:** `nshvc_ext_window_edges(on)`: on a larger screen, a window whose edge is at the
  console's own (left 0, right 255) reaches every column past it, so a window spanning the
  console's line spans the screen; off (the default) a column past the console's edges is
  outside both windows, as before. `nshvc_ext_text_input(on)`: on a touch screen with no
  hardware keyboard, the system's on-screen keyboard while on, what is typed reaching the
  key hook as virtual keys (letters, digits, space, Backspace, Return), each a press then
  a release; nothing elsewhere. `nshvc_keyboard_attached()`: whether the player has a
  hardware keyboard. The host interface (not the contract) adds `host_text_input` and
  `host_keyboard_attached`.
- **2.10:** `nshvc_ext_layer_outside` takes maps 128 tiles wide: size 4 (128 x 32) and
  5 (128 x 64), rows of 128 entries, so a layer whose scroll wraps at 1,024 pixels wraps
  there outside the console's picture too.
- **2.9:** `nshvc_ext_sprite_wrap(on)`: Y page 0's sprites shown again every 256 rows
  above the console's picture on a taller screen (on, the default) or not at all above
  line 1 (off); in the console's own 256 rows they wrap as the console's either way.
- **2.8:** `nshvc_ext_layer_outside(layer, map, size)`: a background's own map past the
  console's 256 columns and lines on a larger screen (entries in BGnSC's layout, copied,
  from the next frame); the layer's characters, scroll and the rest are its own.
  `nshvc_ext_canvas_screen(layer, texels, w, h, k, priority, palette)`: a background's
  picture over the whole screen from the game's texels, by the screen's pixels (no map or
  characters), composed by the console's rules with one priority and palette, presented
  at the window's resolution; one canvas at a time, this or `nshvc_ext_canvas_chars`.
  Both off until asked for.
- **2.7:** `nshvc_ext_screen` to 8192 x 8192 (from 1024 x 480), and
  `nshvc_ext_screen_origin` across it (column to 7936, row to 7968).
  `nshvc_ext_sprite_page(sprite, x_page, y_page)`: a sprite 512 columns a page (-16 to
  16) and 256 rows a page (-32 to 32) from its table's place, without wrapping off Y
  page 0, so sprites reach the whole of the largest screen wherever the console's
  picture sits; `nshvc_ext_sprite_x` and `_y` set a page to 1, as before. Off until
  asked for; memory and a frame's drawing grow with the screen asked for.
  `nshvc_ext_layer_clip(layer, on)`: a background or the sprites kept to the console's
  picture on a larger screen, transparent past its 256 columns and its lines (a sprite
  across the edge cut at it), from the next line drawn. Off until asked for.
- **2.6:** `nshvc_ext_screen_origin(x, y)`: where on a larger screen the console's
  picture sits, the screen's column of its column 0 and row of its line 1 (-1 the centre,
  as without the call), held at the screen's right and bottom edges. The rest of the
  screen is drawn as `nshvc_ext_screen` defines, sprites from the picture's column 0 and
  line 1. Off until asked for.
- **2.5:** `nshvc_ext_canvas_chars` across VRAM pages: a word_addr from `$8000` is in
  VRAM's whole space (`page << 15 | word`, page 1 on), and the canvas covers the
  characters a layer reads from any of its pages there, so one canvas covers a layer whose
  lines read more than one page (a map names up to 1,024 characters). A word_addr below
  `$8000` is in the layer's page, as in 2.4. Off until asked for.
- **2.4:** `nshvc_ext_canvas_chars(layer, word_addr, count, texels, k)` and
  `nshvc_ext_canvas_scale()`: a canvas, a background's characters (those whose data starts
  at word_addr, count of them) drawn from the game's own texels at the window's resolution,
  placed by the layer's own rules. Off until asked for; the console's own memory and timing
  are untouched.
- **2.3:** `nshvc_ext_vram_copy(to_page, from_page)` and `nshvc_ext_vram_direct(page)`: VRAM
  pages 1-3 written outside the console's bus and time, at any moment, a whole page copied
  or the data port's writes (the S-CPU's or a DMA's) landing in a page at once, a DMA of
  only those taking no time; page 0 only through the bus, as before. Off until asked for.
- **2.2:** `nshvc_set_cpu_costs(on)`: the S-CPU's costs on (the default at power on: each
  access the bus's time and its kind's weight from the cost profile) or off (its accesses
  take no console time; the clock still moves in its waits and while a DMA runs, and a loop
  polling the hardware with no wait is fatal, as with interrupts held), from the next
  access, so a game switches between its Original and Enhanced modes as it runs. The
  SPC700's costs and a chip's own time are not affected.
- **2.1:** each S-CPU access costs the bus's time for it, as the console's memory speeds
  give it, and the weight of its kind from the cartridge's cost profile: `nshvc_cart`'s
  `cost_profile` field (a zeroed description has none: the bus's time alone),
  `nshvc_cost_profile`, `nshvc_chip_program`, `NSHVC_KIND_*` and `nshvc_access_kind`
  (`nshvc.h`, docs/cost-profile.md, measured by `tools/nshvc-profile`). A loop that waits
  for the hardware with no wait sees the clock move, and is no longer fatal (but with
  interrupts held). `nshvc_set_read_hook`: each S-CPU read as it is made, with its value.
  A word's or a long's bytes go out in consecutive bus cycles, their weights spent before
  the first. The SPC700's boot ROM keeps its own instruction time, as the console's firmware does,
  and each access the sound driver's C makes costs its one bus cycle and its kind's weight
  from the profile (`spc_tenths`), so an upload sent as a word to `$2140` is taken as on the
  console; a sound driver's loop with no wait sees the clock move, and is no longer fatal.
- **2.0:** behaviour exact, not cycle exact. The S-CPU's and the SPC700's C state no time:
  `cyc`, `cycs`, `cyc_then`, `cyc_loop`, `cycs_then`, `cycs_loop`, `cpu_idle`, `spc_cyc` and
  `spc_wr_cyc` are gone, and `spc_call` and `spc_ret` take no cycles. Their waits are
  `nshvc_wait_until` and `spc_wait_until`; `nshvc_wait_vblank` moves to `machine.h`.
  `nshvc_set_untimed` and `nshvc_untimed` are gone (the processors' work is untimed always),
  and with them the interrupt latency exception of 1.x. `tooling.h` (`nshvc_mark`) and the
  environment options `NSHVC_AUDIT` and `NSHVC_CYCLOG` are gone with the stated time they
  read.
- **1.1:** `nshvc_cart_irq_at` (`cart.h`): a chip changes the cartridge's IRQ line at its
  own clock, and the S-CPU sees a rise from that clock, as on the console. A chip that
  calls `nshvc_cart_irq` builds and runs as before, its line changed at the clock its sync
  was brought to.

## Not done, outside the contract

These change where nativeshvc runs, not what a game writes:

- **Windows:** cross-built with no warnings, but never run on Windows.
- **Linux:** has no windowed host. The headless host has not been built or run on Linux.
