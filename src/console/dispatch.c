/*
 * dispatch.c: what the decomp's C asks of the machine beyond the bus
 * (machine.h, spc.h): the routines it reaches through computed addresses,
 * the two ends no C follows, the interrupts, and where each processor's C
 * starts.
 *
 *   call_routine, jump_routine   the routine with C that starts at the
 *                                address (nshvc_cart.routines); none is
 *                                the original's own runaway (a table read
 *                                past its end), a fatal error
 *   cpu_loop_forever             time passes, interrupts run, for ever
 *   cpu_restart                  every native frame dropped (a jump back
 *                                to the top of the S-CPU's run), then the
 *                                entry's C
 *   interrupts                   the interrupt's sequence (the frame it
 *                                pushes, the vector), then the vector's
 *                                code: the routine with C that starts
 *                                there, run as the handler and returned
 *                                from by RTI; none is a fatal error
 *   SPC700                       the routine with C at the APU address
 *                                whose code the APU RAM holds
 *
 * The S-CPU's C runs on a coroutine of its own: nshvc_run_frame switches
 * to it, and the vertical blank's start switches back.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/machine.h"
#include "nshvc/spc.h"
#include "snes_internal.h"
#include "nshvc/cart.h"
#include "coro.h"
#include "jump.h"

Regs65816 cpu;

static port_jmp_buf top;
static void (*next_entry)(void);
static Coro *cpu_co, *host_co;
static int powered;
static void (*user_frame_hook)(void);

static const nshvc_routine *find(uint32_t addr)
{
    int lo = 0, hi = snes_cart.routine_count - 1;
    addr &= 0xFFFFFF;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (snes_cart.routines[mid].addr == addr)
            return &snes_cart.routines[mid];
        if (snes_cart.routines[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* the connector's (cart.h): a chip's way to the cartridge's routines */
const nshvc_routine *nshvc_cart_routine(uint32_t addr) { return find(addr); }

/* The game reached an address with no routine: on the console the CPU
   would run whatever bytes are there (the original's own crash). */
static void runaway(const char *how, uint32_t addr)
{
    snes_fatal("the game %s $%02X:%04X, where no routine with C starts (the original runs off its program here)",
               how, (unsigned)(addr >> 16 & 0xFF), (unsigned)(addr & 0xFFFF));
}

void call_routine(uint32_t addr, int long_call)
{
    const nshvc_routine *r = find(addr);
    (void)long_call;
    if (!r)
        runaway("calls", addr);
    r->abi();
}

void jump_routine(uint32_t addr)
{
    const nshvc_routine *r = find(addr);
    if (!r)
        runaway("jumps to", addr);
    r->abi();
}

void jump_routine_pushed(uint32_t addr, const uint8_t *pushed, int n)
{
    const nshvc_routine *r = find(addr);
    int k;
    if (!r)
        runaway("jumps to", addr);
    for (k = 0; k < n; k++)
        wr8((uint16_t)(cpu.s - k), pushed[k]);
    cpu.s = (uint16_t)(cpu.s - n);
    r->abi();
}

void cpu_loop_forever(uint32_t at)
{
    (void)at;
    snes_loop_forever();
}

void cpu_wai(uint32_t at)
{
    (void)at;
    snes_wai();
}

void cpu_restart(uint32_t at, void (*entry)(void))
{
    (void)at;
    next_entry = entry;
    port_longjmp(top);
}

/* ---- interrupts ---- */
static void push8(uint8_t v)
{
    snes_interrupt_push(cpu.e ? (uint16_t)(0x0100 | (cpu.s & 0xFF)) : (uint16_t)cpu.s, v);
    cpu.s = cpu.e ? (uint16_t)(0x0100 | ((cpu.s - 1) & 0xFF)) : (uint16_t)(cpu.s - 1);
}

static uint8_t pull8(void)
{
    cpu.s = cpu.e ? (uint16_t)(0x0100 | ((cpu.s + 1) & 0xFF)) : (uint16_t)(cpu.s + 1);
    return snes_cpu_read_within((uint16_t)cpu.s);
}

/* RTI: P and PC (and PB in native mode) pulled */
static void rti(void)
{
    pull8();
    pull8();
    pull8();
    if (!cpu.e)
        pull8();
}

/* The interrupt: the sequence the CPU runs (the frame it pushes, the
   vector read, I set and D clear), then the vector's code. The handler's C
   runs with the frame pushed and the registers as the interrupted code
   left them, up to its RTI, whose pulls are made here. The registers are put back after, as the handler's
   own pushes and pulls and RTI leave them on the console. */
static void take_interrupt(int nmi)
{
    Regs65816 saved = cpu;
    uint32_t vector = cpu.e ? (nmi ? 0x00FFFA : 0x00FFFE) : (nmi ? 0x00FFEA : 0x00FFEE);
    uint32_t at = (uint16_t)(snes_peek(vector) | snes_peek(vector + 1) << 8);
    const nshvc_routine *r;
    if (!cpu.e)
        push8((uint8_t)(cpu.pc >> 16));
    push8((uint8_t)(cpu.pc >> 8));
    push8((uint8_t)cpu.pc);
    push8(cpu.p);
    /* I set and D cleared before the vector's read */
    cpu.p = (uint8_t)((cpu.p | P_I) & ~P_D);
    cpu.pc = at;
    cpu.db = saved.db;
    r = find(at);
    if (!r)
        snes_fatal("interrupt vector $%04X: no routine with C starts at $00:%04X (the code there needs C, as any "
                   "other code the CPU runs)", (unsigned)(vector & 0xFFFF), (unsigned)at);
    r->abi();
    if (cpu.unwound)
        snes_fatal("the interrupt handler at $00:%04X left through its caller's frame", (unsigned)at);
    rti();
    cpu = saved;
}

static int irq_masked(void) { return (cpu.p & P_I) != 0; }

/* ---- the SPC700 ---- */
/* the routine with C at APU address addr whose bytes the APU's RAM holds */
static void (*spc_find(uint16_t addr))(void)
{
    const uint8_t *aram = snes_aram();
    int i;
    for (i = 0; i < snes_cart.spc_routine_count; i++) {
        const nshvc_spc_routine *r = &snes_cart.spc_routines[i];
        const nshvc_spc_use *u;
        int ok = 1;
        if (r->apu != addr)
            continue;
        for (u = r->uses; u && ok && u->len; u++) {
            unsigned k;
            for (k = 0; ok && k < u->len; k++)
                ok = aram[(uint16_t)(u->apu + k)] == snes_rom_byte(snes_rom_mirror(u->rom + k), NSHVC_ROM_PEEK);
        }
        if (ok)
            return r->abi;
    }
    return NULL;
}

static void spc_run(uint16_t addr)
{
    void (*fn)(void);
    if (snes_cart.spc_run) {
        snes_cart.spc_run(addr);
        return;
    }
    fn = spc_find(addr);
    if (!fn)
        snes_fatal("SPC700 code at $%04X: no routine with C matches the APU's RAM", addr);
    fn();
}
/* ---- the S-CPU's run ---- */
static void cpu_main(void *arg)
{
    uint16_t v;
    const nshvc_routine *r;
    (void)arg;
    /* the reset: emulation mode, 8-bit registers, I set, the stack in page
       1, the reset vector's code */
    memset(&cpu, 0, sizeof cpu);
    cpu.e = 1;
    cpu.p = P_M | P_X | P_I;
    cpu.s = 0x01FF;
    v = (uint16_t)(snes_peek(0x00FFFC) | snes_peek(0x00FFFD) << 8);
    r = find(v);
    if (!r)
        snes_fatal("no C at the reset vector $00:%04X", v);
    cpu.pc = v;
    next_entry = r->abi;
    for (;;) {
        if (port_setjmp(top) == 0) {
            next_entry();
            snes_fatal("the game's top-level routine returned");
        }
    }
}

static void frame_done(void)
{
    if (user_frame_hook)
        user_frame_hook();
    /* back to whoever runs the console, until the next frame */
    coro_switch(cpu_co, host_co);
}

void nshvc_set_frame_hook(void (*fn)(void)) { user_frame_hook = fn; }
void nshvc_set_write_hook(void (*fn)(uint32_t addr, uint8_t value)) { snes_write_hook = fn; }

void nshvc_power(const nshvc_cart *cart)
{
    if (powered)
        snes_fatal("nshvc_power: the console is already on (one power-on a process)");
    powered = 1;
    snes_cart = *cart;
    if (!snes_cart.title)
        snes_cart.title = "nativeshvc";
    if (!snes_cart.save_name)
        snes_cart.save_name = snes_cart.title;
    if (!snes_cart.rom && !snes_cart.rom_read)
        snes_fatal("the cartridge has no ROM (rom, or rom_pages and rom_read)");
    if (!snes_cart.map)
        snes_fatal("the cartridge names no mapping (nshvc_cart.map)");
    snes_interrupt_hook = take_interrupt;
    snes_irq_masked = irq_masked;
    snes_spc_run_hook = spc_run;
    snes_frame_hook = frame_done;
    snes_power();
    cpu_co = coro_new(cpu_main, NULL, 16u << 20);
}

void nshvc_run_frame(void)
{
    if (!powered)
        snes_fatal("nshvc_run_frame before nshvc_power");
    host_co = coro_current();
    coro_switch(host_co, cpu_co);
}
