# The cost profile

The standard by which a decompiled game measures what its processors' work costs on
nativeshvc. nativeshvc provides it: this procedure, the tool that carries it out
(`tools/nshvc-profile`), the file it writes, and the field a cartridge declares it in. A game
runs it on its own ROM and declares the result. nativeshvc runs nobody's measurement, and no
person sets a number: anyone can give the tool a ROM and get the same numbers back.

## What it is for

nativeshvc is behaviour exact, not cycle exact (README.md, Time). A processor's C makes the
original's data accesses, but not its instruction fetches or internal cycles. The console
charges each access the bus's own time for it, as the hardware's memory speeds give it. A
game whose pace is how much work fits in a frame (slowdown when much happens at once) has
two modes (README.md, The preservation standard; Time, Pacing), both behaviour correct and
both respecting the original game:

1. **Original mode: on its work, approximately, by the cost profile.** The game keeps the original's pacing,
   and the profile gives the CPU work around each access, which the console cannot see: a
   **weight** for each kind of access, measured on the original, charged on top of the
   bus's time. A frame where more happens makes more accesses and costs more time, so the
   game slows where the original slowed. The decomp's C does the original's work with
   other accesses than the original's (fewer reads, values kept in variables, the console's
   waits where the original polled), and in some code more or fewer than in other code, so
   no set of weights gives each part of its work the original's time: the profile is an
   approximation, and its report gives by how much.
2. **Enhanced mode: independent of processor speed, exactly.** The game's code is updated
   as a properly built modern game's is: its pace is set by the time that really passes and
   the display, not by how much work fits in a frame (60 Hz, or a frame rate left
   unlocked). It behaves the same at any S-CPU cost, zero included, and runs with the
   S-CPU's costs off (`nshvc_set_cpu_costs(0)`: its accesses take no console time). The SPC700 keeps its weights: the sound
   driver runs its own program at its own speed. A cartridge chip is the cartridge's own,
   so the game makes its jobs independent of the chip's speed itself.
   Where the original slowed down, it does not: that slowdown was a side effect of the
   chips' speed, not something the game was written to do.

**Who needs one.** A game's Original mode, where its pace is how much work fits in a frame.

**With none declared, the cost is 0:** each access takes the bus's time alone.

**Scope.** The profile covers the S-CPU, the sound CPU (SPC700) and a cartridge's chips.
The sound driver keeps its beat by its own timers, which the console runs at their real
time; its weights give it the real chip's time around each access, so it answers the
S-CPU's handshakes as the console does (an upload's byte read a few cycles after its index,
past a refresh that falls between the S-CPU's two writes).

## 1. The profile

### The S-CPU

Eight access kinds (`nshvc.h`, `NSHVC_KIND_*`, `nshvc_access_kind`): a read or a write of
work RAM, ROM, cartridge RAM or I/O, by address, the same in every mapping:

| kind | addresses |
|---|---|
| cartridge RAM | banks `$70-$7D` and `$F0-$FF` at `$0000-$7FFF`; banks `$20-$3F` and `$A0-$BF` at `$6000-$7FFF` |
| work RAM | banks `$7E-$7F`; `$0000-$1FFF` in banks `$00-$3F` and `$80-$BF` |
| I/O | `$2000-$5FFF` in banks `$00-$3F` and `$80-$BF` |
| ROM | the rest |

Each kind has a weight in tenths of a master clock, charged before each access of that kind
the game's C makes, besides the bus's time. The tenths carry from one access to the next. A
word or a long (`rd16`, `wr16`, `rd24`, a read-modify-write's two reads or two writes) is one
instruction's: its bytes' weights are all spent before its first byte, and its bytes go out
in consecutive bus cycles, as the 65816 makes them. The
console's interrupt sequence (its pushes, RTI's pulls) is charged the bus's time only.

### The SPC700

Four access kinds: a read or a write of its RAM, or of its registers (`$F0-$FF`). Each has a
weight in tenths of an SPC700 cycle, charged before each access of that kind the driver's C
makes, besides the access's own cycle (which the console always charges). The tenths carry
from one access to the next. The boot ROM's work is the console's, with its own time.

### A cartridge's chip

A chip with a processor is priced the same way, by the chip. The chip names the operation
kinds it sees, as an enum (a Super FX's might be its loads, stores, cache flushes and pixel
reads: its accesses to the cartridge's RAM by what made them). The profile gives one row for
each program the chip runs, keyed by the program's entry (the Super FX: PBR:R15 at GO):

    duration (chip clocks) = fixed + Σ weight[k] × count[k]

`fixed` is in whole chip clocks, the weights non-negative in 1/4096 of a chip clock, one an op
kind in the chip's enum order; the rows are sorted by entry (the chip searches them), with
one default row for a program not listed. The chip turns chip clocks into master clocks at
its own crystal (the Super FX: 21.44 MHz). nativeshvc implements no chip: it carries the
rows to the chip in the cartridge's description (`cart.h` `power`), and the chip prices its
own jobs.

### Declaring it

A cartridge declares its profile once, in its description: `nshvc_cart.cost_profile`, read
at power on. With none, the S-CPU's cost is 0 and a chip uses what it has of its own. As it
runs, the game turns the S-CPU's costs off and on with `nshvc_set_cpu_costs` (on at power
on): off in its Enhanced mode, where its accesses take no console time; on in its Original
mode and in any scene that must keep the original's pacing.

### The file

The tool writes the profile as a versioned text file, and the C table a cartridge declares
(`profile.c`, `const nshvc_cost_profile NAME_cost_profile`):

    nshvc-cost-profile 1
    tool nshvc-profile 1.0
    reference bsnes <the reference core>
    rom-sha256 <the ROM measured>             (a line each ROM)
    source-sha256 <the decomp's source, when step 2 ran with --source; else ->
    checked <yes: step 2 ran | unchecked>
    counts <kind> <decomp ÷ original> ...     (step 2's ratios, when it ran)
    cpu read  wram <w> rom <w> cart-ram <w> io <w>
    cpu write wram <w> rom <w> cart-ram <w> io <w>
    spc read  ram <w> reg <w>
    spc write ram <w> reg <w>
    chip <name> ops <KIND> ... speed <clocks> (named, so a chip whose enum differs is caught)
    program <entry> fixed <f> weights <w> ...
    default fixed <f> weights <w> ...

`nshvc-profile check PROFILE --source DIR` warns (exit 1) when the decomp's source no longer
has the hash the profile was checked against: the profile is stale. A game's build runs it.

## 2. The procedure: `tools/nshvc-profile measure`

    nshvc-profile measure ROM [ROM...] --out DIR [--frames N] [--span PLAY]...
                          [--chip-jobs FILE] [--decomp "BUILD ARGS"] [--source DIR] [--name NAME]

It needs the reference core (`NSHVC_ORACLE_CORE`, tests/README.md).

### Step 1: ROM in, profile out (always)

The ROM runs on the reference (bsnes, accuracy settings, `tests/oracle --cpustats`) with no
input for 3,600 frames, its attract mode, and on each PLAY given (an input-only script in
nativeshvc's script language) as an extra span. On each frame the reference records:

- **the CPU's real work**: the bus time of its data accesses plus its instruction fetches and
  internal cycles. It leaves out WAI, DMA, refresh, and poll loops (a stretch of more than 64
  reads with no write), which the console's waits stand for, and any single gap over 1,024
  clocks (a branch to itself).
- **its data accesses by kind**, the eight above, each with its own gap: the CPU's work
  since the access before (its fetches and internal cycles).
- **the SPC700's**, the same: its data accesses by its four kinds, each with its gap in its
  cycles (fetches, dummy reads, internal cycles), its poll loops left out the same way.

The fit is fixed, with nothing to tune, and each processor's is the one its profile is for.
Both are taken over the frames whose processor worked 5% of the frame or more.

- **The S-CPU's sets the game's pace**: how much work fits in a frame. Its weights are
  non-negative least squares on each frame's error in clocks: they make each frame's total
  come out as near the real one as they can (a frame's time is the sum of its costs, so the
  slowest frames, where slowdown shows, weigh most). The answer is one answer: the tool
  searches every set of non-zero weights exactly, and where the data leave more than one
  answer with the least error (a count that never changes, two that move together), it takes
  the one with the fewest non-zero weights, and among those the first in order (the kinds'
  order; for a chip, `fixed` first, then the ops in theirs). So work that is the same every
  time goes to the first: a chip's fixed part.
- **The SPC700's sets when each access is made**: the driver answers the S-CPU's handshakes
  access by access. Each access's gap is measured, so a kind's weight is least squares on its
  own accesses' gaps, which is its mean gap: its own, never traded against another kind's
  that comes with it (as frame totals would trade a driver's register reads against its RAM
  reads).

A kind with fewer than 1,000 accesses is not measured, and its weight is 0. More than one ROM
gives one profile over all of them. The report gives each processor's error frame by frame,
relative to each frame's real work.

**A chip's programs** come from the chip's jobs as the game recorded them on the reference
(`--chip-jobs`): the game owns its chip and its recorder, and hands the tool its jobs in the
standard's form:

    nshvc-chip-jobs 1 NAME ops KIND KIND ... speed CLOCKS
    job ENTRY CLOCKS COUNT COUNT ...

ENTRY the program's entry (hex), CLOCKS the job's own time in chip clocks (the waits the chip
makes for the S-CPU taken out: the Super FX's RON and RAN holds), a COUNT for each op kind
in the order named; `speed` the chip clocks an access of its takes (the Super FX: 5 at
CLSR's 21 MHz, 6 at 10.7). Each program with 20 jobs or more is fitted on its own (fixed and
weights, non-negative least squares on each job's error in chip clocks, so the long jobs
weigh most; one answer: every set of non-zero weights is searched exactly, and where the
jobs leave more than one answer with the least error, a count that never changes or two
that move together, the one with the fewest non-zero weights, then the first in order,
`fixed` first, so work the same every job goes to `fixed`); one with fewer is one
multiplier on the speed, as is the default row, over all the jobs. Work the recording does
not count as operations (the Super FX's ROM-buffer fills) is carried by the fixed part.

### Step 2: the decomp's consistency (when a decomp build exists)

With `--decomp`, the game's build runs the same spans under nativeshvc, headless, in its
measuring mode (`--counts FILE`: each frame's accesses by kind, with the same rule for poll
loops). The profile applies as measured only if the decomp makes the original's data
accesses, so the report gives each kind's total over the run as a ratio (decomp ÷ original)
and lists the frames whose counts differ. The profile records that step 2 ran, its ratios,
and with `--source` the decomp source's hash; without step 2 it says `unchecked`. A chip's
consistency is the game's own run of its jobs against its recording.

## 3. The report (`report.txt`, fixed format)

- the ROMs and spans, the frames, and those with work;
- the S-CPU and the SPC700: each kind's weight and its accesses (or "too few to measure:
  0");
- the profile's error against the original, frame by frame: bias, mean |error|, 95th
  percentile, worst, and the slowest 5% of frames separately (bias, worst), where slowdown
  shows; beside it, the same with no profile (the bus's time alone);
- each chip program: its jobs, its share of the chip's time, how it was priced (fit or
  multiplier), its model, and its error (bias, mean |error|, p95, worst, mean on the 5%
  longest jobs); a POOR line for each program whose mean is over 10% or p95 over 25%, with
  its share, never hidden;
- step 2: each kind's count ratio and the frames whose counts differ, or `unchecked`;
- **acceptance bar: not set** (an Original mode's profile is an approximation: README.md,
  Time, Pacing); the tool writes `TBD`.

## 4. In the contract (2.1, 2.2)

`nshvc.h`: `nshvc_cost_profile` (its `spc_tenths` the SPC700's), `nshvc_chip_program`, `NSHVC_KIND_*`, `nshvc_access_kind`,
the field `nshvc_cart.cost_profile`, `nshvc_set_cpu_costs` (2.2: the S-CPU's costs on or
off as the game runs), and `nshvc_set_read_hook` (each S-CPU read as it is made,
with its value: what step 2's measuring mode counts by, and any app may use).
