/*
 * apu.c: the sound unit beside the S-CPU: the SPC700's 64 KB of RAM, its
 * registers ($F0-$FF: control, the DSP's address and data, the four ports,
 * the three timers), the bus its C uses (spc.h), and the boot ROM's work.
 *
 * The SPC700's C runs on a coroutine of its own with its own clock
 * (1,025,280 Hz: 32,040 DSP samples a second, a real console's rate). Each
 * access the C makes costs its one bus cycle; the C's own work between
 * accesses costs nothing. The clock also moves on in the C's waits
 * (spc_wait_until) and where a DSP write waits for a key event to be taken
 * (spc.h). The S-CPU brings it up to its own time before it touches the
 * ports and once a line (apu_sync); the SPC700 hands back when it gets
 * there. The DSP runs a clock a cycle, as
 * the chip does, and makes a sample every 32, into a ring the host's audio
 * thread drains (snes_audio_take).
 *
 * At power on the SPC700 runs its boot ROM: it clears its zero page, puts
 * $AA, $BB on ports 0 and 1, and takes the blocks the S-CPU sends by the
 * ROM's protocol, then jumps where the S-CPU says. The ROM's own bytes are
 * not here: boot_run() is its protocol as states, brought up to the S-CPU's
 * time at each sync, each access at the cycle its fixed counts give and each
 * poll passed at once to the read that can see a change (the boot ROM is the
 * console's: its time is kept, as the hardware's is), so the S-CPU's upload
 * sees the same replies at the same pace. Code the S-CPU uploads and starts
 * is the game's C (the runtime's snes_spc_hooks).
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_internal.h"
#include "nshvc/spc.h"
#include "coro.h"
#include "jump.h"
#include "wide.h"

SpcRegs spc;

#define SPC_HZ 1025280u

static uint8_t aram[0x10000];
static uint8_t port_in[4], port_out[4];
/* A port's value takes its read half a cycle before the read's cycle ends
   (the others at the end): the S-CPU's last write to each port, when it
   was made (master clocks from power on), and the value before it, which a
   read whose half-cycle point comes first still sees */
static uint8_t port_prev[4];
static uint64_t port_at[4];
static uint8_t control, dsp_addr;
static uint8_t t_target[3], t_stage[3], t_count[3];
static unsigned t01_div, t2_div;
static uint64_t spc_cycles, spc_target;
static uint64_t dsp_at;               /* the cycle the DSP has run to (dsp_catch_up) */
static uint32_t spc_carry;            /* the cost profile's tenths not yet spent (access_cycle) */
static Coro *spc_co, *main_co;
void (*snes_spc_run_hook)(uint16_t addr);
static port_jmp_buf jb;
static uint16_t jump_to;
static void (*jump_entry)(void);

/* the samples: a ring of stereo frames */
#define RING 32768
static int16_t ring[RING * 2];
static atomic_uint ring_w, ring_r;

static void spc_main(void *arg);
static void boot_run(void);
static int in_ipl;                           /* the boot ROM is running (its accesses cost nothing) */
/* The SPC700 can be ahead of the S-CPU (boot_byte, apu_port_change): the cycle
   each port's last change shows from, and the port until then */
static uint64_t out_at[4];
static uint8_t out_before[4];
static int stop_at_port;                     /* apu_port_change: back at a port's change */

/* NSHVC_SPCLOG=FILE: each SPC700 write and read of $F0-$FF (its time in
   half cycles, 1 write or 0 read, the address, the value), as the oracle's
   --spclog */
static FILE *spclog;
static void spclog_put(int write, uint16_t addr, uint8_t v)
{
    uint32_t t = (uint32_t)(spc_cycles * 2);
    uint8_t rec[8] = { (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24),
                       (uint8_t)write, (uint8_t)addr, (uint8_t)(addr >> 8), v };
    fwrite(rec, 1, 8, spclog);
}


void apu_reset(void)
{
    memset(&spc, 0, sizeof spc);
    memset(aram, 0, sizeof aram);
    memset(port_in, 0, sizeof port_in);
    memset(port_out, 0, sizeof port_out);
    memset(port_prev, 0, sizeof port_prev);
    memset(port_at, 0, sizeof port_at);
    control = 0x80;
    dsp_addr = 0;
    memset(t_target, 0, sizeof t_target);
    memset(t_stage, 0, sizeof t_stage);
    memset(t_count, 0, sizeof t_count);
    t01_div = t2_div = 0;
    spc_cycles = spc_target = dsp_at = 0;
    memset(out_at, 0, sizeof out_at);
    stop_at_port = 0;
    spc_carry = 0;
    in_ipl = 0;
    atomic_store(&ring_w, 0);
    atomic_store(&ring_r, 0);
    mix_init(aram);
    mix_regs_reset();
    if (!spclog && getenv("NSHVC_SPCLOG"))
        spclog = fopen(getenv("NSHVC_SPCLOG"), "wb");
    if (!spc_co)
        spc_co = coro_new(spc_main, NULL, CORO_SPC_STACK);
}

/* ---- time ---- */
/* k steps of timer n's divider: its stage counts to the target (0 counts
   256) and its 4-bit counter counts each time it gets there; the first
   time from where the stage stands (past the target, round through 0) */
static void timer_steps(int n, unsigned k)
{
    unsigned first = ((unsigned)(t_target[n] - t_stage[n] - 1) & 0xFFu) + 1u, period = t_target[n] ? t_target[n] : 256u;
    if (!(control & (1u << n)) || !k)
        return;
    if (k < first) {
        t_stage[n] = (uint8_t)(t_stage[n] + k);
        return;
    }
    t_count[n] = (uint8_t)((t_count[n] + 1 + (k - first) / period) & 15);
    t_stage[n] = (uint8_t)((k - first) % period);
}

/* The DSP's clock is brought up to the SPC700's when something could tell,
 * and it makes each sample that ends by then (mixer.c), each with what the
 * SPC700 had written by the clock the chip reads it. It reads the RAM and its
 * registers, which only the SPC700 writes (each write brings it up first);
 * the SPC700 sees it in its registers' reads and, where the echo may have
 * written, the RAM's; the S-CPU and the host see it in the samples, which
 * come at least each 1,024 cycles the SPC700 hands back and at each frame's
 * end (apu_flush). */
static void dsp_catch_up(void)
{
    uint64_t cycles = spc_cycles - dsp_at;
    dsp_at = spc_cycles;
    /* at most 63 frames' clocks at a time, so its frames fit */
    while (cycles) {
        int16_t made[2 * 64];
        unsigned k = cycles > 32u * 63u ? 32u * 63u : (unsigned)cycles;
        int got = mix_run((int)k, made, 64), i;
        cycles -= k;
        for (i = 0; i < got; i++) {
            unsigned w = atomic_load_explicit(&ring_w, memory_order_relaxed);
            unsigned r = atomic_load_explicit(&ring_r, memory_order_acquire);
            if (w - r < RING) {
                ring[(w % RING) * 2] = made[2 * i];
                ring[(w % RING) * 2 + 1] = made[2 * i + 1];
                atomic_store_explicit(&ring_w, w + 1, memory_order_release);
            }
        }
    }
}

/* the DSP up to the SPC700's cycle, and the echo's writes by then in the RAM */
static void dsp_see_ram(void)
{
    dsp_catch_up();
    mix_settle();
}

void apu_flush(void)
{
    dsp_see_ram();
    if (spc_co && !coro_stack_ok(spc_co))
        snes_fatal("the SPC700's code used all of its stack's %u bytes", (unsigned)CORO_SPC_STACK);
}

static void spc_pass(unsigned cycles)
{
    spc_cycles += cycles;
    t01_div += cycles;
    if (t01_div >= 128) {
        timer_steps(0, t01_div / 128);
        timer_steps(1, t01_div / 128);
        t01_div %= 128;
    }
    t2_div += cycles;
    if (t2_div >= 16) {
        timer_steps(2, t2_div / 16);
        t2_div %= 16;
    }
}

static void spc_advance(unsigned cycles)
{
    spc_pass(cycles);
    /* (the boot ROM goes back from boot_run, on the S-CPU's stack) */
    if (in_ipl)
        return;
    /* an access at or before the S-CPU's time (spc_target: its time in
       whole cycles, rounded down) is made now; one past it waits */
    if (spc_cycles > spc_target) {
        if (spc_cycles - dsp_at >= 1024)
            dsp_catch_up();
        coro_switch(spc_co, main_co);
    }
}

/* The sound unit's time from power on, in master clocks: it starts with
   the console's clock, the S-CPU SNES_POWER_CLOCK later (its reset); the
   console's clock 0 is 4 clocks into it (the line's start here is 4 clocks
   after the counters', tests/rom's measured frame) */
static uint64_t apu_clock(void) { return snes_clock + 4; }

/* the SPC700's time at the sound unit's clock c. The master clock's rate:
   NTSC's 315/88 x 6 MHz whole (the reference's, against which the two
   processors' ports are timed) */
static uint64_t spc_time_at(uint64_t c)
{
    uint64_t m = snes_pal() ? 21281370u : 21477272u;
    return c / m * SPC_HZ + c % m * SPC_HZ / m;
}

static void spc_run_to_target(void)
{
    if (spc_cycles <= spc_target) {
        if (!in_ipl) {
            main_co = coro_current();
            coro_switch(main_co, spc_co);
        }
        if (in_ipl)
            boot_run();
    }
}

void apu_sync(void)
{
    spc_target = spc_time_at(apu_clock());
    spc_run_to_target();
}

/* A wait of the S-CPU's on the ports, with nothing that can write them before
   the console's clock `until`: the SPC700 run ahead through that time, back at
   its first change of a port, so the wait goes to it directly. *at: the first
   clock a read of the port sees it (the first whose spc_time_at reaches its
   cycle), or ~0 with no change before `until`. 0 when the log is on: its records
   are in the order they are made. */
int apu_port_change(uint64_t until, uint64_t *at)
{
    uint64_t now, best = ~(uint64_t)0, m = snes_pal() ? 21281370u : 21477272u;
    int n;
    if (spclog)
        return 0;
    now = spc_time_at(apu_clock());
    /* A change made and not yet seen (the boot ROM's byte, worked out ahead):
       that is the next, and the SPC700 goes no further, or it would pass what
       the S-CPU writes in answer. */
    for (n = 0; n < 4; n++)
        if (out_at[n] > now && out_at[n] < best)
            best = out_at[n];
    if (best == ~(uint64_t)0) {
        spc_target = spc_time_at(until + 4);
        stop_at_port = 1;
        spc_run_to_target();
        stop_at_port = 0;
        spc_target = now;
        for (n = 0; n < 4; n++)
            if (out_at[n] > now && out_at[n] < best)
                best = out_at[n];
    }
    *at = ~(uint64_t)0;
    if (best != ~(uint64_t)0)
        *at = best / SPC_HZ * m + (best % SPC_HZ * m + SPC_HZ - 1) / SPC_HZ - 4;
    return 1;
}

int snes_audio_queued(void)
{
    return (int)(atomic_load(&ring_w) - atomic_load(&ring_r));
}

int snes_audio_take(int16_t *lr, int max)
{
    unsigned r = atomic_load_explicit(&ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&ring_w, memory_order_acquire);
    int n = 0;
    while (r != w && n < max) {
        lr[n * 2] = ring[(r % RING) * 2];
        lr[n * 2 + 1] = ring[(r % RING) * 2 + 1];
        r++;
        n++;
    }
    atomic_store_explicit(&ring_r, r, memory_order_release);
    return n;
}

/* ---- the S-CPU's side ---- */
uint8_t apu_port_read(uint8_t port)
{
    port &= 3;
    if (spc_target < out_at[port])
        return out_before[port];
    return port_out[port];
}
void apu_port_write(uint8_t port, uint8_t v)
{
    port &= 3;
    port_prev[port] = port_in[port];
    port_at[port] = apu_clock();
    port_in[port] = v;
    if (spclog) {                 /* kind 2: the S-CPU's, at its time in SPC700 half cycles */
        uint32_t m = snes_pal() ? 21281370u : 21477272u;
        uint32_t t = wide_div_lo(wide_mul(port_at[port], 2u * SPC_HZ), m);
        uint8_t rec[8] = { (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24), 2,
                           (uint8_t)(0xF4 + port), 0, v };
        fwrite(rec, 1, 8, spclog);
    }
}

/* port n as a read now sees it: its value before the S-CPU's last write if
   the read's half-cycle point, spc_cycles - 1/2, comes before that write */
static uint8_t port_read(int n)
{
    uint32_t m = snes_pal() ? 21281370u : 21477272u;
    if (spc_cycles && wide_less(wide_mul(2 * spc_cycles - 1, m), wide_mul(port_at[n], 2u * SPC_HZ)))
        return port_prev[n];
    return port_in[n];
}
uint8_t apu_debug_port(int n) { return port_in[n & 3]; }
static uint16_t dbg_last;
void apu_debug(char *out, int n)
{
    snprintf(out, (size_t)n, "SPC %llu/%llu last %04X control %02X", (unsigned long long)spc_cycles,
             (unsigned long long)spc_target, dbg_last, control);
}

/* ---- the SPC700's bus (spc.h) ---- */
uint8_t spc_rd(uint16_t addr);
void spc_wr(uint16_t addr, uint8_t v);
void spc_restart(uint16_t at, void (*entry)(void));
void spc_boot_rom(void);

/* A wait (spc.h): the loop's reads noted as its condition makes them,
 * each with the cycle from which a read of it could next answer otherwise
 * (turn_note); the clock goes there and the condition looks again. A
 * port: where a write of the S-CPU's comes into sight (port_read), or,
 * seen already, past the S-CPU's time, where its next write can come (the
 * SPC700 hands back there); a timer's counter: its next count; RAM the echo
 * may write, and the DSP's registers the chip changes: the next clock it can
 * (mix_ram_next, mix_reg_next); the rest of RAM, the DSP's other registers and
 * $F0-$F2, only the driver's own writes. A
 * condition that writes looks again a cycle on. */
static int turn_on, turn_step;
static uint64_t turn_until;

/* the cycle a timer's counter next counts at (never: ~0) */
static uint64_t timer_next(int n)
{
    unsigned period = n < 2 ? 128 : 16, div = n < 2 ? t01_div : t2_div;
    unsigned steps = ((unsigned)(t_target[n] - t_stage[n] - 1) & 0xFFu) + 1u;   /* a target of 0 counts 256 */
    if (!(control & (1u << n)))
        return ~(uint64_t)0;
    return spc_cycles + (period - div) + (uint64_t)(steps - 1) * period;
}

/* a read of port n at cycle t sees its value before the S-CPU's last
   write (port_read's test) */
static int port_old_at(int n, uint64_t t)
{
    uint32_t m = snes_pal() ? 21281370u : 21477272u;
    return t && wide_less(wide_mul(2 * t - 1, m), wide_mul(port_at[n], 2u * SPC_HZ));
}

/* the first cycle from t at which port n shows its value after the
   S-CPU's last write (port_old_at: old before it, new from it on), up to
   `most` */
static uint64_t port_new_from(int n, uint64_t t, uint64_t most)
{
    uint64_t lo = t, hi = most;
    if (!port_old_at(n, t))
        return t;
    if (port_old_at(n, most))
        return most;
    while (hi - lo > 1) {                     /* old at lo, new at hi */
        uint64_t mid = lo + (hi - lo) / 2;
        if (port_old_at(n, mid))
            lo = mid;
        else
            hi = mid;
    }
    return hi;
}

static void turn_note(uint16_t addr, uint8_t v)
{
    uint64_t c = ~(uint64_t)0;
    (void)v;
    if (addr >= 0xF4 && addr <= 0xF7) {
        int n = addr - 0xF4;
        c = port_prev[n] != port_in[n] && port_old_at(n, spc_cycles) ? port_new_from(n, spc_cycles, spc_target + 1)
                                                                      : spc_target + 1;
    } else if (addr >= 0xFD && addr <= 0xFF) {
        int n = addr - 0xFD;
        c = t_count[n] ? spc_cycles : timer_next(n);
    } else if (addr == 0xF3) {
        unsigned n = mix_reg_next(dsp_addr & 0x7F);
        if (n)
            c = spc_cycles + n;
    } else if (mix_echo_may_write(addr)) {
        c = spc_cycles + (unsigned)mix_ram_next();
    }
    if (c < turn_until)
        turn_until = c;
}

uint32_t spc_wait_until(int (*ready)(void *ctx), void *ctx)
{
    uint64_t from = spc_cycles;
    for (;;) {
        int r;
        turn_on = 1;
        turn_step = 0;
        turn_until = ~(uint64_t)0;
        r = ready(ctx);
        turn_on = 0;
        if (r)
            break;
        /* (with NSHVC_SPCLOG, each turn a cycle: the log's turns) */
        if (turn_step || spclog || turn_until <= spc_cycles)
            turn_until = spc_cycles + 1;
        spc_advance((unsigned)(turn_until - spc_cycles < 0x10000000u ? turn_until - spc_cycles : 0x10000000u));
    }
    return (uint32_t)(spc_cycles - from);
}

static uint8_t spc_rd_reg(uint16_t addr);

/* Each access the driver's C makes costs its bus cycle, and before it the
   weight of its kind from the cartridge's cost profile (nshvc.h
   spc_tenths: [read, write][RAM, registers], tenths of a cycle carried from
   one access to the next; 0 with none): the driver's own work around it,
   its fetches and internal cycles (spc.h). So it answers the S-CPU's
   handshakes in the console's time. The boot ROM's work states its own
   cycles, the accesses' included (ipl, below). With the costs off
   (nshvc_set_cpu_costs(0), a game's Enhanced mode) the driver's C takes no
   time, as the S-CPU's does: its time passes in its waits (a loop of reads
   that waits for it and so never ends: the front end's watchdog). */

static void access_cycle(uint16_t addr, int write)
{
    const nshvc_cost_profile *p = snes_cart.cost_profile;
    unsigned n = 1;
    if (in_ipl)
        return;
    if (!snes_costs_on())
        return;
    if (p) {
        spc_carry += p->spc_tenths[write][addr >= 0xF0 && addr <= 0xFF];
        n += spc_carry / 10;
        spc_carry %= 10;
    }
    spc_advance(n);
}

uint8_t spc_rd(uint16_t addr)
{
    uint8_t v;
    access_cycle(addr, 0);
    dbg_last = addr;
    if (addr < 0xF0 || addr > 0xFF) {
        if (mix_echo_may_write(addr))
            dsp_see_ram();
        v = aram[addr];
    } else {
        v = spc_rd_reg(addr);
        if (spclog)
            spclog_put(0, addr, v);
    }
    if (turn_on)
        turn_note(addr, v);
    return v;
}

static uint8_t spc_rd_reg(uint16_t addr)
{
    uint8_t v;
    switch (addr) {
    case 0xF2: return dsp_addr;
    case 0xF3: dsp_catch_up(); return mix_reg_read(dsp_addr & 0x7F);
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: return port_read(addr - 0xF4);
    case 0xF8: case 0xF9:
        if (mix_echo_may_write(addr))
            dsp_see_ram();
        return aram[addr];
    case 0xFD: case 0xFE: case 0xFF:
        v = t_count[addr - 0xFD];
        t_count[addr - 0xFD] = 0;
        return v;
    }
    return 0;
}

void spc_wr(uint16_t addr, uint8_t v)
{
    turn_step |= turn_on;
    access_cycle(addr, 1);
    dsp_catch_up();
    /* a key event the DSP has not taken yet: the next write to the DSP
       waits for it (spc.h), to the clock the DSP takes it */
    if (addr == 0xF3 && dsp_addr < 0x80)
        while (mix_key_unread()) {
            spc_advance((unsigned)mix_key_clocks());
            dsp_catch_up();
        }
    if (spclog)
        spclog_put(1, addr, v);
    /* a write to a register lands in the RAM under it too, where the DSP,
       which reads the RAM itself, finds it */
    mix_before_ram_write();
    aram[addr] = v;
    if (addr < 0xF0 || addr > 0xFF)
        return;
    switch (addr) {
    case 0xF1: {
        int n;
        /* a timer is restarted as its enable goes from 0 to 1; timer 2 on
           every write that does not do that (the reference's, measured by
           tests/dsp/fuzz.py) */
        for (n = 0; n < 3; n++)
            if ((n == 2) != (!(control & (1u << n)) && (v & (1u << n)))) {
                t_stage[n] = 0;
                t_count[n] = 0;
            }
        if (v & 0x10)
            port_in[0] = port_in[1] = port_prev[0] = port_prev[1] = 0;
        if (v & 0x20)
            port_in[2] = port_in[3] = port_prev[2] = port_prev[3] = 0;
        control = v;
        return;
    }
    case 0xF2: dsp_addr = v; return;
    case 0xF3:
        if (dsp_addr < 0x80)
            mix_reg_write(dsp_addr, v);
        return;
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: {
        int n = addr - 0xF4;
        if (port_out[n] == v)
            return;
        /* (several at a cycle: all show from it) */
        if (out_at[n] != spc_cycles)
            out_before[n] = port_out[n];
        out_at[n] = spc_cycles;
        port_out[n] = v;
        if (stop_at_port)
            spc_target = spc_cycles;
        return;
    }
    case 0xFA: case 0xFB: case 0xFC: t_target[addr - 0xFA] = v; return;
    }
}

/* spc.h's two ends: the driver's jump to code that restarts it (its reload
   loop), and its jump to the boot ROM. The C's own frames end here (a jump
   back to the coroutine's top), and the coroutine goes on at that code. */
void spc_restart(uint16_t at, void (*entry)(void))
{
    jump_to = at;
    jump_entry = entry;
    port_longjmp(jb);
}

void spc_boot_rom(void)
{
    jump_to = 0xFFC0;
    jump_entry = NULL;
    port_longjmp(jb);
}

const uint8_t *snes_aram(void) { return aram; }

/* The boot ROM's work (see the file's comment): its protocol as states. A
   state that reads a port stands at its cycle until the S-CPU's time has
   reached it, and goes to the next by the ROM's fixed cycle counts. The ROM is
   the console's own code, so its time is the console's: an S-CPU that writes a
   port's index a cycle before its byte (a word to $2140) is answered as the
   console answers it. A byte, its index seen, is one step (boot_byte). */
enum {
    B_START, B_CC, B_NEXT, B_BYTE, B_DATA, B_STORE_RD, B_STORE_WR, B_PAGE, B_ANOTHER, B_F6, B_F7, B_F4, B_F5, B_JUMP
};
static unsigned boot_state;
static uint8_t boot_a, boot_x, boot_y, boot_v;
static uint16_t boot_p;
static int boot_n, boot_z;

static uint8_t boot_nz(uint8_t v)
{
    boot_z = v == 0;
    boot_n = v >> 7;
    return v;
}

static void boot_go(unsigned cycles, unsigned state)
{
    spc_pass(cycles);
    boot_state = state;
}

/* A poll's turns that see what the last saw, passed at once: the next turn
   reads port n `at` cycles in and is `turn` long; while its read is at or
   before the S-CPU's time (where the S-CPU's port writes come in) and sees
   `seen`, the turn is the last again, its time passed. A write still coming
   into sight (port_read) is met turn by turn. */
static void boot_hold(unsigned at, unsigned turn, int n, uint8_t seen)
{
    uint64_t t = spc_cycles + at, k;
    if (spclog || t > spc_target || port_in[n] != seen)
        return;
    if (port_prev[n] != port_in[n] && port_old_at(n, t))
        return;
    k = (spc_target - t) / turn + 1;
    spc_pass((unsigned)(k < 0xFFFFu ? k : 0xFFFFu) * turn);
}

/* The clear's writes can be seen by nothing: each puts 0 over 0, and the echo
   cannot write among them. (The DSP reads this RAM on its own clock, and a
   driver's zero page is not all 0 when it jumps back here: then the writes
   are made in turn, each between the same two of its clocks.) */
static int boot_clear_unseen(void)
{
    unsigned p;
    if (spclog)
        return 0;
    for (p = 0x01; p <= 0xEF; p++)
        if (aram[p] || mix_echo_may_write((uint16_t)p))
            return 0;
    return 1;
}

/* FFE9 bpl $FFDA: to the byte's poll, or on to see a new block */
static void boot_skip(int polling, uint8_t seen)
{
    if (boot_n) {
        boot_go(2 + 3, B_ANOTHER);
        return;
    }
    spc_pass(4);
    if (polling)
        boot_hold(3, 11, 0, seen);
    boot_go(3, B_BYTE);
}

/* After the store: FFE4 inc y; FFE5 bne $FFDA */
static void boot_stored(void)
{
    boot_y = boot_nz((uint8_t)(boot_y + 1));
    if (boot_z)
        boot_go(2 + 2 + 3, B_PAGE);
    else
        boot_go(2 + 4 + 3, B_BYTE);
}

/* A byte's work, from the read of its value to the next poll, in one step:
   FFDE mov a,$F5; FFE0 mov $F4,y; FFE2 mov [$00]+y,a. Nothing in it reads what
   the S-CPU can still change, so it is worked out at once, up to 20 cycles
   ahead of the S-CPU. The S-CPU sees the echo from the echo's cycle
   (apu_port_read); the DSP sees the RAM at its cycles, as spc_wr brings it up
   to each. The ROM's other reads, of $F4 and of the byte it stores over, change
   nothing. A store to the registers can touch the ports and the timers, whose
   time is the S-CPU's: that one waits for it as a state of its own. */
static void boot_byte(void)
{
    uint16_t to;
    boot_a = boot_nz(spc_rd(0xF5));
    spc_pass(3);
    /* (the log's record of it) */
    if (spclog)
        spc_rd(0xF4);
    spc_pass(1);
    spc_wr(0xF4, boot_y);
    spc_pass(3);
    boot_p = spc_rd(0x00);
    spc_pass(1);
    boot_p = (uint16_t)(boot_p | spc_rd(0x01) << 8);
    spc_pass(2);
    to = (uint16_t)(boot_p + boot_y);
    if (to >= 0xF0 && to <= 0xFF) {
        boot_state = B_STORE_RD;
        return;
    }
    if (spclog)
        spc_rd(to);
    spc_pass(1);
    spc_wr(to, boot_a);
    boot_stored();
}

/* The ROM's start, up to its first poll, in one step: nothing in it reads what the
   S-CPU can change. FFC0 mov x,#$EF; FFC2 mov sp,x; FFC3 mov a,#$00; then the
   clear, FFC5 mov (x),a; FFC6 dec x; FFC7 bne $FFC5, $EF down to $01, 10 cycles a
   turn and 8 the last; FFC9 mov $F4,#$AA; FFCC mov $F5,#$BB. Each write's cycle
   its own (the DSP sees the RAM at it, the S-CPU the ports from it). */
static void boot_start(void)
{
    boot_x = 0xEF;
    spc.sp = boot_x;
    boot_a = 0x00;
    spc_pass(2 + 2 + 2);
    if (boot_clear_unseen()) {
        boot_x = 0;
        spc_pass(238 * 10 + 8 + 4);
    } else {
        spc_pass(3);
        for (;;) {
            if (spclog)
                spc_rd(boot_x);
            spc_pass(1);
            spc_wr(boot_x, boot_a);
            boot_x = boot_nz((uint8_t)(boot_x - 1));
            if (boot_z)
                break;
            spc_pass(2 + 4 + 3);
        }
        spc_pass(2 + 2 + 4);
    }
    if (spclog)
        spc_rd(0xF4);
    spc_pass(1);
    spc_wr(0xF4, 0xAA);
    spc_pass(4);
    if (spclog)
        spc_rd(0xF5);
    spc_pass(1);
    spc_wr(0xF5, 0xBB);
    boot_go(4, B_CC);
}

static void boot_run(void)
{
    while (in_ipl && spc_cycles <= spc_target) {
        switch (boot_state) {
        case B_START:
            boot_start();
            break;
        case B_CC:
            /* FFCF cmp $F4,#$CC; FFD2 bne $FFCF; FFD4 bra $FFEF */
            boot_v = spc_rd(0xF4);
            if (boot_v == 0xCC) {
                boot_go(1 + 2 + 4 + 3, B_F6);
            } else {
                spc_pass(1 + 4);
                boot_hold(4, 9, 0, boot_v);
                boot_go(4, B_CC);
            }
            break;
        case B_NEXT:
            /* FFD6 mov y,$F4; FFD8 bne $FFD6 */
            boot_y = boot_nz(spc_rd(0xF4));
            if (boot_z) {
                boot_go(2 + 3, B_BYTE);
            } else {
                spc_pass(4);
                boot_hold(3, 7, 0, boot_y);
                boot_go(3, B_NEXT);
            }
            break;
        case B_BYTE:
            /* FFDA cmp y,$F4; FFDC bne $FFE9 */
            boot_v = spc_rd(0xF4);
            boot_nz((uint8_t)(boot_y - boot_v));
            if (boot_z) {
                boot_go(2 + 3, B_DATA);
            } else {
                spc_pass(4);
                boot_skip(1, boot_v);
            }
            break;
        case B_DATA:
            boot_byte();
            break;
        case B_STORE_RD:
            spc_rd((uint16_t)(boot_p + boot_y));
            boot_go(1, B_STORE_WR);
            break;
        case B_STORE_WR:
            spc_wr((uint16_t)(boot_p + boot_y), boot_a);
            boot_stored();
            break;
        case B_PAGE:
            /* FFE7 inc $01 */
            boot_v = boot_nz((uint8_t)(spc_rd(0x01) + 1));
            spc_pass(1);
            spc_wr(0x01, boot_v);
            boot_skip(0, 0);
            break;
        case B_ANOTHER:
            /* FFEB cmp y,$F4; FFED bpl $FFDA */
            boot_nz((uint8_t)(boot_y - spc_rd(0xF4)));
            if (boot_n)
                boot_go(2 + 3, B_F6);
            else
                boot_go(4 + 3, B_BYTE);
            break;
        case B_F6:
            /* FFEF movw ya,$F6; FFF1 movw $00,ya */
            boot_a = spc_rd(0xF6);
            boot_go(2, B_F7);
            break;
        case B_F7:
            /* (then FFF1 movw $00,ya, reading no port) */
            boot_y = spc_rd(0xF7);
            spc_pass(3);
            if (spclog)
                spc_rd(0x00);
            spc_pass(1);
            spc_wr(0x00, boot_a);
            spc_pass(1);
            spc_wr(0x01, boot_y);
            boot_go(3, B_F4);
            break;
        case B_F4:
            /* FFF3 movw ya,$F4; FFF5 mov $F4,a */
            boot_a = spc_rd(0xF4);
            boot_go(2, B_F5);
            break;
        case B_F5:
            /* (then FFF5 mov $F4,a; FFF7 mov a,y; FFF8 mov x,a; FFF9 bne $FFD6) */
            boot_y = spc_rd(0xF5);
            spc_pass(3);
            if (spclog)
                spc_rd(0xF4);
            spc_pass(1);
            spc_wr(0xF4, boot_a);
            boot_a = boot_y;
            boot_x = boot_nz(boot_a);
            if (boot_z)
                boot_go(2 + 2 + 2 + 5, B_JUMP);
            else
                boot_go(2 + 2 + 4 + 3, B_NEXT);
            break;
        case B_JUMP:
            /* FFFB jmp [$0000+x] */
            boot_p = spc_rd(boot_x);
            spc_pass(1);
            boot_p = (uint16_t)(boot_p | spc_rd((uint8_t)(boot_x + 1)) << 8);
            spc.a = boot_a;
            spc.x = boot_x;
            spc.y = boot_y;
            spc.n = (uint8_t)boot_n;
            spc.z = (uint8_t)boot_z;
            jump_to = boot_p;
            jump_entry = NULL;
            in_ipl = 0;
            main_co = coro_current();
            coro_switch(main_co, spc_co);
            break;
        }
    }
    if (spc_cycles > spc_target && spc_cycles - dsp_at >= 1024)
        dsp_catch_up();
}

static void spc_main(void *arg)
{
    (void)arg;
    jump_to = 0xFFC0;
    jump_entry = NULL;
    for (;;) {
        if (port_setjmp(jb) == 0) {
            /* the boot ROM runs from apu_sync (boot_run), and comes back here
               where it jumps */
            while (jump_to == 0xFFC0) {
                if (!(control & 0x80))
                    snes_fatal("SPC700 jumps to $FFC0 with the boot ROM unmapped");
                in_ipl = 1;
                boot_state = B_START;
                while (in_ipl)
                    coro_switch(spc_co, main_co);
            }
            if (jump_entry)
                jump_entry();
            else if (snes_spc_run_hook) {
                dsp_see_ram();                /* (the routine is found by the RAM's bytes) */
                snes_spc_run_hook(jump_to);
            } else
                snes_fatal("SPC700 started with no runtime");
            snes_fatal("SPC700 code at $%04X returned", jump_to);
        }
    }
}
