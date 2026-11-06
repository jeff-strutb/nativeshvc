# The lazy console

Nothing in the console runs unless something observes it. Every piece of hardware is a
function of the clock, brought up to date at an **observation**: an access that reads or
changes it, or a **deadline**, where the hardware itself must act on the S-CPU at a clock.
Cost scales with what the game does, not with console time.

Deadlines are only: NMI due; IRQ due (the H/V timer enabled, or the cartridge's line); an
HDMA line (HDMAEN not 0); a DMA started; the vertical blank's first line; the frame's start.
Game C takes no console time with the costs off; Original mode's cost profile charges its
time in bulk (docs/cost-profile.md). A wait ends at the clock its condition can first change.

## The rule

No code steps through console time. No loop over clocks, dots, lines, sub-sample clocks,
timer edges or another processor's cycles exists unless each step makes something observed:
an audio sample the host plays, a frame-record line, an HDMA transfer the game turned on, or
a handler the game's interrupt runs. Everything else is a formula of the clock, evaluated
when read.

## The audit: every loop in `src/console`, `src/ppu` and `src/dsp`

Each loop with what it produces for an observer. Loops over fixed tables and small fixed sets
(not over time) are grouped first; the loops indexed by console time follow, one by one.

### Not over time

| where | loop | observed output |
|---|---|---|
| `apu.c` `snes_audio_take` | ring entries asked for | samples the host plays |
| `apu.c` `port_new_from` | bisection over the SPC's logged writes | the port value a read sees |
| `apu.c` `spc_wr` (`n < 3`) | the three timers on a CONTROL write | timer stages the driver reads |
| `bus.c` `snes_rom_mirror` | the ROM size's bits | the address read |
| `bus.c` `allowance_set`, `acc_info_build`, `regions_place`, `regions_build`, `snes_power` | setup tables, at power on or a cost change | each access's cost, the regions read |
| `bus.c` `allowance_ahead` | a word's bytes | its accesses' weights |
| `bus.c` `nshvc_cart_direct_changed` | the chip's direct ranges | the regions read after it |
| `coro.c` `coro_new`, `coro_stack_ok` | a new stack, its canary | the coroutine run, the overflow check |
| `dispatch.c` `find`, `take_interrupt` | routine tables and their ROM checks | the C entered |
| `dispatch.c` `jump_routine_pushed` | a call's pushed bytes | stack bytes the game reads |
| `dma.c` `dma_reset`, `direct`, `dma_direct_only`, `hdma_active`, `later_done`, `hdma_init` | the 8 channels | the channels' registers, the transfer's start |
| `dma.c` `vram_run` | the bytes of one run | VRAM words read back or drawn |
| `ports.c` `device_reset`, `ports_reset`, `device_latch` | the ports and their pads | what the joypad registers return |
| `bus.c` `joy_run` | none: the edges passed come from the clock, and each device gives its bits as one word (`ports.c` `ports_bits`) | JOY1-4, when read |
| `ppu.c` `cv_fill`, `page_changed` | buffers and layers | the canvas's texels, the copies' state |
| `ppu.c` `ppu_vram_port_run` | a DMA's bytes | VRAM |
| `ppu.c` `obj_eval` | 128 sprites and their tiles | STAT77's flags, when read |
| `ppu.c` `record_line` | a line's mid-line writes | a frame-record line's segments |
| `ppu.c` `cp_decode`, `cp_sprites`, `cp_state`, `ppu_compose`, `ppu_frame_presents`, `m7_state`, `mode7_segs` | a frame's lines, parts and sprites, once a frame presented | the description the GPU draws |
| `mixer.c` `mix_init`, `mix_voice_states`, `decode`, `sample_end`, `mix_control`, `sets_passed`, `mix_before_ram_write` | 8 voices, 4 BRR samples, 2 channels, a voice's reads | a sample's voices and echo, what the driver reads |
| `mixer_regs.c` `mix_regs_reset` | the registers | what the driver reads |
| `resample.c` | the table's taps; a pull's frames | samples the host plays |

### Over console time

| where | loop | steps | observed output |
|---|---|---|---|
| `apu.c` `dsp_catch_up` | `while (cycles)`, `for (i < got)` | samples due since last observed, in chunks | audio samples; OUTX, ENVX, ENDX when read |
| `mixer.c` `mix_run` | `while (n >= 32 - rel)` | samples that end in the clocks passed | an audio sample each, worked out whole by the read clocks' tables (`at_` in `mixer.c`): no work for the clocks between. A write reaches this sample or the next by its setting's read clock. A read of ENDX, ENVX or OUTX, or a write of the sound RAM, has the voices whose reads have passed worked out at once, and the rest at the sample's end |
| `apu.c` `spc_wr` | `while (mix_key_unread())` | the DSP's next key read | the write waits for it, as the driver sees |
| `apu.c` `spc_wait_until` | `for (;;)` | the driver's wait, one look per possible change | the condition the driver waits on |
| `apu.c` `spc_main` | `for (;;)` | one entry of the driver | the driver's own C |
| `apu.c` `boot_run` | `while`, one state a read of a port | **the boot ROM's protocol as states, up to the S-CPU's time**, each at the cycle its fixed counts give and each poll passed at once to the read that can see a change. A byte, its index seen, is one step, `boot_byte`, its echo shown from its cycle. Its clear is 239 writes where something can see them (`boot_clear_unseen`), one step of 2,388 cycles where nothing can | the reply a game's upload reads; the samples the DSP makes |
| `bus.c` `run_events` | `for (;;)` | one deadline a pass | NMI, IRQ, HDMA, the field: what the game's handlers and reads see |
| `bus.c` `hdma_bulk` | `while` | an HDMA line of a wait, its transfer | the bytes moved. No wait step and no `run_events` pass a line: each line takes up its transfer directly at the same clocks, and the deadlines are worked out once when the lines are done (a transfer that moves them does so itself). Lines that move no byte and reload no table are passed in one step (`dma.c` `hdma_quiet`), so the cost follows the bytes moved. Line by line through `wait_step` near a deadline, with a running chip, a wait reading the DMA registers or memory a channel writes, or a hook on the accesses |
| `bus.c` `irq_find` | `for (;;)`, `l++` | lines until the next IRQ's time, only with the IRQ enabled | the next deadline |
| `bus.c` `refresh_take` | `while` | a refresh passed, one for each line of settled work | the S-CPU's clock. Costs on only (Original mode's time); with the costs off no work is charged, and a wait sets the next refresh directly |
| `bus.c` `nshvc_wait_until` | `for (;;)` and its `while` | a look per possible change of what the condition read | the condition the game waits on |
| `bus.c` `snes_wai`, `snes_loop_forever` | `for (;;)` | one interrupt taken or deadline reached | the game's handlers |
| `dispatch.c` `cpu_main` | `for (;;)` | one entry of the game | the game's own C |
| `dma.c` `dma_run` | channels; bytes | each byte a game's DMA moves | the bytes written; the clock read |
| `dma.c` `hdma_line` | 8 channels, on an HDMA line only | an HDMA transfer the game turned on | registers changed for that line. Original mode's lines are still deadlines while its timed C runs, as its writes interleave with the tables |
| `ppu.c` `ppu_stat_catch_up` | `r` from the last evaluated row to the beam | one row's evaluation, only when $213E is read | STAT77's range and time over flags, sticky across rows |
| `ppu.c` `ppu_render_lines` | lines first to end | one record entry a line: state and palette indices, mosaic row (lines with no change share a state) | the compositor's row table, a STAT77 read. Written line by line because the vertical mosaic's row depends on the line |

## What is not done to the note's full standard

1. The boot ROM (`apu.c` `boot_run`) answers a byte in one step, but that step still makes the
   byte's accesses one after another at their cycles, and it waits at each poll and at the read of
   the byte for the S-CPU's time. A block's start and the jump are still a state an access, and a
   byte stored to $F0-$FF two. A driver jumping back to it still clears its zero page byte by
   byte, where the DSP can see it.
2. A DSP sample is worked out whole only when nothing looks into it: a read of ENDX, ENVX or
   OUTX, or a write of the sound RAM, splits it at a voice, and a RAM write also has the reads of
   the voice in hand made in the chip's order. The DSP is still the chip's arithmetic and read
   order, a sample at a time. All eight voices are worked out each sample, silent or not.
3. HDMA's lines in a wait are made in bulk before the wait's next stop, those that move nothing
   passed at once, so the cost follows the bytes moved. With something able to see them (a chip
   running, a hook, a wait reading the DMA registers or memory a channel writes), or a deadline
   a line away, the lines stay deadlines, one at a time. Original mode's stay deadlines too
   while its timed C runs across them.

## How it is checked

`tests/unit/scaling_check.c` (tests/README.md): a frame's cost the same whatever the console's
time holds. The sound CPU's timers are not in it.

Measured at `d3c0a26`, the best of 5 batches of 200 frames, a frame's cost, **in builds with
no optimisation (no `CMAKE_BUILD_TYPE`): the ratios to idle mean something, the times do not**.
A Release build a commit or two before it took 43.0 us idle on the desktop and 3,869.8 us in
PPSSPP, with display, NMI and IRQ at 0.96-1.01 of idle; `d3c0a26` itself has not been timed
in Release:

| scene | desktop (Linux x86-64, headless) | PPSSPP, an instruction-count estimate |
|---|---|---|
| idle | 187.4 us | 24,262.0 us |
| display | 202.4 us, 1.08 of idle | 24,245.0 us, 1.00 of idle |
| NMI and joypads | 197.1 us, 1.05 | 24,307.3 us, 1.00 |
| IRQ masked | 198.9 us, 1.06 | 24,260.8 us, 1.00 |
| IRQ | 190.1 us, 1.01 | 24,276.1 us, 1.00 |
| an HDMA byte each line | 220.6 us, 0.139 us a line over the display's | 27,671.3 us, 15.322 us a line |
| a frame alone, among 100 | 203.4 us, 197.1 us | 24,224.5 us, 24,222.7 us |

The HDMA row is the game's own work, printed and not judged.

## Stated limits of the verification

PPSSPP does not model Allegrex timing: a PSP number from it is an instruction-count estimate
of the CPU's list building, not a hardware time.
