/*
 * chip.c: the cartridge connector (cart.h), checked with a chip of a few
 * lines of C on a board that decodes all of the cartridge's space
 * (NSHVC_CHIP): the board's ROM where the chip says (rom_at), the chip's
 * registers and RAM, its IRQ line, open bus where it answers nothing, and
 * the ROM read for the chip (nshvc_cart_rom) with its own bits passed on.
 *
 *   chip              runs the checks; exit 0 when every one holds
 *   chip --direct     the same with the chip giving its RAM to the S-CPU
 *                     directly while it is not busy (cart.h direct_read,
 *                     direct_write, contract 2.18): its accesses made with
 *                     nothing of the chip's, until the chip says otherwise
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nshvc/nshvc.h"
#include "nshvc/cart.h"
#include "nshvc/machine.h"

static int fails;

static void check(int ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    fails += !ok;
}

/* ---- the cartridge: 64 KB of ROM, read through rom_read ---- */
#define ROM_SIZE 0x10000
static int last_who;

static uint8_t rom_byte(uint32_t off)
{
    /* the vectors at the end of the first 32 KB bank: reset $8000, IRQ $9000 */
    switch (off) {
    case 0x7FD5: return 0x20;
    case 0x7FFC: return 0x00;
    case 0x7FFD: return 0x80;
    case 0x7FEE: case 0x7FFE: return 0x00;
    case 0x7FEF: case 0x7FFF: return 0x90;
    }
    return (uint8_t)(off * 7 + 3);
}

static uint8_t rom_read(uint32_t off, int who)
{
    last_who = who;
    return rom_byte(off);
}

/* ---- the chip: a register at $3000, its IRQ raised by a write to $3001
   and taken back by a read of $3002, 64 KB of RAM at $70 ---- */
static uint8_t reg, ram[0x10000];
static int reads, writes;

static int chip_rom_at(uint32_t addr, uint32_t *off)
{
    uint8_t b = (uint8_t)(addr >> 16) & 0x7F;
    uint16_t o = (uint16_t)addr;
    if (b < 0x40 && o >= 0x8000) {
        *off = (uint32_t)(b & 1) << 15 | (o & 0x7FFF);
        return 1;
    }
    if (b >= 0x40 && b < 0x60) {
        *off = o;
        return 1;
    }
    return 0;
}

static int chip_peek(uint32_t addr, uint8_t *v)
{
    uint8_t b = (uint8_t)(addr >> 16);
    uint16_t o = (uint16_t)addr;
    if (b < 0x40 && o == 0x3000) {
        *v = reg;
        return 1;
    }
    if (b == 0x70) {
        *v = ram[o];
        return 1;
    }
    return 0;
}

static int chip_read(uint32_t addr, uint8_t *v)
{
    uint8_t b = (uint8_t)(addr >> 16);
    uint16_t o = (uint16_t)addr;
    if (b < 0x40 && o == 0x3002) {
        nshvc_cart_irq(0);
        *v = 0x5A;
        reads++;
        return 1;
    }
    if (chip_peek(addr, v)) {
        reads++;
        return 1;
    }
    return 0;
}

static int chip_write(uint32_t addr, uint8_t v)
{
    uint8_t b = (uint8_t)(addr >> 16);
    uint16_t o = (uint16_t)addr;
    if (b < 0x40 && o == 0x3000) {
        reg = v;
        writes++;
        return 1;
    }
    if (b < 0x40 && o == 0x3001) {
        nshvc_cart_irq(1);
        writes++;
        return 1;
    }
    if (b == 0x70) {
        ram[o] = v;
        writes++;
        return 1;
    }
    return 0;
}

static uint8_t *chip_ram(uint32_t *size)
{
    *size = sizeof ram;
    return ram;
}

/* --direct: the RAM at $70 the S-CPU's directly, but while `busy` (the
   chip at work on it: the S-CPU's reads see $EE, its writes are lost) */
static int busy;
static const uint8_t *chip_direct_read(uint32_t addr)
{
    return (uint8_t)(addr >> 16) == 0x70 && !busy ? &ram[addr & 0xE000] : NULL;
}
static uint8_t *chip_direct_write(uint32_t addr)
{
    return (uint8_t)(addr >> 16) == 0x70 && !busy ? &ram[addr & 0xE000] : NULL;
}
static int busy_read(uint32_t addr, uint8_t *v)
{
    if (busy && (uint8_t)(addr >> 16) == 0x70) {
        reads++;
        *v = 0xEE;
        return 1;
    }
    return chip_read(addr, v);
}
static int busy_write(uint32_t addr, uint8_t v)
{
    if (busy && (uint8_t)(addr >> 16) == 0x70) {
        writes++;
        return 1;
    }
    return chip_write(addr, v);
}

static const nshvc_chip toy = {
    .name = "toy",
    .read = chip_read,
    .write = chip_write,
    .peek = chip_peek,
    .rom_at = chip_rom_at,
    .ram = chip_ram,
};

static const nshvc_chip toy_direct = {
    .name = "toy, direct",
    .read = busy_read,
    .write = busy_write,
    .peek = chip_peek,
    .rom_at = chip_rom_at,
    .ram = chip_ram,
    .direct_read = chip_direct_read,
    .direct_write = chip_direct_write,
};
static int direct_mode;

/* ---- the code ---- */
static int irq_taken, irq_ack;

static void abi_irq(void)
{
    irq_taken++;
    irq_ack = rd8(0x003002);
}

static void abi_reset(void)
{
    uint8_t v;
    uint32_t n;

    v = rd8(0x008123);
    check(v == rom_byte(0x0123) && last_who == NSHVC_ROM_CPU, "$00:8123 is the board's ROM at $0123, read by the S-CPU");
    check(rd8(0x018000) == rom_byte(0x8000), "$01:8000 is the board's ROM at $8000");
    check(rd8(0x404567) == rom_byte(0x4567), "$40:4567 is the board's ROM at $4567");
    check(rd8(0xC04567) == rom_byte(0x4567), "$C0:4567 mirrors $40:4567");

    wr8(0x003000, 0x3C);
    check(reg == 0x3C && rd8(0x003000) == 0x3C, "the chip's register: written and read back through the chip");
    wr8(0x701234, 0xA7);
    check(ram[0x1234] == 0xA7 && rd8(0x701234) == 0xA7, "the chip's RAM: written and read back through the chip");
    check(nshvc_sram() == ram && chip_ram(&n) == ram, "the chip's RAM is the cartridge's RAM (nshvc_sram)");

    wr8(0x000010, 0x99);
    v = rd8(0x606000);
    check(v == 0x99, "$60:6000, which neither the board nor the chip answers: open bus (the data bus's last byte)");

    check(nshvc_cart_rom(0x0200, NSHVC_ROM_CHIP | NSHVC_ROM_CHIP_OWN * 3) == rom_byte(0x0200) &&
              last_who == (NSHVC_ROM_CHIP | NSHVC_ROM_CHIP_OWN * 3),
          "the ROM read for the chip: the byte, and who as the chip gave it");

    cpu.p = (uint8_t)(cpu.p & ~P_I);
    wr8(0x003001, 0x01);
    check(irq_taken == 1 && irq_ack == 0x5A, "the chip's IRQ: taken after the write that raised it, acknowledged by the handler");
    rd8(0x000010);
    check(irq_taken == 1, "the IRQ line low again: no second interrupt");
    cpu.p = (uint8_t)(cpu.p | P_I);
    wr8(0x003001, 0x01);
    rd8(0x000010);
    check(irq_taken == 1, "the IRQ masked by the I flag: not taken");
    nshvc_cart_irq(0);

    if (direct_mode) {
        int r0, w0, i, same = 1;
        /* the first access the chip's; then the 8 KB its directly */
        rd8(0x702000);
        r0 = reads;
        w0 = writes;
        for (i = 0; i < 0x2000; i++)
            wr8(0x702000 + (uint32_t)i, (uint8_t)(i * 13));
        for (i = 0; i < 0x2000; i++)
            same &= rd8(0x702000 + (uint32_t)i) == (uint8_t)(i * 13) && ram[0x2000 + i] == (uint8_t)(i * 13);
        check(same && reads == r0 && writes <= w0 + 1,
              "direct: 8,192 writes and reads of the chip's RAM made with nothing of the chip's, the RAM its own");
        /* the chip at work: its answers no longer stand, the S-CPU's
           accesses its again */
        busy = 1;
        nshvc_cart_direct_changed();
        r0 = reads;
        check(rd8(0x702345) == 0xEE && reads == r0 + 1, "direct: the chip busy (its answers changed): the read the chip's");
        wr8(0x702345, 0x11);
        check(ram[0x2345] != 0x11, "direct: and the write the chip's (lost while it is busy)");
        busy = 0;
        nshvc_cart_direct_changed();
        check(rd8(0x702345) == (uint8_t)(0x345 * 13), "direct: the chip done: the RAM read again");
        r0 = reads;
        check(rd8(0x702346) == (uint8_t)(0x346 * 13) && reads == r0, "direct: and directly again after that");
        /* the console's own registers never directly, though the chip's
           answers would give them */
        r0 = reads;
        rd8(0x003000);
        rd8(0x003000);
        check(reads == r0 + 2, "direct: the chip's register at $3000 still the chip's");
    }

    printf("%d check(s) failed\n", fails);
    exit(fails ? 1 : 0);
}

static const nshvc_routine routines[] = {
    { 0x008000, abi_reset, "reset", 0 },
    { 0x009000, abi_irq, "irq", 0 },
};

int main(int argc, char **argv)
{
    nshvc_cart cart;
    direct_mode = argc > 1 && !strcmp(argv[1], "--direct");
    memset(&cart, 0, sizeof cart);
    cart.title = "chip";
    cart.map = NSHVC_CHIP;
    cart.rom_size = ROM_SIZE;
    cart.rom_read = rom_read;
    cart.sram_size = sizeof ram;
    cart.chip = direct_mode ? &toy_direct : &toy;
    cart.routines = routines;
    cart.routine_count = 2;
    nshvc_power(&cart);
    for (;;)
        nshvc_run_frame();
}
