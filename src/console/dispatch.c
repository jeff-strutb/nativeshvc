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
static int in_interrupt;
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

/* one cycle of the CPU's the console states for the C: `cc` clocks */
static void cpu_cycle(unsigned cc) { snes_cpu_cycle_k(cc); }

/* the clocks the fetch of a code byte at `at` takes */
static unsigned fetch(uint32_t at) { return snes_access_clocks(at); }

void cpu_loop_forever(uint32_t at)
{
    in_interrupt = 0;
    snes_in_interrupt = 0;
    if (snes_untimed())
        snes_fatal("untimed, the S-CPU loops for ever at $%06X: it waits for an interrupt that cannot come",
                   (unsigned)at);
    for (;;) {
        /* BRA to itself: its opcode and operand fetched, an internal cycle */
        cpu_cycle(fetch(at)); cpu_cycle(fetch(at + 1)); cpu_cycle(6);
        snes_cpu_boundary();
    }
}

void cpu_idle(uint32_t clocks) { snes_cpu_idle(clocks); }

void cpu_wai(uint32_t at)
{
    /* WAI: the opcode, then internal cycles; one that begins with an
       interrupt due (an NMI, or the IRQ line, masked or not) is the last
       but one, and the interrupt is taken after the next */
    cpu_cycle(fetch(at));
    while (!snes_interrupt_due())
        cpu_cycle(6);
    cpu_cycle(6); cpu_cycle(6);
    snes_cpu_boundary();
}
void cyc(uint32_t clocks) { snes_cpu_stated(clocks, __builtin_return_address(0)); }
void nshvc_mark(uint32_t id) { snes_cyclog_mark(id); }

void cycs(uint64_t lengths)
{
    int shift = 60;
    snes_cyclog_cycs(__builtin_return_address(0), lengths);
    while (shift >= 0 && !((lengths >> shift) & 0xF))
        shift -= 4;
    for (; shift >= 0; shift -= 4) {
        unsigned c = (unsigned)(lengths >> shift) & 0xF;
        if (c != 6 && c != 8 && c != 12)
            snes_fatal("cycs(0x%llx): a cycle of %u clocks (6, 8 or C for 12), from %p", (unsigned long long)lengths, c,
                       __builtin_return_address(0));
        snes_cpu_cycle_c(c);
    }
}
void cpu_implied(uint32_t at)
{
    /* with an interrupt to be taken, the internal cycle is a read at the
       next opcode instead */
    cpu_cycle(snes_interrupt_taken_now() ? fetch(at + 1) : 6);
    snes_c_between();
}

void cpu_restart(uint32_t at, void (*entry)(void))
{
    (void)at;
    next_entry = entry;
    in_interrupt = 0;
    snes_in_interrupt = 0;
    /* untimed is the work of the calls the restart drops: it ends with them */
    nshvc_set_untimed(0);
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

/* RTI: its fetch, two internal cycles, P and PC (and PB in native mode)
   pulled */
static void rti(uint32_t at)
{
    cpu_cycle(fetch(at)); cpu_cycle(6); cpu_cycle(6);
    pull8();
    pull8();
    pull8();
    if (!cpu.e)
        pull8();
}

/* The interrupt: the sequence the CPU runs (two internal cycles, the frame
   it pushes, the vector read, I set and D clear), then the vector's code.
   The handler's C runs with the frame pushed and the registers as the
   interrupted code left them; it states its time up to its RTI, whose
   pulls are made here. The registers are put back after, as the handler's
   own pushes and pulls and RTI leave them on the console. */
static void take_interrupt(int nmi)
{
    Regs65816 saved = cpu;
    uint32_t vector = cpu.e ? (nmi ? 0x00FFFA : 0x00FFFE) : (nmi ? 0x00FFEA : 0x00FFEE);
    uint32_t at = (uint16_t)(snes_peek(vector) | snes_peek(vector + 1) << 8);
    const nshvc_routine *r;
    /* a read at the next opcode, an internal cycle */
    cpu_cycle(fetch(cpu.pc)); cpu_cycle(6);
    if (!cpu.e)
        push8((uint8_t)(cpu.pc >> 16));
    push8((uint8_t)(cpu.pc >> 8));
    push8((uint8_t)cpu.pc);
    push8(cpu.p);
    /* I set and D cleared before the vector's read */
    cpu.p = (uint8_t)((cpu.p | P_I) & ~P_D);
    cpu_cycle(fetch(vector)); cpu_cycle(fetch(vector + 1));
    cpu.pc = at;
    cpu.db = saved.db;
    in_interrupt++;
    snes_in_interrupt++;
    r = find(at);
    if (!r)
        snes_fatal("interrupt vector $%04X: no routine with C starts at $00:%04X (the code there needs C, as any "
                   "other code the CPU runs)", (unsigned)(vector & 0xFFFF), (unsigned)at);
    r->abi();
    if (cpu.unwound)
        snes_fatal("the interrupt handler at $00:%04X left through its caller's frame", (unsigned)at);
    rti(at);
    in_interrupt--;
    snes_in_interrupt--;
    cpu = saved;
}

static int irq_masked(void) { return (cpu.p & P_I) != 0; }

/* ---- unit calls (snes.h SnesUnit; the script's call) ---- */
int snes_unit_pending;
static SnesUnit unit;
static int unit_state;
static uint32_t unit_stub, unit_loop;      /* the stub's address, its loop's */
static uint32_t unit_wram;                 /* the stub's offset in work RAM */
static uint8_t unit_code[32];
static int unit_len;
static Regs65816 unit_regs;                /* the registers at the command, the call's own over them */

int snes_unit_state(void) { return unit_state; }

void snes_unit_cancel(void)
{
    if (unit_state == 1) {
        unit_state = 0;
        snes_unit_pending = 0;
    }
}

/* The stub, as the reference's harness writes it (stuntracefx's sfxbox):
   JSR (then NOP) or JSL to the routine, or to a trampoline at +6 that
   pushes what frame=, php or phb name and jumps there; rti= pushes the
   frame an interrupt pushes and jumps (JML); then BRA to itself. */
void snes_unit_call(const SnesUnit *u)
{
    uint32_t addr = u->addr & 0xFFFFFF, bank = addr >> 16, at = 0, sb = 0;
    uint8_t *b = unit_code;
    int m = 0;
    if (unit_state == 1 || unit_state == 2)
        snes_fatal("a unit call while one is under way");
    if (u->lng || u->rti >= 0) {
        at = 0x1FE0;
        sb = 0x00;
    } else if (bank == 0x7F) {
        at = 0x1FFE0;                      /* $7F:FFE0: past the code the game copies to $7F:0800 */
        sb = 0x7F;
    } else if (bank == 0x7E || bank < 0x40 || (bank >= 0x80 && bank < 0xC0)) {
        at = 0x1FE0;
        sb = bank;
    } else {
        snes_fatal("call: a JSR cannot reach bank %02X from work RAM; use long", (unsigned)bank);
    }
    if (u->php && u->lng)
        snes_fatal("call: php is for a JSR call (frame= for long)");
    if (u->phb && !u->lng)
        snes_fatal("call: phb is for a JSL call (long)");
    unit = *u;
    if (u->rti >= 0) {
        uint16_t l = (uint16_t)(at + 10);
        b[m++] = 0xF4; b[m++] = (uint8_t)(l >> 8); b[m++] = (uint8_t)sb;
        b[m++] = 0xF4; b[m++] = (uint8_t)u->rti; b[m++] = (uint8_t)l;
        b[m++] = 0x5C; b[m++] = (uint8_t)addr; b[m++] = (uint8_t)(addr >> 8); b[m++] = (uint8_t)bank;
    } else if ((u->frame >= 0 && u->lng) || u->phb) {
        uint16_t t = (uint16_t)(at + 6);
        b[m++] = 0x22; b[m++] = (uint8_t)t; b[m++] = (uint8_t)(t >> 8); b[m++] = (uint8_t)sb;
    } else if (u->frame >= 0 || u->php) {
        uint16_t t = (uint16_t)(at + 6);
        b[m++] = 0x20; b[m++] = (uint8_t)t; b[m++] = (uint8_t)(t >> 8); b[m++] = 0xEA;
    } else if (u->lng) {
        b[m++] = 0x22; b[m++] = (uint8_t)addr; b[m++] = (uint8_t)(addr >> 8); b[m++] = (uint8_t)bank;
    } else {
        b[m++] = 0x20; b[m++] = (uint8_t)addr; b[m++] = (uint8_t)(addr >> 8); b[m++] = 0xEA;
    }
    b[m++] = 0x80; b[m++] = 0xFE;
    unit_stub = sb << 16 | (at & 0xFFFF);
    unit_loop = unit_stub + (uint32_t)(m - 2);
    if (u->frame >= 0) {
        b[m++] = 0xF4; b[m++] = (uint8_t)u->frame; b[m++] = (uint8_t)(u->frame >> 8);
        if (u->lng) {
            b[m++] = 0x5C; b[m++] = (uint8_t)addr; b[m++] = (uint8_t)(addr >> 8); b[m++] = (uint8_t)bank;
        } else {
            b[m++] = 0x4C; b[m++] = (uint8_t)addr; b[m++] = (uint8_t)(addr >> 8);
        }
    }
    if (u->phb) {
        b[m++] = 0x8B;
        b[m++] = 0x5C; b[m++] = (uint8_t)addr; b[m++] = (uint8_t)(addr >> 8); b[m++] = (uint8_t)bank;
    }
    if (u->php) {
        b[m++] = 0x08;
        b[m++] = 0x4C; b[m++] = (uint8_t)addr; b[m++] = (uint8_t)(addr >> 8);
    }
    unit_len = m;
    unit_wram = at;
    /* as the reference's harness: the stub written now (it stays when the
       machine is put back), the registers the call does not name as they
       are now */
    memcpy(snes_wram_rw() + unit_wram, unit_code, (size_t)unit_len);
    unit_regs = cpu;
    unit_state = 1;
    snes_unit_pending = 1;
}

/* the stub's pushes and pulls, inside its instructions */
static void unit_push(uint8_t v)
{
    snes_cpu_write_within((uint16_t)cpu.s, v);
    cpu.s = (uint16_t)(cpu.s - 1);
}

static uint8_t unit_pull(void)
{
    cpu.s = (uint16_t)(cpu.s + 1);
    return snes_cpu_read_within((uint16_t)cpu.s);
}

/* the start of one of the stub's instructions (the console's own, at a
   known address): where the CPU takes an interrupt */
static void unit_insn(uint32_t at)
{
    cpu.pc = at;
    snes_cpu_boundary();
}

/* the routine's C, entered from the stub; `pulls`, its return's (2 RTS, 3
   RTL), made here (its C states the return's time but for them); 0 for
   RTI, whose C makes all */
static void unit_enter(uint32_t t, int pulls)
{
    const nshvc_routine *r = find(t);
    int k;
    if (!r)
        runaway("calls", t);
    cpu.pc = t;
    r->abi();
    if (cpu.unwound)
        snes_fatal("unit call: the routine at $%06X left through its caller's frame", (unsigned)t);
    for (k = 0; k < pulls; k++)
        unit_pull();
}

/* What the stub snes_unit_call wrote does, from its start to its loop, as
   the CPU does it: the stub is the console's own code, one of the few
   forms snes_unit_call writes, so the console carries out that form (its
   fetches' time, its internal cycles, its pushes, the routine's C) with
   no look at its bytes. */
static void unit_run(void)
{
    uint32_t at = unit_stub, pb = at & 0xFF0000, addr = unit.addr & 0xFFFFFF;
    uint32_t t = pb | (uint16_t)(at + 6);
    int tramp = unit.frame >= 0 || unit.php || unit.phb;
    int jsl = (unit.frame >= 0 && unit.lng) || unit.phb || (unit.lng && !tramp);
#define AT(k) (pb | (uint16_t)(at + (k)))
#define F(a) cpu_cycle(fetch(a))
    if (unit.rti >= 0) {
        uint16_t l = (uint16_t)(at + 10);
        /* PEA, PEA: the frame an interrupt pushes, returning to the loop */
        unit_insn(AT(0));
        F(AT(0)); F(AT(1)); F(AT(2));
        unit_push((uint8_t)(pb >> 16));
        unit_push((uint8_t)(l >> 8));
        unit_insn(AT(3));
        F(AT(3)); F(AT(4)); F(AT(5));
        unit_push((uint8_t)l);
        unit_push((uint8_t)unit.rti);
        /* JML to the routine, which returns by RTI */
        unit_insn(AT(6));
        F(AT(6)); F(AT(7)); F(AT(8)); F(AT(9));
        unit_enter(addr, 0);
        return;
    }
    /* JSL or JSR to the routine, or to the trampoline at +6 */
    unit_insn(AT(0));
    if (jsl) {
        F(AT(0)); F(AT(1)); F(AT(2));
        unit_push((uint8_t)(pb >> 16));
        cpu_cycle(6); F(AT(3));
        unit_push((uint8_t)((at + 3) >> 8));
        unit_push((uint8_t)(at + 3));
    } else {
        F(AT(0)); F(AT(1)); F(AT(2)); cpu_cycle(6);
        unit_push((uint8_t)((at + 2) >> 8));
        unit_push((uint8_t)(at + 2));
    }
    if (tramp) {
        unit_insn(t);
        if (unit.frame >= 0) {
            /* PEA frame, then JML or JMP to the routine */
            F(t); F(t + 1); F(t + 2);
            unit_push((uint8_t)(unit.frame >> 8));
            unit_push((uint8_t)unit.frame);
            unit_insn(t + 3);
            F(t + 3); F(t + 4); F(t + 5);
            if (unit.lng)
                F(t + 6);
        } else {
            /* PHB then JML, or PHP then JMP */
            F(t); cpu_cycle(6);
            unit_push(unit.phb ? cpu.db : cpu.p);
            unit_insn(t + 1);
            F(t + 1); F(t + 2); F(t + 3);
            if (unit.phb)
                F(t + 4);
        }
    }
    unit_enter(addr, jsl ? 3 : 2);
    if (!jsl) {
        /* the NOP after the JSR */
        unit_insn(AT(3));
        F(AT(3)); cpu_cycle(6);
    }
#undef AT
#undef F
}

void snes_unit_start(void)
{
    Regs65816 from;
    if (unit_state != 1) {
        snes_unit_pending = 0;
        return;
    }
    if (in_interrupt || snes_untimed())
        return;
    if (snes_cart.chip && snes_cart.chip->busy && snes_cart.chip->busy())
        return;
    if (!unit.vblank && snes_vcounter() >= snes_vblank_line())
        return;
    snes_unit_pending = 0;
    unit_state = 2;
    from = cpu;
    if (unit.apply)
        unit.apply();
    cpu = unit_regs;
    cpu.s = from.s;
    if (unit.has_a) cpu.a = unit.a;
    if (unit.has_x) cpu.x = unit.x;
    if (unit.has_y) cpu.y = unit.y;
    if (unit.has_p) cpu.p = unit.p;
    if (unit.has_db) cpu.db = unit.db;
    if (unit.has_d) cpu.d = unit.d;
    if (cpu.p & P_X) {
        cpu.x &= 0xFF;
        cpu.y &= 0xFF;
    }
    cpu.e = 0;
    cpu.unwound = 0;
    unit_run();
    unit_state = 3;
    if (unit.keep) {
        cpu = from;                          /* the game goes on from where it was */
        return;
    }
    /* parked in the stub's loop (BRA to itself) until the script ends this
       run (the machine put back as it was before the call) */
    cpu.pc = unit_loop;
    for (;;) {
        cpu_cycle(fetch(unit_loop)); cpu_cycle(fetch(unit_loop + 1)); cpu_cycle(6);
        snes_cpu_boundary();
    }
}

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
/* ---- SPC700 unit calls (snes.h snes_spc_unit_call; spc.h spc_at) ---- */
#define SPC_STUB 0x01F0
static struct {
    int state;                               /* 1 waiting for `at`, 2 under way */
    uint16_t target, at;
    int has[4];
    uint8_t reg[4];
    int nset;
    uint16_t set_at[256];
    uint8_t set_v[256];
} su;

void snes_spc_unit_set(uint16_t addr, uint8_t v)
{
    if (su.nset == 256)
        snes_fatal("spcset: more than 256 bytes for one spccall");
    su.set_at[su.nset] = addr;
    su.set_v[su.nset++] = v;
}

void snes_spc_unit_call(uint16_t target, uint16_t at, const int *has, const uint8_t *reg)
{
    int i;
    if (su.state)
        snes_fatal("spccall: the previous one has not run yet");
    su.target = target;
    su.at = at;
    for (i = 0; i < 4; i++) {
        su.has[i] = has[i];
        su.reg[i] = reg[i];
    }
    su.state = 1;
}

int snes_spc_unit_busy(void) { return su.state; }

void spc_at(uint16_t at)
{
    uint8_t *aram, kept[5];
    SpcRegs saved;
    void (*fn)(void);
    int i;
    if (su.state != 1 || at != su.at)
        return;
    fn = spc_find(su.target);
    if (!fn)
        snes_fatal("spccall: SPC700 code at $%04X: no routine with C matches the APU's RAM", su.target);
    su.state = 2;
    saved = spc;
    aram = apu_aram_rw();
    memcpy(kept, aram + SPC_STUB, 5);
    aram[SPC_STUB] = 0x3F;                   /* CALL target */
    aram[SPC_STUB + 1] = (uint8_t)su.target;
    aram[SPC_STUB + 2] = (uint8_t)(su.target >> 8);
    aram[SPC_STUB + 3] = 0x2F;               /* BRA $ */
    aram[SPC_STUB + 4] = 0xFE;
    for (i = 0; i < su.nset; i++)
        aram[su.set_at[i]] = su.set_v[i];
    su.nset = 0;
    if (su.has[0]) spc.a = su.reg[0];
    if (su.has[1]) spc.x = su.reg[1];
    if (su.has[2]) spc.y = su.reg[2];
    if (su.has[3]) spc_set_psw(su.reg[3]);
    /* CALL: its fetches and idle cycle, the pushes, two idle cycles */
    spc_call(fn, SPC_STUB + 3, 5, 1, 2);
    memcpy(aram + SPC_STUB, kept, 5);
    spc = saved;
    su.state = 0;
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
