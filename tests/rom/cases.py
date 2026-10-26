"""
cases.py: the test cartridges (snestest.Test), each one feature of the
console with random data, so every path of it is reached.
"""
from snestest import Test


def bg_mode(name, mode, bgmode_extra=0, tm=0x1F, ts=0x00, big=0, scroll=True, seed=None, t=None):
    t = t or Test(name, seed=seed)
    t.vram(0x0000, t.rand_bytes(0x10000))          # random tiles and maps everywhere
    t.cgram(0, t.rand_colors(256))
    t.w(0x2105, mode | bgmode_extra | big << 4)
    for n in range(4):
        t.w(0x2107 + n, t.rng.getrandbits(8))
    t.w(0x210B, t.rng.getrandbits(8))
    t.w(0x210C, t.rng.getrandbits(8))
    if scroll:
        for n in range(4):
            t.w16(0x210D + 2 * n, t.rng.getrandbits(10))
            t.w16(0x210E + 2 * n, t.rng.getrandbits(10))
    t.w(0x212C, tm)
    t.w(0x212D, ts)
    return t


def shown(t, inidisp=0x0F):
    t.show(inidisp)
    return t


def sprites(name, obsel, count=128, prio_rotate=None, seed=None, mode=1):
    """random sprites over a random mode-1 picture"""
    t = bg_mode(name, mode, tm=0x13, seed=seed)
    t.w(0x2101, obsel)
    oam = bytearray()
    for i in range(128):
        if i < count:
            x = t.rng.getrandbits(9)
            y = t.rng.getrandbits(8)
            oam += bytes([x & 0xFF, y, t.rng.getrandbits(8), t.rng.getrandbits(8)])
        else:
            oam += bytes([0, 0xE0, 0, 0])
    oam += t.rand_bytes(32)
    t.oam(oam)
    if prio_rotate is not None:
        t.w2(0x2102, 0x8000 | prio_rotate << 1)
    t.w(0x212C, 0x17)
    return shown(t)


def windows(name, seed=None):
    t = bg_mode(name, 1, tm=0x17, ts=0x17, seed=seed)
    for r in (0x2123, 0x2124, 0x2125, 0x2126, 0x2127, 0x2128, 0x2129, 0x212A, 0x212B):
        t.w(r, t.rng.getrandbits(8))
    t.w(0x212E, 0x1F)
    t.w(0x212F, 0x1F)
    return shown(t)


def colour_math(name, cgwsel, cgadsub, seed=None, mode=1, windows_on=False):
    t = bg_mode(name, mode, tm=0x15, ts=0x0B, seed=seed)
    t.w(0x2132, 0x20 | t.rng.getrandbits(5))
    t.w(0x2132, 0x40 | t.rng.getrandbits(5))
    t.w(0x2132, 0x80 | t.rng.getrandbits(5))
    if windows_on:
        for r in (0x2125, 0x2126, 0x2127, 0x2128, 0x2129, 0x212B):
            t.w(r, t.rng.getrandbits(8))
    t.w(0x2130, cgwsel)
    t.w(0x2131, cgadsub)
    return shown(t)


def mode7(name, m7sel, extbg=False, seed=None):
    t = Test(name, seed=seed)
    # the map in the low bytes, the characters' pixels in the high bytes
    t.vram(0x0000, t.rand_bytes(0x8000), vmain=0x00)
    t.vram(0x0000, t.rand_bytes(0x8000), vmain=0x80)
    t.cgram(0, t.rand_colors(256))
    t.w(0x2105, 0x07)
    t.w(0x211A, m7sel)
    for r in (0x211B, 0x211C, 0x211D, 0x211E):
        t.w16(r, t.rng.getrandbits(16))
    t.w16(0x211F, t.rng.getrandbits(13))
    t.w16(0x2120, t.rng.getrandbits(13))
    t.w16(0x210D, t.rng.getrandbits(13))
    t.w16(0x210E, t.rng.getrandbits(13))
    t.w(0x2133, 0x40 if extbg else 0x00)
    t.w(0x212C, 0x03 if extbg else 0x01)
    return shown(t)


def brightness(level):
    t = bg_mode("bright%d" % level, 1, tm=0x07)
    return shown(t, level)


def bright_ramp(level):
    """every level of each channel through the backdrop, a colour a line
       (HDMA to CGRAM 0), at one brightness"""
    t = Test("bright_ramp%d" % level)
    table = bytearray()
    for y in range(224):
        r, g, b = y % 32, (y // 7) % 32, 31 - y % 32
        c = r | g << 5 | b << 10
        table += bytes([1, 0, 0, c & 0xFF, c >> 8])
    table += b"\0"
    t.hdma(3, 0x03, 0x21, table)
    t.w(0x420C, 0x08)
    return shown(t, level)


def mosaic(name, size, mask, mode=1, seed=None):
    t = bg_mode(name, mode, tm=0x0F, seed=seed)
    t.w(0x2106, size << 4 | mask)
    return shown(t)


def hdma_scroll():
    """per-line scroll of BG1 and BG2 (HDMA, direct, write twice)"""
    t = bg_mode("hdma_scroll", 1, tm=0x03)
    table = bytearray()
    for _ in range(40):
        table += bytes([0x80 | 5]) + b"".join(bytes([v & 0xFF, v >> 8 & 3]) for v in [t.rng.getrandbits(10)] * 5)
    table += b"\0"
    t.hdma(1, 0x02, 0x0D, table)
    t2 = bytearray()
    for _ in range(28):
        n = t.rng.randrange(1, 12)
        t2 += bytes([n]) + bytes([t.rng.getrandbits(8), t.rng.getrandbits(2)])
    t2 += b"\0"
    t.hdma(2, 0x02, 0x0F, t2)
    t.w(0x420C, 0x06)
    return shown(t)


def hdma_gradient():
    """a colour per line into CGRAM 0 (HDMA mode 3 to CGADD, CGDATA), and an
       indirect table changing the fixed colour"""
    t = bg_mode("hdma_gradient", 1, tm=0x01)
    table = bytearray()
    for _ in range(112):
        c = t.rng.getrandbits(15)
        table += bytes([2, 0, 0, c & 0xFF, c >> 8])
    table += b"\0"
    t.hdma(3, 0x03, 0x21, table)
    data = t.blob(t.rand_bytes(256))
    ind = bytearray()
    for i in range(16):
        n = t.rng.randrange(1, 20) | (0x80 if t.rng.getrandbits(1) else 0)
        a = (data & 0xFFFF) + t.rng.randrange(0, 200)
        ind += bytes([n, a & 0xFF, a >> 8])
    ind += b"\0"
    t.hdma(4, 0x40, 0x32, ind, indirect_bank=data >> 16)
    t.w(0x2131, 0x21)
    t.w(0x420C, 0x18)
    return shown(t)


def reads():
    """the PPU's and CPU's read side: the multiplier, VRAM through its
       prefetch, CGRAM, OAM, the status registers, the CPU's multiplier and
       divider"""
    t = Test("reads", frames=4, shots=(4,))
    t.vram(0x0000, t.rand_bytes(0x800))
    t.cgram(0, t.rand_colors(32))
    t.oam(t.rand_bytes(544))
    for _ in range(4):
        t.w16(0x211B, t.rng.getrandbits(16))
        t.w(0x211C, t.rng.getrandbits(8))
        for a in (0x2134, 0x2135, 0x2136):
            t.read(a)
    t.w(0x2115, 0x80)
    t.w2(0x2116, 0x0123)
    for _ in range(8):
        t.read(0x2139)
        t.read(0x213A)
    t.w(0x2115, 0x00)
    t.w2(0x2116, 0x0040)
    for _ in range(6):
        t.read(0x2139)
        t.read(0x213A)
    t.w(0x2121, 3)
    for _ in range(12):
        t.read(0x213B)
    t.w2(0x2102, 0x0010)
    for _ in range(10):
        t.read(0x2138)
    t.w2(0x2102, 0x0100)
    for _ in range(6):
        t.read(0x2138)
    t.read(0x213E)
    t.read(0x213F)
    for _ in range(3):
        t.w(0x4202, t.rng.getrandbits(8))
        t.w(0x4203, t.rng.getrandbits(8))
        t.wait_line(10)
        t.read(0x4216)
        t.read(0x4217)
        t.w2(0x4204, t.rng.getrandbits(16))
        t.w(0x4206, t.rng.getrandbits(8) | 1)
        t.wait_line(20)
        for a in (0x4214, 0x4215, 0x4216, 0x4217):
            t.read(a)
    t.w2(0x4204, 1234)
    t.w(0x4206, 0)
    t.wait_line(30)
    for a in (0x4214, 0x4215, 0x4216, 0x4217):
        t.read(a)
    t.show()
    return t


def vmain_modes():
    """VRAM written in every VMAIN mode (each increment, each address
       remap, low or high byte stepping), then read back through the
       prefetch"""
    t = Test("vmain_modes", frames=4, shots=(4,))
    for k, vmain in enumerate([0x80, 0x81, 0x82, 0x83, 0x84, 0x88, 0x8C, 0x00, 0x01, 0x0B]):
        base = 0x0100 + k * 0x0C00
        data = t.rand_bytes(64)
        t.w(0x2115, vmain)
        t.w2(0x2116, base)
        port = 0x18 if vmain & 0x80 else 0x19
        t.dma(0, 0x01, 0x18, t.blob(data), len(data))
        t.w(0x2115, 0x80)
        t.w2(0x2116, base)
        t.read(0x2139)
        t.read(0x213A)
        for i in range(40):
            t.read(0x2139)
            t.read(0x213A)
        t.w2(0x2116, base + 0x20 * 3)
        for i in range(8):
            t.read(0x2139)
            t.read(0x213A)
        t.w2(0x2116, base + 0x80 * 2)
        for i in range(8):
            t.read(0x2139)
            t.read(0x213A)
    t.show()
    return t


def overscan():
    t = bg_mode("overscan", 1, tm=0x13)
    t.w(0x2133, 0x04)
    t.overscan = True
    return shown(t)


def pseudo_hires():
    t = bg_mode("pseudo_hires", 1, tm=0x03, ts=0x04)
    t.w(0x2133, 0x08)
    return shown(t)


def all_tests():
    out = []
    for mode in range(7):
        out.append(shown(bg_mode("mode%d" % mode, mode)))
        out.append(shown(bg_mode("mode%d_big" % mode, mode, big=0xF)))
    out.append(shown(bg_mode("mode1_bg3prio", 1, bgmode_extra=0x08)))
    for mode in (2, 4, 6):
        out.append(shown(bg_mode("mode%d_big12" % mode, mode, big=0x3)))
        out.append(shown(bg_mode("mode%d_big3" % mode, mode, big=0x4)))
        for seed in range(6):
            out.append(shown(bg_mode("mode%d_seed%d" % (mode, seed), mode, big=seed % 8, seed=100 + seed)))
    out.append(shown(bg_mode("mode3_direct", 3, tm=0x01)) if False else None)
    t = bg_mode("mode3_direct", 3, tm=0x01)
    t.w(0x2130, 0x01)
    out.append(shown(t))
    t = bg_mode("mode4_direct", 4, tm=0x03)
    t.w(0x2130, 0x01)
    out.append(shown(t))
    for obsel in range(8):
        out.append(sprites("sprites_size%d" % obsel, obsel << 5 | 0x02 | 0x08))
    out.append(sprites("sprites_gap", 0x1A, seed=7))
    out.append(sprites("sprites_rotate", 0x00, prio_rotate=37))
    out.append(sprites("sprites_few", 0x20, count=12))
    for i in range(4):
        out.append(windows("windows%d" % i))
    for i, (sel, sub) in enumerate([(0x02, 0x3F), (0x02, 0x7F), (0x02, 0xBF), (0x02, 0xFF), (0x00, 0x3F),
                                    (0x00, 0xFF), (0x52, 0x7F), (0xA2, 0x7F), (0xF2, 0xBF), (0x32, 0x43)]):
        out.append(colour_math("math%d" % i, sel, sub, windows_on=i >= 6))
    for i, sel in enumerate([0x00, 0x01, 0x02, 0x03, 0x80, 0xC0, 0x40]):
        out.append(mode7("mode7_%d" % i, sel))
    out.append(mode7("mode7_extbg", 0x00, extbg=True))
    out.append(mode7("mode7_wrap_extbg", 0xC0, extbg=True))
    for level in (0, 1, 7, 8, 14):
        out.append(brightness(level))
    for level in range(16):
        out.append(bright_ramp(level))
    out.append(mosaic("mosaic_3", 3, 0x0F))
    out.append(mosaic("mosaic_15", 15, 0x05))
    out.append(hdma_scroll())
    out.append(hdma_gradient())
    out.append(reads())
    out.append(probe_stat78())
    out.append(probe_openbus())
    out.append(vmain_modes())
    out.append(overscan())
    out.append(pseudo_hires())
    for mode in (2, 6):
        for h1, h3, st in ((0, 0, 8), (3, 0, 8), (0, 16, 8), (5, 0, 24)):
            out.append(probe_opt(mode, h1, h3, st))
        for h1, h3, st in ((0, 0, 8), (0, 8, 8), (0, 24, 8), (5, 0, 24)):
            out.append(probe_opt(mode, h1, h3, st, big3=True))
    for size in (5, 6, 7):
        for flags in (0x00, 0x40, 0x80, 0xC0):
            out.append(probe_sprite(size, flags))
    for h in range(0, 340, 20):
        out.append(irq_dot(h))
    out.append(irq_dot(339))
    for h in (0, 50, 100, 200, 300):
        out.append(irq_latch(h))
    for h in range(3, 339, 9):
        out.append(irq_latch(h, v=60))
    for h in (0, 30, 60, 90, 180, 220, 260, 300):
        out.append(probe_scroll(h))
        out.append(probe_scroll(h, value=0x43))
    for h in range(0, 120, 7):
        out.append(probe_scroll(h, reg=0x2100, value=0x07))
        out.append(probe_scroll(h, reg=0x210E, value=0x08))
    for h in (0, 40, 80, 120, 160, 200, 240, 280, 320):
        out.append(probe_cgram(h))
    for h in range(40, 300, 13):
        out.append(probe_cgram_col(h))
    for h in (80, 160, 240):
        out.append(probe_cgram_col(h, tm=0x00))
        out.append(probe_cgram_col(h, tm=0x01, ts=0x01, cgwsel=0x02))
    out.append(probe_cgram(100, index=0x80))
    out.append(probe_cgram(100, mode=3))
    for h in (0, 100, 160, 200):
        out.append(irq_reg(0x2100, 0x07, h))
        out.append(irq_reg(0x212C, 0x00, h))
    out.append(irq_split("irq_v", 0xA0))
    out.append(irq_split("irq_off", 0x80, seed="irq_v"))
    out.append(irq_split("irq_hv_10", 0xB0, h=10))
    out.append(irq_split("irq_hv_200", 0xB0, h=200, lines=(30, 31, 90, 150, 151, 200)))
    out.append(mapping("map_lorom_1m", "lorom", 0x100000))
    out.append(mapping("map_lorom_3m", "lorom", 0x300000, sram=0x2000))
    out.append(mapping("map_lorom_1_5m_sram", "lorom", 0x180000, sram=0x8000))
    out.append(mapping("map_lorom_2m_sram", "lorom", 0x200000, sram=0x2000))
    out.append(mapping("map_lorom_2_5m_sram", "lorom", 0x280000, sram=0x2000))
    out.append(mapping("map_lorom_512k_sram", "lorom", 0x80000, sram=0x800))
    out.append(mapping("map_hirom_2m", "hirom", 0x200000, sram=0x2000))
    out.append(mapping("map_hirom_3m", "hirom", 0x300000, sram=0x800))
    out.append(mapping("map_hirom_4m", "hirom", 0x400000))
    out.append(mapping("map_exhirom_6m", "exhirom", 0x600000, sram=0x2000))
    out.append(backdrop())
    out.append(one_bg())
    return [t for t in out if t is not None]


def backdrop():
    t = Test("backdrop")
    t.cgram(0, bytes([0x1F, 0x00]))
    t.show()
    return t


def one_bg():
    t = Test("one_bg")
    tile = bytes([0xFF, 0x00] * 8)             # colour 1 everywhere
    t.vram(0x1000, tile)
    t.cgram(0, bytes([0x00, 0x7C, 0x1F, 0x00]))
    t.w(0x2107, 0x00)
    t.w(0x210B, 0x01)
    t.w(0x212C, 0x01)
    t.show()
    return t


def probe_sprite(size, flags):
    """one large sprite whose every 8x8 tile is a solid colour, its number:
       which tile the PPU draws where (sizes 6 and 7, flips)"""
    t = Test("probe_spr%d_%02x" % (size, flags))
    chars = bytearray()
    for n in range(512):
        c = ((n >> 4) & 7) + 1 + 8 * (n & 1)
        planes = [(0xFF if c >> p & 1 else 0) for p in range(4)]
        chars += bytes([planes[0], planes[1]] * 8 + [planes[2], planes[3]] * 8)
    t.vram(0x0000, chars)
    pal = bytearray()
    for i in range(128):
        pal += bytes([0, 0])
    for i in range(16):
        c = i * 2 | (i // 2) << 5 | (31 - i) << 10
        pal += bytes([c & 0xFF, c >> 8])
    t.cgram(0, pal)
    t.w(0x2101, size << 5)
    oam = bytearray([40, 20, 0x00, flags])
    for i in range(1, 128):
        oam += bytes([0, 0xE0, 0, 0])
    oam += bytes([0x02]) + bytes(31)
    t.oam(oam)
    t.w(0x212C, 0x10)
    return shown(t)


def probe_opt(mode, hofs1=0, hofs3=0, step=8, apply_v=False, big3=False):
    """offset-per-tile measured: BG1's map columns each a colour of their
       own (tile colour = column & 15, palette = column >> 4), BG3's entries
       a horizontal offset of `step` x their column, every one applied to
       BG1; the picture tells which entry moves which screen column, and by
       how much"""
    t = Test("probe_opt%d_%d_%d_%d%s" % (mode, hofs1, hofs3, step, "_b3" if big3 else ""))
    bpp = 4
    chars = bytearray()
    for c in range(16):
        planes = [(0xFF if c >> p & 1 else 0) for p in range(4)]
        chars += bytes([planes[0], planes[1]] * 8 + [planes[2], planes[3]] * 8)
    t.vram(0x2000, chars)                 # BG1's characters at $2000 (word)
    bg1 = bytearray()
    for ty in range(32):
        for tx in range(64):
            col = tx
            e = (col & 15) | ((col >> 4) & 7) << 10
            bg1 += bytes([e & 0xFF, e >> 8])
    t.vram(0x0000, bg1[:0x800])           # 32x32 at $0000, the right half at $0400
    t.vram(0x0400, bg1[0x800:0x1000])
    # BG1 is a 64x32 map: columns 0-31 at $0000, 32-63 at $0400
    m = bytearray()
    for ty in range(32):
        for tx in range(32):
            e = (tx & 15) | ((tx >> 4) & 7) << 10
            m += bytes([e & 0xFF, e >> 8])
    m2 = bytearray()
    for ty in range(32):
        for tx in range(32, 64):
            e = (tx & 15) | ((tx >> 4) & 7) << 10
            m2 += bytes([e & 0xFF, e >> 8])
    t.vram(0x0000, m)
    t.vram(0x0400, m2)
    opt = bytearray()
    for ty in range(32):
        for tx in range(32):
            v = (tx * step) & 0x3F8 | 0x2000
            if apply_v and ty == 1:
                v = 0
            opt += bytes([v & 0xFF, v >> 8])
    t.vram(0x1000, opt)                   # BG3's map at $1000
    pal = bytearray()
    for p in range(8):
        for c in range(16):
            col = c << 1 | p << 6 | (31 - c) << 10
            pal += bytes([col & 0xFF, col >> 8])
    t.cgram(0, pal)
    t.w(0x2105, mode | (0x40 if big3 else 0))
    t.w(0x2107, 0x01)                     # BG1: map $0000, 64x32
    t.w(0x2109, 0x10)                     # BG3: map $1000
    t.w(0x210B, 0x02)                     # BG1 characters at $2000
    t.w16(0x210D, hofs1)
    t.w16(0x2111, hofs3)
    t.w(0x212C, 0x01)
    return shown(t)


def mapping(name, mapping, rom_size, sram=0, banks=None):
    """the cartridge's addresses: ROM read across every bank and its
       mirrors, the cartridge's RAM written and read back"""
    t = Test(name, frames=3, shots=(3,), mapping=mapping, rom_size=rom_size, sram=sram)
    addrs = []
    for bank in banks or range(0, 256, 3):
        if bank in (0x7E, 0x7F):
            continue
        for off in (0x0000, 0x2345, 0x6000, 0x7FFF, 0x8000, 0xC123, 0xFFFF):
            if bank & 0x40 == 0 and off < 0x6000:
                continue          # work RAM and registers
            addrs.append(bank << 16 | off)
    if sram:
        ram_at = {"lorom": [0x700000, 0x710123, 0x7D7FFF, 0xF00000, 0xFE4567, 0x700000 + sram],
                  "hirom": [0x206000, 0x317FFF, 0x3F6123, 0xA06000, 0xBF7FFF],
                  "exhirom": [0x806000, 0x8F7123, 0xBF7FFF, 0xA06000]}[mapping]
        for i, a in enumerate(ram_at):
            t.wlong(a, 0x11 * (i + 1))
        for a in ram_at:
            addrs.append(a)
    for a in addrs:
        t.read(a)
    t.show()
    return t


def irq_dot(h, v=40):
    """one write (BG1's scroll) from an IRQ at dot h of line v: where in the
       line the change starts"""
    t = bg_mode("irq_dot_%d_%d" % (v, h), 1, tm=0x01, seed="sweep")
    t.w2(0x4209, v)
    t.w2(0x4207, h)
    t.irq_w(0x210D, 0x80)
    t.irq_w(0x210D, 0x00)
    t.irq_w(0x4209, 0xFF)
    t.irq_w(0x420A, 0x01)
    t.irq_end()
    t.nmi_w(0x4209, v & 0xFF)
    t.nmi_w(0x420A, v >> 8)
    t.nmi_w(0x210D, 0x00)
    t.nmi_w(0x210D, 0x00)
    t.show()
    t.w(0x4200, 0xB0)
    t.cli()
    return t


def irq_reg(reg, val, h, v=40, name=None):
    """one write to a register from an IRQ at dot h of line v"""
    t = bg_mode(name or "irq_reg_%02x_%d" % (reg & 0xFF, h), 1, tm=0x01, seed="sweep")
    t.w2(0x4209, v)
    t.w2(0x4207, h)
    t.irq_w(reg, val)
    t.irq_w(0x4209, 0xFF)
    t.irq_w(0x420A, 0x01)
    t.irq_end()
    t.nmi_w(0x4209, v & 0xFF)
    t.nmi_w(0x420A, v >> 8)
    t.nmi_w(reg, 0x0F if reg == 0x2100 else 0x01)
    t.show()
    t.w(0x4200, 0xB0)
    t.cli()
    return t


def irq_latch(h, v=40):
    """the counters latched from an IRQ at dot h of line v (WRIO's bit 7
       falling), read back by the main code: when the IRQ's code runs"""
    t = Test("irq_latch_%d_%d" % (v, h), frames=6, shots=(6,))
    t.w2(0x4209, v)
    t.w2(0x4207, h)
    t.irq_w(0x4201, 0x00)
    t.irq_w(0x4201, 0xFF)
    t.irq_end()
    t.nmi_w(0x4209, v & 0xFF)
    t.nmi_w(0x420A, v >> 8)
    t.show()
    t.wait_frames(2)
    t.w(0x4200, 0x30)
    t.cli()
    t.wai()
    t.w(0x4200, 0x00)
    t.read(0x213F)
    for a in (0x213C, 0x213C, 0x213D, 0x213D):
        t.read(a)
    return t


def probe_scroll(h, reg=0x210D, value=0x40, v=40):
    """BG1 in mode 3 whose every pixel shows its own column ((x + scroll) &
       255 is its colour index, every colour distinct), changed from an IRQ
       at dot h of line v: the pixel the change reaches each pixel at"""
    t = Test("probe_scroll_%02x_%d_%02x" % (reg & 0xFF, h, value))
    chars = bytearray()
    for tile in range(32):
        rows = []
        for y in range(8):
            planes = [0] * 8
            for x in range(8):
                c = tile * 8 + x
                for p in range(8):
                    if c >> p & 1:
                        planes[p] |= 0x80 >> x
            rows.append(planes)
        for pair in range(4):
            for y in range(8):
                chars += bytes([rows[y][2 * pair], rows[y][2 * pair + 1]])
    t.vram(0x1000, chars)                 # characters at $1000 (word), 64 bytes each
    m = bytearray()
    for ty in range(32):
        for tx in range(32):
            m += bytes([tx, 0])
    t.vram(0x0000, m)
    pal = bytearray()
    for c in range(256):
        col = (c & 31) | ((c >> 5) & 7) << 5 | (31 - (c & 31)) << 10
        pal += bytes([col & 0xFF, col >> 8])
    t.cgram(0, pal)
    t.w(0x2105, 0x03)
    t.w(0x2107, 0x00)
    t.w(0x210B, 0x01)
    t.w(0x212C, 0x01)
    t.w2(0x4209, v)
    t.w2(0x4207, h)
    if reg in (0x210D, 0x210E):
        t.irq_w(reg, value)
        t.irq_w(reg, 0x00)
    else:
        t.irq_w(reg, value)
    t.irq_w(0x4209, 0xFF)
    t.irq_w(0x420A, 0x01)
    t.irq_end()
    t.nmi_w(0x4209, v & 0xFF)
    t.nmi_w(0x420A, v >> 8)
    t.nmi_w(0x210D, 0x00)
    t.nmi_w(0x210D, 0x00)
    t.nmi_w(0x210E, 0x00)
    t.nmi_w(0x210E, 0x00)
    t.nmi_w(0x2100, 0x0F)
    t.show()
    t.w(0x4200, 0xB0)
    t.cli()
    return t


def probe_cgram(h, index=0x10, v=40, mode=1):
    """a colour written to CGRAM from an IRQ at dot h of line v, the screen
       shown: where it lands (all of CGRAM read back in forced blank)"""
    t = bg_mode("probe_cgram_%d_%02x_m%d" % (h, index, mode), mode, tm=0x13, seed="cgprobe")
    t.w2(0x4209, v)
    t.w2(0x4207, h)
    t.irq_w(0x2121, index)
    t.irq_w(0x2122, 0x5A)
    t.irq_w(0x2122, 0x25)
    t.irq_w(0x4200, 0x00)
    t.irq_end()
    t.show()
    t.w(0x4200, 0x30)
    t.cli()
    t.wai()
    t.w(0x2100, 0x80)
    t.w(0x2121, 0)
    for i in range(512):
        t.read(0x213B)
    return t


def probe_cgram_col(h, v=40, tm=0x01, ts=0x00, cgwsel=0x00):
    """as probe_cgram, over the column-coded screen of probe_scroll (each
       pixel's colour index its column): the entry a write lands at tells
       which pixel's colour the PPU was looking up"""
    t = probe_scroll(0, reg=0x2100, value=0x0F)
    t.name = "probe_cgcol_%d_%02x_%02x_%02x" % (h, tm, ts, cgwsel)
    t.steps = t.steps[:0]
    t.irq_steps = bytearray()
    t.nmi_steps = bytearray()
    t.prologue()
    base = probe_scroll(0, reg=0x2100, value=0x0F)
    t.blobs = []
    t.next_file = 0x8000
    # rebuild the screen as probe_scroll does, then the probe
    chars = bytearray()
    for tile in range(32):
        rows = []
        for y in range(8):
            planes = [0] * 8
            for x in range(8):
                c = tile * 8 + x
                for p in range(8):
                    if c >> p & 1:
                        planes[p] |= 0x80 >> x
            rows.append(planes)
        for pair in range(4):
            for y in range(8):
                chars += bytes([rows[y][2 * pair], rows[y][2 * pair + 1]])
    t.vram(0x1000, chars)
    m = bytearray()
    for ty in range(32):
        for tx in range(32):
            m += bytes([tx, 0])
    t.vram(0x0000, m)
    pal = bytearray()
    for c in range(256):
        col = (c & 31) | ((c >> 5) & 7) << 5 | (31 - (c & 31)) << 10
        pal += bytes([col & 0xFF, col >> 8])
    t.cgram(0, pal)
    t.w(0x2105, 0x03)
    t.w(0x2107, 0x00)
    t.w(0x210B, 0x01)
    t.w(0x212C, tm)
    t.w(0x212D, ts)
    t.w(0x2130, cgwsel)
    t.w2(0x4209, v)
    t.w2(0x4207, h)
    t.irq_w(0x2121, 0x00)
    t.irq_w(0x2122, 0xFF)
    t.irq_w(0x2122, 0x7F)
    t.irq_w(0x4200, 0x00)
    t.irq_end()
    t.show()
    t.w(0x4200, 0x30)
    t.cli()
    t.wai()
    t.w(0x2100, 0x80)
    t.w(0x2121, 0)
    for i in range(512):
        t.read(0x213B)
    return t


def probe_openbus():
    """the write-only PPU registers read at the start and after a read of
       a PPU1 register: which answer PPU1's open bus, which the CPU's"""
    t = Test("probe_openbus", frames=3, shots=(3,))
    t.reads_first = True
    for r in range(0x2100, 0x2134):
        t.read(r)
    t.w16(0x211B, 0x1234)
    t.w(0x211C, 0x5A)
    t.read(0x2135)
    for r in range(0x2100, 0x2134):
        t.read(r)
    t.show()
    return t


def probe_stat78():
    """what $213F's open-bus bit holds: after the start, after CGRAM reads
       of known high bytes, after writes"""
    t = Test("probe_stat78", frames=3, shots=(3,))
    t.cgram(0, bytes([0x00, 0x00, 0x00, 0x20, 0x00, 0x7F, 0x55, 0x00]))
    t.read(0x213F)
    t.read(0x213F)
    t.w(0x2121, 0)
    t.read(0x213B)
    t.read(0x213B)
    t.read(0x213F)
    t.read(0x213B)
    t.read(0x213B)
    t.read(0x213F)
    t.read(0x213B)
    t.read(0x213B)
    t.read(0x213F)
    t.w(0x2130, 0x20)
    t.read(0x213F)
    t.w(0x2104, 0x00)
    t.read(0x213F)
    t.read(0x213E)
    t.read(0x213E)
    t.show()
    return t


def irq_split(name, mode, h=None, lines=(40, 80, 120, 160), seed=None):
    """raster splits from the IRQ: at each line (and dot h, else the line's
       start) BG1's and BG2's scroll and the backdrop change, then the next
       IRQ's line is set; the NMI starts the IRQs again each frame"""
    t = bg_mode(name, 1, tm=0x03, seed=seed)
    t.w2(0x4209, lines[0])
    if h is not None:
        t.w2(0x4207, h)
    for k, line in enumerate(lines):
        t.irq_w(0x210D, t.rng.getrandbits(8))
        t.irq_w(0x210D, t.rng.getrandbits(2))
        t.irq_w(0x2110, t.rng.getrandbits(8))
        t.irq_w(0x2110, t.rng.getrandbits(2))
        t.irq_w(0x2121, 0)
        c = t.rng.getrandbits(15)
        t.irq_w(0x2122, c & 0xFF)
        t.irq_w(0x2122, c >> 8)
        nxt = lines[k + 1] if k + 1 < len(lines) else 0x1FF
        t.irq_w(0x4209, nxt & 0xFF)
        t.irq_w(0x420A, nxt >> 8)
        t.irq_end()
    # the NMI puts the first split's line back
    t.nmi_w(0x4209, lines[0] & 0xFF)
    t.nmi_w(0x420A, lines[0] >> 8)
    t.show()
    t.w(0x4200, mode)
    t.cli()
    return t
