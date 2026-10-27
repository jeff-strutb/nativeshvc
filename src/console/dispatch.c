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
 *                                code: a routine with C that starts there,
 *                                run as the handler and returned from by
 *                                RTI; or, where none starts, the code
 *                                there as the CPU runs it, for the short
 *                                stubs games keep in RAM or ROM (NOPs, a
 *                                JMP, JML or JSL to a routine with C, RTI)
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
        snes_cpu_interrupts();
    }
}

void cpu_idle(uint32_t clocks) { snes_cpu_wait(clocks); }

void cpu_wai(uint32_t at)
{
    /* WAI: the opcode, then internal cycles; one that begins with an
       interrupt due (an NMI, or the IRQ line, masked or not) is the last
       but one, and the interrupt is taken after the next */
    cpu_cycle(fetch(at));
    while (!snes_interrupt_due())
        cpu_cycle(6);
    cpu_cycle(6); cpu_cycle(6);
    snes_cpu_interrupts();
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
        snes_cpu_cycle(c);
    }
}
void cpu_implied(uint32_t at)
{
    /* with an interrupt to be taken, the internal cycle is a read at the
       next opcode instead */
    cpu_cycle(snes_interrupt_taken_now() ? fetch(at + 1) : 6);
}
void cpu_boundary(void) { snes_cpu_boundary(); }

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
    return rd8((uint16_t)cpu.s);
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
    int steps;
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
    if (r) {
        r->abi();
        if (cpu.unwound)
            snes_fatal("the interrupt handler at $00:%04X left through its caller's frame", (unsigned)at);
        rti(at);
        in_interrupt--;
        snes_in_interrupt--;
        cpu = saved;
        return;
    }
    /* a stub: run as the CPU runs it */
    for (steps = 0; steps < 64; steps++) {
        uint8_t op = snes_peek(at);
        if (op == 0xEA) {                       /* NOP: fetch, internal cycle */
            cpu_cycle(fetch(at)); cpu_cycle(6);
            at = (at & 0xFF0000) | (uint16_t)(at + 1);
        } else if (op == 0x4C) {                /* JMP abs: in the same bank */
            uint32_t t = (at & 0xFF0000) | snes_peek(at + 1) | snes_peek(at + 2) << 8;
            cpu_cycle(fetch(at)); cpu_cycle(fetch(at + 1)); cpu_cycle(fetch(at + 2));
            r = find(t);
            if (r) {
                r->abi();
                if (cpu.unwound)
                    snes_fatal("an interrupt handler left through its caller's frame");
                rti(t);
                break;
            }
            at = t;
        } else if (op == 0x5C) {                /* JML long */
            uint32_t t = snes_peek(at + 1) | snes_peek(at + 2) << 8 | (uint32_t)snes_peek(at + 3) << 16;
            cpu_cycle(fetch(at)); cpu_cycle(fetch(at + 1)); cpu_cycle(fetch(at + 2)); cpu_cycle(fetch(at + 3));
            r = find(t);
            if (r) {
                /* the handler's C states its own RTI and makes its pulls */
                r->abi();
                break;
            }
            at = t;
        } else if (op == 0x22) {                /* JSL to the handler */
            uint32_t t = snes_peek(at + 1) | snes_peek(at + 2) << 8 | (uint32_t)snes_peek(at + 3) << 16;
            uint16_t ret = (uint16_t)(at + 3);
            r = find(t);
            if (!r)
                runaway("takes an interrupt to", t);
            /* JSL: opcode and address fetched, PB pushed, an internal cycle,
               the bank fetched, PC pushed; the handler's C states its RTL */
            cpu_cycle(fetch(at)); cpu_cycle(fetch(at + 1)); cpu_cycle(fetch(at + 2));
            push8((uint8_t)(at >> 16));
            cpu_cycle(6); cpu_cycle(fetch(at + 3));
            push8((uint8_t)(ret >> 8));
            push8((uint8_t)ret);
            r->abi();
            if (cpu.unwound)
                snes_fatal("an interrupt handler left through its caller's frame");
            pull8();
            pull8();
            pull8();
            at = (at & 0xFF0000) | (uint16_t)(at + 4);
        } else if (op == 0x40) {                /* RTI */
            rti(at);
            break;
        } else {
            snes_fatal("interrupt code at $%02X:%04X: opcode %02X, which nativeshvc does not run "
                       "(give the routine there C)", (unsigned)(at >> 16), (unsigned)(at & 0xFFFF), op);
        }
    }
    in_interrupt--;
    snes_in_interrupt--;
    cpu = saved;
}

static int irq_masked(void) { return (cpu.p & P_I) != 0; }

/* ---- unit calls (snes.h SnesUnit; the script's call and fx) ---- */
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

static int in_wram(uint32_t a)
{
    uint8_t bank = (uint8_t)(a >> 16);
    return bank == 0x7E || bank == 0x7F || ((bank & 0x7F) < 0x40 && (a & 0xFFFF) < 0x2000);
}

/* the console's run of the stub's code (and code it reaches in work RAM
   that has no C: fx's program) from `at` to the stub's loop: each
   instruction's cycles as the CPU makes them, its fetches (no effect but
   their time), internal cycles and data accesses; a routine with C it
   enters runs its C, whose return (RTS, RTL: its C states the time but for
   the pulls, made here; RTI: its C makes all) lands on the call's next
   instruction */
static void unit_run(uint32_t at)
{
    struct { uint32_t next; int pulls; } fr[16];
    int depth = 0;
    if (unit.rti >= 0) {
        fr[0].next = unit_loop;
        fr[0].pulls = 0;
        depth = 1;
    }
    while (at != unit_loop) {
        uint32_t pb = at & 0xFF0000, t = 0;
        uint8_t op = snes_peek(at);
        int call = 0, word = !(cpu.p & P_M);
        cpu.pc = at;
        snes_cpu_interrupts();
#define F(k) cpu_cycle(fetch(pb | (uint16_t)(at + (k))))
#define B(k) snes_peek(pb | (uint16_t)(at + (k)))
#define NEXT(n) (at = pb | (uint16_t)(at + (n)))
        switch (op) {
        case 0x20:                           /* JSR abs */
            F(0); F(1); F(2); cpu_cycle(6);
            unit_push((uint8_t)((at + 2) >> 8));
            unit_push((uint8_t)(at + 2));
            t = pb | B(1) | B(2) << 8;
            call = 2;
            break;
        case 0x22:                           /* JSL long */
            F(0); F(1); F(2);
            unit_push((uint8_t)(at >> 16));
            cpu_cycle(6); F(3);
            unit_push((uint8_t)((at + 3) >> 8));
            unit_push((uint8_t)(at + 3));
            t = B(1) | B(2) << 8 | (uint32_t)B(3) << 16;
            call = 3;
            break;
        case 0x4C:                           /* JMP abs */
            F(0); F(1); F(2);
            t = pb | B(1) | B(2) << 8;
            call = 1;
            break;
        case 0x5C:                           /* JML long */
            F(0); F(1); F(2); F(3);
            t = B(1) | B(2) << 8 | (uint32_t)B(3) << 16;
            call = 1;
            break;
        case 0xF4:                           /* PEA */
            F(0); F(1); F(2);
            unit_push(B(2));
            unit_push(B(1));
            NEXT(3);
            break;
        case 0x08: case 0x8B:                /* PHP, PHB */
            F(0); cpu_cycle(6);
            unit_push(op == 0x08 ? cpu.p : cpu.db);
            NEXT(1);
            break;
        case 0xEA:                           /* NOP */
            F(0); cpu_cycle(6);
            NEXT(1);
            break;
        case 0x60: case 0x6B: {              /* RTS, RTL */
            uint32_t r;
            F(0); cpu_cycle(6); cpu_cycle(6);
            r = unit_pull();
            r |= (uint32_t)unit_pull() << 8;
            if (op == 0x6B)
                r |= (uint32_t)unit_pull() << 16;
            else
                cpu_cycle(6);
            if (!depth || fr[depth - 1].pulls != (op == 0x60 ? 2 : 3))
                snes_fatal("unit call: an %s at $%06X with no %s under way", op == 0x60 ? "RTS" : "RTL",
                           (unsigned)at, op == 0x60 ? "JSR" : "JSL");
            depth--;
            at = (op == 0x6B ? (r & 0xFF0000) : pb) | (uint16_t)(r + 1);
            break;
        }
        case 0xC2: case 0xE2:                /* REP, SEP # */
            F(0); F(1); cpu_cycle(6);
            cpu.p = op == 0xC2 ? (uint8_t)(cpu.p & ~B(1)) : (uint8_t)(cpu.p | B(1));
            if (cpu.p & P_X) {
                cpu.x &= 0xFF;
                cpu.y &= 0xFF;
            }
            NEXT(2);
            break;
        case 0xA9: case 0x09: case 0x29: {   /* LDA, ORA, AND # */
            uint16_t v = B(1);
            F(0); F(1);
            if (word) {
                F(2);
                v |= (uint16_t)(B(2) << 8);
            }
            if (op == 0x09)
                v |= cpu.a;
            else if (op == 0x29)
                v &= cpu.a;
            cpu.a = word ? v : (uint16_t)((cpu.a & 0xFF00) | (v & 0xFF));
            cpu.p = word ? nz16(cpu.p, v) : nz8(cpu.p, (uint8_t)v);
            NEXT(word ? 3 : 2);
            break;
        }
        case 0xAD: {                         /* LDA abs */
            uint32_t d = (uint32_t)cpu.db << 16 | B(1) | B(2) << 8;
            uint16_t v;
            F(0); F(1); F(2);
            v = snes_cpu_read_within(d);
            if (word)
                v |= (uint16_t)(snes_cpu_read_within((d + 1) & 0xFFFFFF) << 8);
            cpu.a = word ? v : (uint16_t)((cpu.a & 0xFF00) | v);
            cpu.p = word ? nz16(cpu.p, v) : nz8(cpu.p, (uint8_t)v);
            NEXT(3);
            break;
        }
        case 0x8D: case 0x9C: {              /* STA, STZ abs */
            uint32_t d = (uint32_t)cpu.db << 16 | B(1) | B(2) << 8;
            uint16_t v = op == 0x8D ? cpu.a : 0;
            F(0); F(1); F(2);
            snes_cpu_write_within(d, (uint8_t)v);
            if (word)
                snes_cpu_write_within((d + 1) & 0xFFFFFF, (uint8_t)(v >> 8));
            NEXT(3);
            break;
        }
        case 0xD0:                           /* BNE */
            F(0); F(1);
            if (!(cpu.p & P_Z)) {
                cpu_cycle(6);
                NEXT(2 + (int8_t)B(1));
            } else {
                NEXT(2);
            }
            break;
        default:
            snes_fatal("unit call: opcode %02X at $%06X, which the console's stub runner does not run", op,
                       (unsigned)at);
        }
#undef F
#undef B
#undef NEXT
        if (!call)
            continue;
        if (call > 1) {
            if (depth == 16)
                snes_fatal("unit call: calls nested too deep");
            fr[depth].next = (at & 0xFF0000) | (uint16_t)(at + (call == 2 ? 3 : 4));
            fr[depth++].pulls = call;
        }
        {
            const nshvc_routine *r = find(t);
            if (!r) {
                if (!in_wram(t))
                    runaway("calls", t);
                at = t;                      /* code in work RAM with no C: run here */
                continue;
            }
            cpu.pc = t;
            r->abi();
            if (cpu.unwound)
                snes_fatal("unit call: the routine at $%06X left through its caller's frame", (unsigned)t);
            if (!depth)
                snes_fatal("unit call: the routine at $%06X returned with no call under way", (unsigned)t);
            depth--;
            if (fr[depth].pulls == 3) {
                unit_pull();
                unit_pull();
                unit_pull();
            } else if (fr[depth].pulls == 2) {
                unit_pull();
                unit_pull();
            }
            at = fr[depth].next;
        }
    }
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
    unit_run(unit_stub);
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
        snes_cpu_interrupts();
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
