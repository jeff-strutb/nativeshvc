/*
 * superfx.c: the Super FX (GSU-1), an expansion chip on the cartridge
 * (superfx.h's nshvc_superfx, through the connector: cart.h): its registers
 * as the S-CPU sees them ($3000-$32FF), its RAM, and the bus and plotting
 * its C uses (gsu.h).
 *
 * A program starts when the S-CPU writes R15's high byte: its C (the
 * cartridge's, at PBR:R15: nshvc_cart.gsu_run or its routines) runs on the Super FX's own
 * coroutine and returns where the program STOPs; then GO clears and, unless
 * CFGR masks it, the Super FX's IRQ rises. Time: the Super FX keeps its own
 * clock, which moves on as the GSU-1's does (gsu_clock.c): by the original's
 * code fetches the C states (gsu_code: the code cache, its fills, ROM), its
 * loads and stores (the RAM buffer), R14 writes and ROM-buffer reads,
 * MULT and FMULT, the pixel caches' flushes, and the waits for the bus while
 * the S-CPU has it (SCMR's RON and RAN clear). The S-CPU brings it up to its
 * own time (gsu_sync) before it touches the Super FX's registers or RAM and
 * once a line, and the Super FX hands back when it gets there. So what the
 * S-CPU sees of a running program (its status, its RAM, its interrupt)
 * appears at the time the program reaches it, as on the cartridge.
 *
 * PLOT and RPIX go through the two pixel caches as on the hardware: a row
 * of 8 pixels reaches the RAM's bitplanes when it is flushed.
 */
#include <string.h>

#include "nshvc/superfx.h"
#include "nshvc/gsu.h"
#include "gsu_clock.h"

RegsGSU gsu;
static uint32_t gsu_ram_size = 0x10000;
static const nshvc_cart *cart;           /* the cartridge it is on */

static uint32_t fetch_at;                /* where the ROM buffer was last filled from */

/* the game pak RAM: the GSU's 32 to 128 KB at $70-$71, or, a cartridge
   that asks for more (nshvc_cart.sram_size), up to 14 banks, $70-$7D */
#define GSU_RAM_MAX 0xE0000
static uint8_t ram_store[GSU_RAM_MAX];
/* nshvc_ext_gsu_screen's: the screen's width and height (0: the GSU's own) */
static uint16_t ext_w, ext_h;

/* a byte of the game pak RAM at bank:address, $70 on: the GSU's own
   32-128 KB mirrored through $70-$71, more RAM in the banks after */
static uint8_t *ram_byte(uint32_t addr)
{
    uint32_t off = (uint32_t)(((addr >> 16) & 0xFF) - 0x70) << 16 | (uint16_t)addr;
    if (gsu_ram_size <= 0x20000)
        return &ram_store[(off & 0x1FFFF) & (gsu_ram_size - 1)];
    return &ram_store[off < gsu_ram_size ? off : off % gsu_ram_size];
}
static uint8_t cache[0x200];
static uint8_t pbr, cfgr, clsr, bramr, cbr_hi, cbr_lo;
static uint8_t reg_latch;
static uint8_t sfr_irq, sfr_alt, sfr_b;

static int go, pending;
/* the store in the RAM buffer (gsu_wr8) */
static uint32_t store_addr;
static uint8_t store_value;
static int store_pending;

static uint64_t gsu_now, gsu_target;   /* master clocks */
static GsuClock clk;
/* The cartridge clocks the Super FX from a crystal of its own, 21.44 MHz,
   not the console's 21.477 MHz: the model's clocks (clk.now) are the Super
   FX's, turned into the console's at the rate between them. */
#define GSU_HZ 21440000u
#define MASTER_HZ 21477273u
static uint64_t run_master, run_clk;   /* a program's start: the console's clock, the model's */
static nshvc_coro *gsu_co, *main_co;   /* the Super FX's, and the one that last switched to it */

static void gsu_main(void *arg);
static inline uint8_t bus_byte(uint32_t addr);
uint8_t gsu_fetch(uint32_t addr);
static void bus_wait(GsuClock *c, int ram);
static uint8_t pix_read(GsuClock *c, uint32_t addr);
static void pix_write(GsuClock *c, uint32_t addr, uint8_t v);
static void store_made(GsuClock *c);
uint8_t gsu_rd8(uint32_t addr);
void gsu_wr8(uint32_t addr, uint8_t value);
void gsu_plot(uint8_t x, uint8_t y);
uint8_t gsu_rpix(uint8_t x, uint8_t y);

static void power(const nshvc_cart *c)
{
    cart = c;
    memset(&gsu, 0, sizeof gsu);
    /* a cartridge with no save: its RAM reads $FF throughout, as the
       verification harness powers the original on */
    memset(ram_store, 0xFF, sizeof ram_store);
    memset(cache, 0, sizeof cache);
    pbr = cfgr = clsr = bramr = cbr_hi = cbr_lo = 0;
    reg_latch = 0;
    sfr_irq = sfr_alt = sfr_b = 0;
    go = pending = 0;
    gsu_now = gsu_target = 0;
    memset(&clk, 0, sizeof clk);
    clk.bus = bus_wait;
    clk.pix_read = pix_read;
    clk.pix_write = pix_write;
    clk.ram_store = store_made;
    store_pending = 0;
    run_master = run_clk = 0;
    if (c->sram_size > 0x20000) {
        if (c->sram_size > GSU_RAM_MAX || c->sram_size % 0x10000)
            nshvc_fatal("the Super FX's RAM: up to %u KB, in 64 KB banks (sram_size %u)", GSU_RAM_MAX / 1024,
                        (unsigned)c->sram_size);
        gsu_ram_size = c->sram_size;
    } else {
        gsu_ram_size = c->sram_size >= 0x8000
                     ? (c->sram_size >= 0x20000 ? 0x20000 : c->sram_size >= 0x10000 ? 0x10000 : 0x8000)
                     : 0x10000;
    }
    ext_w = ext_h = 0;
    if (!gsu_co)
        gsu_co = nshvc_coro_new(gsu_main, NULL, 8u << 20);
}

static int busy(void) { return go; }

/* ---- time ---- */

/* The Super FX's clock as the model has moved it; past the S-CPU's time,
   back to the S-CPU until it catches up. */
static void clk_sync(void)
{
    if (!go)
        return;
    gsu_now = run_master + (clk.now - run_clk) * MASTER_HZ / GSU_HZ;
    if (gsu_now >= gsu_target)
        nshvc_coro_switch(gsu_co, main_co);
}

static void sync(void)
{
    if (!go) {
        gsu_now = nshvc_cart_clock();
        return;
    }
    /* untimed: a running program goes on to its STOP now */
    gsu_target = nshvc_untimed() ? UINT64_MAX : nshvc_cart_clock();
    if (gsu_now < gsu_target) {
        main_co = nshvc_coro_current();
        nshvc_coro_switch(main_co, gsu_co);
    }
}

/* A bus access while the S-CPU has that bus (SCMR's RAN bit 3 for the RAM,
   RON bit 4 for the ROM clear): 6 clocks at a time until it gives it back. */
static void bus_wait(GsuClock *c, int ram)
{
    if (nshvc_untimed() && go && !(gsu.scmr & (ram ? 0x08 : 0x10)))
        nshvc_fatal("untimed, the Super FX waits for the bus the S-CPU holds (SCMR's %s): it would never get it",
                    ram ? "RAN" : "RON");
    while (go && !(gsu.scmr & (ram ? 0x08 : 0x10))) {
        gsuclk_step(c, GSUCLK_BUS, 6);
        clk_sync();
    }
}

static void program_starts(void)
{
    go = 1;
    pending = 1;
    gsu_now = nshvc_cart_clock();
    /* the NOP the last STOP left in the pipeline runs first: it fetches
       the program's first byte */
    run_master = gsu_now;
    run_clk = clk.now;
}

/* the C of the program at bank:addr: the cartridge's gsu_run, or its
   routine there */
static void run_program(uint8_t bank, uint16_t addr)
{
    const nshvc_routine *r;
    if (cart->gsu_run) {
        cart->gsu_run(bank, addr);
        return;
    }
    r = nshvc_cart_routine((uint32_t)bank << 16 | addr);
    if (!r)
        nshvc_fatal("Super FX program at $%02X:%04X has no C", bank, addr);
    r->abi();
}

static void gsu_main(void *arg)
{
    (void)arg;
    for (;;) {
        const nshvc_routine *r;
        while (!pending)
            nshvc_coro_switch(gsu_co, main_co);
        pending = 0;
        gsu.pre_b = gsu.pre_alt1 = gsu.pre_alt2 = gsu.pre_sreg = gsu.pre_dreg = 0;
        gsuclk_code(&clk, (uint32_t)pbr << 16 | gsu.r[15], 1);
        r = nshvc_cart_routine((uint32_t)pbr << 16 | gsu.r[15]);
        if (r && (r->flags & NSHVC_RT_GSU_CACHE_FIRST))
            gsuclk_cache(&clk, (uint16_t)(gsu.r[15] + 1));   /* a CACHE first: it runs after the next fetch */
        clk_sync();
        run_program(pbr, gsu.r[15]);
        /* the STOP: GO clear, the interrupt unless masked */
        go = 0;
        store_made(&clk);
        clk.ramcl = 0;
        sfr_alt = sfr_b = 0;
        if (!(cfgr & 0x80)) {
            sfr_irq = 1;
            nshvc_cart_irq(1);
        }
        nshvc_coro_switch(gsu_co, main_co);
    }
}

/* ---- the S-CPU's side ---- */
static uint16_t sfr(void)
{
    return (uint16_t)(gsu.z << 1 | gsu.cy << 2 | gsu.s << 3 | gsu.ov << 4 | go << 5 |
                      (sfr_alt & 3) << 8 | sfr_b << 12 | sfr_irq << 15);
}

static uint8_t reg_read(uint16_t addr)
{
    if (addr >= 0x3100 && addr < 0x3300)
        return cache[(addr - 0x3100 + clk.cbr) & 511];
    if (addr < 0x3020) {
        uint16_t r = gsu.r[(addr >> 1) & 15];
        return (uint8_t)((addr & 1) ? r >> 8 : r);
    }
    switch (addr) {
    case 0x3030: return (uint8_t)sfr();
    case 0x3031: {
        uint8_t v = (uint8_t)(sfr() >> 8);
        sfr_irq = 0;
        nshvc_cart_irq(0);
        return v;
    }
    case 0x3034: return pbr;
    case 0x3036: return gsu.rombr;
    case 0x303B: return 0x01;            /* VCR: the GSU-1 */
    case 0x303C: return gsu.rambr;
    case 0x303E: return cbr_lo;
    case 0x303F: return cbr_hi;
    }
    return nshvc_open_bus();
}

static void reg_write(uint16_t addr, uint8_t v)
{
    if (addr >= 0x3100 && addr < 0x3300) {
        /* the code cache, CBR on; a line's last byte makes it valid */
        unsigned a = (addr - 0x3100 + clk.cbr) & 511;
        cache[a] = v;
        if ((a & 15) == 15)
            clk.valid |= 1u << (a >> 4);
        return;
    }
    if (addr < 0x3020) {
        int n = (addr >> 1) & 15;
        if (!(addr & 1)) {
            reg_latch = v;
            return;
        }
        gsu.r[n] = (uint16_t)(v << 8 | reg_latch);
        if (n == 14)
            gsu.romdr = gsu_fetch((uint32_t)gsu.rombr << 16 | gsu.r[14]);
        if (n == 15)
            program_starts();   /* R15 written: the program starts */
        return;
    }
    switch (addr) {
    case 0x3030:
        gsu.z = (v >> 1) & 1;
        gsu.cy = (v >> 2) & 1;
        gsu.s = (v >> 3) & 1;
        gsu.ov = (v >> 4) & 1;
        if (!(v & 0x20) && go)
            nshvc_fatal("the S-CPU stopped a running Super FX program (not supported)");
        if ((v & 0x20) && !go)
            program_starts();
        return;
    case 0x3031:
        sfr_alt = v & 3;
        sfr_b = (v >> 4) & 1;
        sfr_irq = (v >> 7) & 1;
        return;
    case 0x3033: bramr = v; return;
    case 0x3034:
        pbr = v;
        gsuclk_flush_code(&clk, clk.cbr);   /* the code cache emptied */
        return;
    case 0x3037: cfgr = v; clk.ms0 = (uint8_t)(v >> 5 & 1); return;
    case 0x3038: gsu.scbr = v; return;
    case 0x3039: clsr = v; clk.clsr = (uint8_t)(v & 1); return;
    case 0x303A: gsu.scmr = v; return;
    }
}

/* ---- the Super FX's own bus (gsu.h) ---- */

static inline uint8_t bus_byte_by(uint32_t addr, int who)
{
    uint8_t bank = (uint8_t)(addr >> 16);
    uint16_t off = (uint16_t)addr;
    if (bank >= 0x70 && bank < 0x7E)
        return *ram_byte(addr);
    if (bank < 0x40)
        return nshvc_cart_rom((uint32_t)(bank & 0x3F) << 15 | (off & 0x7FFF), who);
    if (bank < 0x60)
        return nshvc_cart_rom((uint32_t)(bank & 0x1F) << 16 | off, who);
    return 0;
}

static inline uint8_t bus_byte(uint32_t addr) { return bus_byte_by(addr, NSHVC_ROM_GSU); }

/* The ROM buffer's fill (an R14 write): the byte at addr, which the program
   may never read. On a cartridge that holds only the game's data, a byte
   it does not hold (code the C stands for) marks the buffer instead of
   stopping the console; only a GETB or GETC of it asks for the byte. */
static int romdr_unheld;

uint8_t gsu_fetch(uint32_t addr)
{
    uint8_t v;
    addr &= 0xFFFFFF;
    fetch_at = addr;
    nshvc_rom_unheld = 0;
    v = bus_byte_by(addr, NSHVC_ROM_GSU | NSHVC_ROM_FETCH);
    romdr_unheld = nshvc_rom_unheld;
    return v;
}

static int is_ram(uint32_t addr) { uint8_t bank = (uint8_t)(addr >> 16); return bank >= 0x70 && bank < 0x7E; }

/* A load or store through the RAM buffer (a read of ROM is the ROM
   buffer's, timed by gsu_buffer). */
uint8_t gsu_rd8(uint32_t addr)
{
    addr &= 0xFFFFFF;
    if (is_ram(addr) && go) {
        gsuclk_ram_read(&clk);
        clk_sync();
    }
    return bus_byte(addr);
}

/* A store goes into the RAM buffer and reaches the RAM as the buffer
   drains, 5 clocks on (after the last store's, which it waits for); one
   left when the program stops reaches it then. */
static void store_made(GsuClock *c)
{
    (void)c;
    if (store_pending) {
        store_pending = 0;
        *ram_byte(store_addr) = store_value;
    }
}

void gsu_wr8(uint32_t addr, uint8_t value)
{
    addr &= 0xFFFFFF;
    if (!is_ram(addr))
        return;
    if (go) {
        gsuclk_ram_write(&clk);
        store_addr = addr;
        store_value = value;
        store_pending = 1;
        clk_sync();
        return;
    }
    *ram_byte(addr) = value;
}

/* the pixel caches' own accesses: straight to the RAM */
static uint8_t pix_read(GsuClock *c, uint32_t addr) { (void)c; return *ram_byte(addr); }
static void pix_write(GsuClock *c, uint32_t addr, uint8_t v) { (void)c; *ram_byte(addr) = v; }

/* ---- the time the C states (gsu.h) ---- */
void gsu_code(uint32_t addr, uint32_t n) { gsuclk_code(&clk, addr, n); clk_sync(); }
void gsu_cache(uint16_t r15) { gsuclk_cache(&clk, r15); }
void gsu_cyc(uint32_t clocks) { gsuclk_step(&clk, GSUCLK_INSN, clocks); clk_sync(); }

void gsu_time(int what)
{
    if (what == 0)
        gsuclk_mult(&clk);         /* GSU_T_MULT */
    else
        gsuclk_fmult(&clk);        /* GSU_T_FMULT */
    clk_sync();
}

void gsu_buffer(int what)
{
    if (what == 0)
        gsuclk_r14(&clk);          /* GSU_B_R14 */
    else if (what == 2)
        gsuclk_rambuf(&clk);       /* GSU_B_RAMBUF */
    else
        gsuclk_rombuf(&clk);       /* GSU_B_ROMBUF, GSU_B_ROMB */
    clk_sync();
    /* GETB, GETC of a buffer filled from a byte the cartridge does not
       hold: the byte, asked for now */
    if (what == 1 && romdr_unheld) {
        gsu.romdr = bus_byte(fetch_at);
        romdr_unheld = 0;
    }
}

/* the screen as PLOT and RPIX lay it out: SCMR's colour depth (bits 0-1)
   and height (bits 2 and 5; POR's OBJ, bit 4, the sprite layout), its base
   SCBR x 1 KB */
static GsuScreen screen(void)
{
    GsuScreen s;
    s.ht = (uint8_t)((gsu.por & 0x10) ? 3 : ((gsu.scmr >> 2) & 1) | ((gsu.scmr >> 4) & 2));
    s.md = (uint8_t)(gsu.scmr & 3);
    s.scbr = gsu.scbr;
    s.ext_w = ext_w;
    s.ext_h = ext_h;
    return s;
}

void nshvc_ext_gsu_screen(int width, int height)
{
    if ((width || height) && (width < 8 || width > 1024 || height < 8 || height > 512 || (width & 7) || (height & 7)))
        nshvc_fatal("nshvc_ext_gsu_screen(%d, %d): 8 to 1024 by 8 to 512, in characters (0, 0: the GSU's own)", width,
                   height);
    ext_w = (uint16_t)width;
    ext_h = (uint16_t)height;
}

void gsu_plot_at(uint16_t x, uint16_t y);
uint8_t gsu_rpix_at(uint16_t x, uint16_t y);

void gsu_plot(uint8_t x, uint8_t y) { gsu_plot_at(x, y); }
uint8_t gsu_rpix(uint8_t x, uint8_t y) { return gsu_rpix_at(x, y); }

void gsu_plot_at(uint16_t x, uint16_t y)
{
    GsuScreen s = screen();
    uint8_t c = gsu.colr;
    if (!(gsu.por & 0x01)) {
        if (s.md == 3) {
            if (gsu.por & 0x08) {
                if (!(c & 0x0F))
                    return;
            } else if (!c) {
                return;
            }
        } else if (!(c & 0x0F)) {
            return;
        }
    }
    if ((gsu.por & 0x02) && s.md != 3) {
        if ((x ^ y) & 1)
            c >>= 4;
        c &= 0x0F;
    }
    gsuclk_plot(&clk, &s, x, y, c);
    clk_sync();
}

uint8_t gsu_rpix_at(uint16_t x, uint16_t y)
{
    GsuScreen s = screen();
    uint8_t v = gsuclk_rpix(&clk, &s, x, y);
    clk_sync();
    return v;
}

/* ---- the chip on the connector (cart.h) ---- */

/* the S-CPU's accesses it decodes: its registers at $3000-$32FF of banks
   $00-$3F and $80-$BF, its RAM at $70-$7D (and its first 8 KB at
   $6000-$7FFF of those banks), brought up to the S-CPU's time first */
static int decodes(uint32_t addr, int *reg)
{
    uint8_t b = (uint8_t)(addr >> 16) & 0x7F;
    uint16_t off = (uint16_t)addr;
    if (b < 0x40 && off >= 0x3000 && off < 0x3300) {
        *reg = 1;
        return 1;
    }
    *reg = 0;
    if (b < 0x40 && off >= 0x6000 && off < 0x8000)
        return 1;
    return b >= 0x70 && b < 0x7E && (gsu_ram_size > 0x20000 ? (uint32_t)(b - 0x70) << 16 < gsu_ram_size : b < 0x72);
}

/* the S-CPU's view of the RAM at addr (banks $80-$FF as $00-$7F; the 8 KB
   window at $6000 its first) */
static uint8_t *cpu_ram(uint32_t addr)
{
    uint8_t b = (uint8_t)(addr >> 16) & 0x7F;
    if (b < 0x40)
        return ram_byte(0x700000u | ((uint16_t)addr - 0x6000));
    return ram_byte((uint32_t)b << 16 | (uint16_t)addr);
}

static int cpu_read(uint32_t addr, uint8_t *v)
{
    int reg;
    if (!decodes(addr, &reg))
        return 0;
    sync();
    *v = reg ? reg_read((uint16_t)addr) : *cpu_ram(addr);
    return 1;
}

static int cpu_write(uint32_t addr, uint8_t v)
{
    int reg;
    if (!decodes(addr, &reg))
        return 0;
    sync();
    if (reg)
        reg_write((uint16_t)addr, v);
    else
        *cpu_ram(addr) = v;
    return 1;
}

static int cpu_peek(uint32_t addr, uint8_t *v)
{
    int reg;
    if (!decodes(addr, &reg))
        return 0;
    *v = reg ? 0 : *cpu_ram(addr);
    return 1;
}

/* the interrupt as a hold found it, put back as the hold ends, so a program
   held code ran (its STOP raising the interrupt) leaves the game's own
   handlers nothing to take */
static int held_irq;

static void hold(int on)
{
    if (on) {
        held_irq = sfr_irq;
        return;
    }
    sfr_irq = (uint8_t)held_irq;
    nshvc_cart_irq(sfr_irq);
}

static uint8_t *ram(uint32_t *size)
{
    sync();
    if (size)
        *size = gsu_ram_size;
    return ram_store;
}

const nshvc_chip nshvc_superfx = {
    .name = "Super FX",
    .power = power,
    .read = cpu_read,
    .write = cpu_write,
    .peek = cpu_peek,
    .sync = sync,
    .hold = hold,
    .ram = ram,
    .busy = busy,
};
