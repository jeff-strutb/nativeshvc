# The game contract, version 2.1

This is what a decompiled game writes against and what nativeshvc promises in return.
nativeshvc is a behaviour-exact implementation of the console, not an emulator. The
contract is at 2.1: the console is behaviour exact, the game's C states no time, and each
access it makes costs what the console's bus takes for it, and its cost profile's weight
(Changes, below). A
game written to it plays on every later 2.x without a change.

## How it changes

- **Major version (3.0):** the only kind of release that may change or remove anything a
  game writes or relies on below.
- **Minor version (2.1):** may only add things: a new call, a new extension, or a new field
  at the end of a struct that a zeroed struct leaves off. A 2.0 game builds and plays the
  same.
- **Patch version (2.0.1):** brings the console closer to the hardware where it differed
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
  are done when read. Each access the SPC700's C makes costs its one bus cycle and the
  weight of its kind from the profile (0 with none); its clock also moves in its waits.
- **Pacing, in two modes.** A game whose pace is how much work fits in a frame has both
  (README.md, The preservation standard; Time, Pacing): an Original mode, paced on its work
  by a cost profile, which approximates the original's processor time, so its slowdown is
  approximate; and an Enhanced mode, made independent of processor speed (its pace set by
  the time that really passes and the display, as a properly built modern game's is, not
  by how much work fits in a frame), which nativeshvc runs exactly at any S-CPU and SPC700
  cost (a cartridge chip's jobs still take their own time, the hardware's). Both are
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
  with interrupts held (where no time passes), a loop that waits for the hardware
  (20,000,000 reads with no write between). It's reported, then the process exits with status 70.
- **The console's numbers by default.** Every extension (`nshvc_ext_*`,
  `nshvc_hold_interrupts`) is off until the game asks for it, and with no cost profile an
  access costs the bus's time alone.
- **The same run on every host.** One thread, a deterministic schedule: the same input
  gives the same machine.

## Changes

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
