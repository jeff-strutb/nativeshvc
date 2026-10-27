/*
 * apu.c: the sound unit beside the S-CPU: the SPC700's 64 KB of RAM, its
 * registers ($F0-$FF: control, the DSP's address and data, the four ports,
 * the three timers), the bus its C uses (spc.h), and the boot ROM's work.
 *
 * The SPC700's C runs on a coroutine of its own with its own clock
 * (1,025,280 Hz: 32,040 DSP samples a second, a real console's rate), which
 * the time the C states moves on (spc_cyc: the original's cycles up to and
 * including each access's own). The S-CPU brings it up to its
 * own time before it touches the ports and once a line (apu_sync); the
 * SPC700 hands back when it gets there. The DSP runs a clock a cycle, as
 * the chip does, and makes a sample every 32, into a ring the host's audio
 * thread drains (snes_audio_take).
 *
 * At power on the SPC700 runs its boot ROM: it clears its zero page, puts
 * $AA, $BB on ports 0 and 1, and takes the blocks the S-CPU sends by the
 * ROM's protocol, then jumps where the S-CPU says. The ROM's own bytes are
 * not here: ipl() does what that code does, instruction for instruction,
 * its accesses and its cycles as the decomp's SPC700 C states them, so the
 * S-CPU's upload sees the same replies at the same pace. Code the S-CPU
 * uploads and starts is the game's C (the runtime's snes_spc_hooks).
 */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "snes_internal.h"
#include "nshvc/spc.h"
#include "coro.h"
#include "jump.h"

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
    spc_cycles = spc_target = 0;
    atomic_store(&ring_w, 0);
    atomic_store(&ring_r, 0);
    mix_init(aram);
    mix_regs_reset();
    if (!spclog && getenv("NSHVC_SPCLOG"))
        spclog = fopen(getenv("NSHVC_SPCLOG"), "wb");
    if (!spc_co)
        spc_co = coro_new(spc_main, NULL, 4u << 20);
}

/* ---- time ---- */
static void timer_tick(int n)
{
    if (!(control & (1u << n)))
        return;
    if (++t_stage[n] == t_target[n]) {          /* a target of 0 counts 256 */
        t_stage[n] = 0;
        t_count[n] = (uint8_t)((t_count[n] + 1) & 15);
    }
}

static void spc_advance(unsigned cycles)
{
    spc_cycles += cycles;
    t01_div += cycles;
    while (t01_div >= 128) {
        t01_div -= 128;
        timer_tick(0);
        timer_tick(1);
    }
    t2_div += cycles;
    while (t2_div >= 16) {
        t2_div -= 16;
        timer_tick(2);
    }
    /* the DSP a clock a cycle, so an access after this sees it where the
       chip would be */
    while (cycles--) {
        unsigned w, r;
        int16_t l, rr;
        if (!mix_clock(&l, &rr))
            continue;
        w = atomic_load_explicit(&ring_w, memory_order_relaxed);
        r = atomic_load_explicit(&ring_r, memory_order_acquire);
        if (w - r < RING) {
            ring[(w % RING) * 2] = l;
            ring[(w % RING) * 2 + 1] = rr;
            atomic_store_explicit(&ring_w, w + 1, memory_order_release);
        }
    }
    /* an access at or before the S-CPU's time (spc_target: its time in
       whole cycles, rounded down) is made now; one past it waits */
    if (spc_cycles > spc_target)
        coro_switch(spc_co, main_co);
}

/* The sound unit's time from power on, in master clocks: it starts with
   the console's clock, the S-CPU SNES_POWER_CLOCK later (its reset); the
   console's clock 0 is 4 clocks into it (the line's start here is 4 clocks
   after the counters', tests/rom's measured frame) */
static uint64_t apu_clock(void) { return snes_clock + 4; }

void apu_sync(void)
{
    uint64_t c = apu_clock();
    {
        /* the master clock's rate: NTSC's 315/88 x 6 MHz whole (the
           reference's, against which the two processors' ports are timed) */
        uint64_t m = snes_pal() ? 21281370u : 21477272u;
        spc_target = c / m * SPC_HZ + c % m * SPC_HZ / m;
    }
    if (spc_cycles <= spc_target) {
        main_co = coro_current();
        coro_switch(main_co, spc_co);
    }
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
uint8_t apu_port_read(uint8_t port) { return port_out[port & 3]; }
void apu_port_write(uint8_t port, uint8_t v)
{
    port &= 3;
    port_prev[port] = port_in[port];
    port_at[port] = apu_clock();
    port_in[port] = v;
    if (spclog) {                 /* kind 2: the S-CPU's, at its time in SPC700 half cycles */
        uint64_t m = snes_pal() ? 21281370u : 21477272u;
        uint32_t t = (uint32_t)((unsigned __int128)port_at[port] * (2u * SPC_HZ) / m);
        uint8_t rec[8] = { (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)(t >> 16), (uint8_t)(t >> 24), 2,
                           (uint8_t)(0xF4 + port), 0, v };
        fwrite(rec, 1, 8, spclog);
    }
}

/* port n as a read now sees it: its value before the S-CPU's last write if
   the read's half-cycle point, spc_cycles - 1/2, comes before that write */
static uint8_t port_read(int n)
{
    uint64_t m = snes_pal() ? 21281370u : 21477272u;
    if (spc_cycles && (unsigned __int128)(2 * spc_cycles - 1) * m < (unsigned __int128)port_at[n] * (2u * SPC_HZ))
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
void spc_cyc(unsigned n);

/* The time the C states: the clock moves on, and the SPC700 hands back to
   the S-CPU there when it reaches the S-CPU's time, before the access. */
void spc_cyc(unsigned n) { spc_advance(n); }

static uint8_t spc_rd_reg(uint16_t addr);

uint8_t spc_rd(uint16_t addr)
{
    uint8_t v;
    dbg_last = addr;
    if (addr < 0xF0 || addr > 0xFF)
        return aram[addr];
    if (spclog) {
        v = spc_rd_reg(addr);
        spclog_put(0, addr, v);
        return v;
    }
    return spc_rd_reg(addr);
}

static uint8_t spc_rd_reg(uint16_t addr)
{
    uint8_t v;
    switch (addr) {
    case 0xF2: return dsp_addr;
    case 0xF3: return mix_reg_read(dsp_addr & 0x7F);
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: return port_read(addr - 0xF4);
    case 0xF8: case 0xF9: return aram[addr];
    case 0xFD: case 0xFE: case 0xFF:
        v = t_count[addr - 0xFD];
        t_count[addr - 0xFD] = 0;
        return v;
    }
    return 0;
}

void spc_wr(uint16_t addr, uint8_t v)
{
    if (spclog)
        spclog_put(1, addr, v);
    /* a write to a register lands in the RAM under it too, where the DSP,
       which reads the RAM itself, finds it */
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
    case 0xF4: case 0xF5: case 0xF6: case 0xF7: port_out[addr - 0xF4] = v; return;
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
uint8_t *apu_aram_rw(void) { return aram; }

/* the boot ROM's work (see the file's comment): its 64 bytes at $FFC0, an
   instruction at a time, each with the cycles the decomp's SPC700 C states
   for that form (spc.h's spc_cyc: up to and including each access) */
static int ipl_n, ipl_z;
static uint8_t ipl_nz(uint8_t v)
{
    ipl_z = v == 0;
    ipl_n = v >> 7;
    return v;
}
static void ipl_cmp(uint8_t x, uint8_t y) { ipl_nz((uint8_t)(x - y)); }
/* a store: the dummy read, then the write */
static void ipl_st(uint16_t addr, uint8_t v, unsigned before_read, unsigned before_write)
{
    spc_cyc(before_read);
    spc_rd(addr);
    spc_cyc(before_write);
    spc_wr(addr, v);
}

static void ipl(void)
{
    uint8_t a, x, y;
    uint16_t p;
    /* FFC0 mov x,#$EF; FFC2 mov sp,x; FFC3 mov a,#$00 */
    x = ipl_nz(0xEF);
    spc_cyc(2);
    spc.sp = x;
    spc_cyc(2);
    a = ipl_nz(0x00);
    spc_cyc(2);
    do {
        /* FFC5 mov (x),a; FFC6 dec x; FFC7 bne $FFC5 */
        ipl_st(x, a, 3, 1);
        x = ipl_nz((uint8_t)(x - 1));
        spc_cyc(2);
        spc_cyc(ipl_z ? 2 : 4);
    } while (!ipl_z);
    /* FFC9 mov $F4,#$AA; FFCC mov $F5,#$BB */
    ipl_st(0xF4, 0xAA, 4, 1);
    ipl_st(0xF5, 0xBB, 4, 1);
    do {
        /* FFCF cmp $F4,#$CC; FFD2 bne $FFCF */
        spc_cyc(4);
        ipl_cmp(spc_rd(0xF4), 0xCC);
        spc_cyc(1);
        spc_cyc(ipl_z ? 2 : 4);
    } while (!ipl_z);
    /* FFD4 bra $FFEF */
    spc_cyc(4);
    goto block;
next:
    /* FFD6 mov y,$F4; FFD8 bne $FFD6 */
    spc_cyc(3);
    y = ipl_nz(spc_rd(0xF4));
    if (!ipl_z) {
        spc_cyc(4);
        goto next;
    }
    spc_cyc(2);
byte:
    /* FFDA cmp y,$F4; FFDC bne $FFE9 */
    spc_cyc(3);
    ipl_cmp(y, spc_rd(0xF4));
    if (!ipl_z) {
        spc_cyc(4);
        goto skip;
    }
    spc_cyc(2);
    /* FFDE mov a,$F5; FFE0 mov $F4,y */
    spc_cyc(3);
    a = ipl_nz(spc_rd(0xF5));
    ipl_st(0xF4, y, 3, 1);
    /* FFE2 mov [$00]+y,a */
    spc_cyc(3);
    p = spc_rd(0x00);
    spc_cyc(1);
    p = (uint16_t)(p | spc_rd(0x01) << 8);
    ipl_st((uint16_t)(p + y), a, 2, 1);
    /* FFE4 inc y; FFE5 bne $FFDA */
    y = ipl_nz((uint8_t)(y + 1));
    spc_cyc(2);
    if (!ipl_z) {
        spc_cyc(4);
        goto byte;
    }
    spc_cyc(2);
    /* FFE7 inc $01 */
    spc_cyc(3);
    {
        uint8_t v = ipl_nz((uint8_t)(spc_rd(0x01) + 1));
        spc_cyc(1);
        spc_wr(0x01, v);
    }
skip:
    /* FFE9 bpl $FFDA */
    if (!ipl_n) {
        spc_cyc(4);
        goto byte;
    }
    spc_cyc(2);
    /* FFEB cmp y,$F4; FFED bpl $FFDA */
    spc_cyc(3);
    ipl_cmp(y, spc_rd(0xF4));
    if (!ipl_n) {
        spc_cyc(4);
        goto byte;
    }
    spc_cyc(2);
block:
    /* FFEF movw ya,$F6; FFF1 movw $00,ya */
    spc_cyc(3);
    a = spc_rd(0xF6);
    spc_cyc(2);
    y = spc_rd(0xF7);
    spc_cyc(3);
    spc_rd(0x00);
    spc_cyc(1);
    spc_wr(0x00, a);
    spc_cyc(1);
    spc_wr(0x01, y);
    /* FFF3 movw ya,$F4; FFF5 mov $F4,a */
    spc_cyc(3);
    a = spc_rd(0xF4);
    spc_cyc(2);
    y = spc_rd(0xF5);
    ipl_st(0xF4, a, 3, 1);
    /* FFF7 mov a,y; FFF8 mov x,a; FFF9 bne $FFD6 */
    a = ipl_nz(y);
    spc_cyc(2);
    x = ipl_nz(a);
    spc_cyc(2);
    if (!ipl_z) {
        spc_cyc(4);
        goto next;
    }
    spc_cyc(2);
    /* FFFB jmp [$0000+x] */
    spc_cyc(5);
    p = spc_rd(x);
    spc_cyc(1);
    p = (uint16_t)(p | spc_rd((uint8_t)(x + 1)) << 8);
    spc.a = a;
    spc.x = x;
    spc.y = y;
    spc.n = (uint8_t)ipl_n;
    spc.z = (uint8_t)ipl_z;
    jump_to = p;
    jump_entry = NULL;
    port_longjmp(jb);
}

static void spc_main(void *arg)
{
    (void)arg;
    jump_to = 0xFFC0;
    jump_entry = NULL;
    for (;;) {
        if (port_setjmp(jb) == 0) {
            if (jump_to == 0xFFC0 && (control & 0x80))
                ipl();
            else if (jump_to == 0xFFC0)
                snes_fatal("SPC700 jumps to $FFC0 with the boot ROM unmapped");
            if (jump_entry)
                jump_entry();
            else if (snes_spc_run_hook)
                snes_spc_run_hook(jump_to);
            else
                snes_fatal("SPC700 started with no runtime");
            snes_fatal("SPC700 code at $%04X returned", jump_to);
        }
    }
}
