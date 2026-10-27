# The game contract, version 1.0

This is what a decompiled game writes against and what nativeshvc promises in return. It is
fixed at 1.0. A game written to it plays on every later 1.x without a change.

## How it changes

- **Major version (2.0):** the only kind of release that may change or remove anything a
  game writes or relies on below.
- **Minor version (1.1):** may only add things: a new call, a new extension, or a new field
  at the end of a struct that a zeroed struct leaves off. A 1.0 game builds and plays the
  same.
- **Patch version (1.0.1):** brings the console closer to the hardware where it differed
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

- [`tooling.h`](../include/nshvc/tooling.h) (`nshvc_mark`, `spc_at`). A game plays the same
  without them.
- The headless script's steps (`src/front/script.h`).
- The environment options (`NSHVC_AUDIT`, `NSHVC_CYCLOG`, `NSHVC_STATES`,
  `NSHVC_SCRIPT_NOFORK`, `NSHVC_DEBUG`).
- The host interface for `NSHVC_HOST=none` (`src/host/host.h`).
- Everything under `src/`.

## What the game does: the S-CPU

1. **Every bus access the original makes, in its order.** These go through `rd8` and
   `wr8`. A word or a long is one operation: `rd16`, `wr16` and `rd24`, low byte first. A
   read-modify-write instruction is `rd8_rmw` then `wr8`, or `rd16_rmw` then `wr16_rmw`
   (high byte written first). Dummy reads and the stack's pushes and pulls are accesses
   like any other.
2. **The time between accesses.** `cyc(clocks)` gives the master clocks the original spends
   between two accesses, besides the accesses' own: its fetches and internal cycles. Where
   the statement can't stand on its own (inside an expression, or at a loop's test),
   `cyc_then` and `cyc_loop` do the same.
3. **Cycle by cycle where a DMA or HDMA starts.** If a DMA (from a `$420B` write) or an HDMA
   (each shown line's horizontal blank, while HDMA is on) starts inside a stretch of time,
   that stretch is given as `cycs` instead of `cyc`: one hex digit per cycle (6, 8 or C).
   The console then starts the transfer on the right cycle edge. The S-CPU's time is exact
   only when this holds. `NSHVC_AUDIT=1` names every `cyc` stretch on a run where a
   transfer started inside one.
4. **Time with no access of the C's.** For example, a delay loop, given as
   `cpu_idle(clocks)`.
5. **The register file at a routine's entry and exit.** A routine entered from an address
   (an interrupt vector, a computed call or jump, a chip's program) has an `abi_` wrapper
   that takes its inputs from `cpu` and leaves its outputs there. `cpu.unwound` says when
   C leaves through its caller's frame.
6. **The routine table** (`nshvc_cart.routines`, sorted by address). It lists every
   routine an address reaches: the reset and interrupt vectors' code (a stub in RAM
   included), every computed call's and jump's target, and any routine a chip runs by
   address. Reaching an address with no C is a fatal error; nativeshvc runs no machine
   code.
7. **Control the original's machine code has and C doesn't:**
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
2. **The SPC700 cycles before each access**, besides the access's own: `spc_cyc(n)`, or
   `spc_wr_cyc` where the write's value is an expression.
3. **Calls and returns as the stack sees them** (`spc_call`, `spc_ret`), with their
   cycles. A routine that calls another gives its own CALL's cycles.
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

- **Everything but the processors and the cartridge's own hardware, as the hardware
  does.** This means:
  - the bus and its timing, work RAM, the mappings;
  - the CPU's registers (NMI, H/V IRQ, multiplier, divider, the joypads' automatic read);
  - DMA and HDMA, the PPU, the sound unit and DSP;
  - every controller in `NSHVC_PORT_*`.

  The reference is bsnes with its accuracy options. Where nativeshvc differs from it,
  that's a defect in nativeshvc, fixed in a patch release.
- **Interrupts in the console's time.** An interrupt comes due at the clock the hardware
  raises it. It is taken at the first point between two of the C's operations at or
  after that clock: the end of an access, a word, or a read-modify-write's write, or that
  clock itself inside `cyc`, `cycs` or `cpu_idle`. No operation is split. The IRQ waits
  while `cpu`'s I flag masks it. WAI, a branch to itself and the console's own code take
  interrupts exactly as the CPU does.
- **The one exception to cycle exactness: interrupt latency.** The 65816 starts a handler
  at the end of the instruction in progress. Structured C doesn't carry that instruction,
  so here the handler can start earlier by the rest of it. On the test cartridges that
  measure it, that's 1 to 20 dots (README.md, Time; `tests/rom/KNOWN`). What the handler
  reads and writes is the hardware's; only when it starts differs. Where the code waits
  for the interrupt (WAI, or polling a flag or status register), the timing is exact.
- **A fatal error, never a guess,** when the game reaches something the contract doesn't
  cover: an address with no C (a vector's included), a cycle length other than 6, 8 or 12, or a wait on time while untimed. It's reported,
  then the process exits with status 70.
- **The console's numbers by default.** Every extension (`nshvc_ext_*`,
  `nshvc_set_untimed`, `nshvc_hold_interrupts`) is off until the game asks for it. With
  all of them off, the console is exact.
- **The same run on every host.** One thread, a deterministic schedule: the same input
  gives the same machine.

## Not done, outside the contract

These change where nativeshvc runs, not what a game writes:

- **Windows:** cross-built with no warnings, but never run on Windows.
- **Linux:** has no windowed host. The headless host has not been built or run on Linux.
