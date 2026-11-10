#!/usr/bin/env python3
"""scenes.py: each feature scene is made twice from one description: a test ROM
for bsnes (through tests/oracle) and a scene file for the engine's API
(player.c). The pictures must match byte for byte.

  scenes.py --oracle ORACLE --core CORE --player PLAYER --out DIR [--only A,B]
  scenes.py ... --prove         one scene changed on the engine's side only: must fail

The engine plays each scene headless with the picture at the console's own
size, a window pixel for a picture pixel (RR_SIZE the oracle's shot's size,
the screen filling the window), so nothing is filtered or mixed.
"""
import argparse, os, random, re, subprocess, sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "rom"))
import snestest

def line_fields():
    """nshvc_line_field's values, read from the API's header"""
    path = os.path.join(os.path.dirname(__file__), "..", "..", "include", "nshvc", "nshvc.h")
    with open(path) as f:
        text = f.read()
    body = text[text.index("typedef enum {", text.index("what a line can change")):]
    body = body[:body.index("} nshvc_line_field;")]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    out, n = {}, 0
    for item in body.split("typedef enum {")[1].split(","):
        item = item.strip()
        if not item:
            continue
        if "=" in item:
            name, v = [x.strip() for x in item.split("=")]
            n = int(v)
        else:
            name = item
        out[name] = n
        n += 1
    return out


LINE = line_fields()


def solid_tile(bpp, c):
    """a tile whose every texel is colour c, in the SNES's planar format"""
    out = bytearray()
    for pair in range(bpp // 2):
        for _ in range(8):
            out += bytes([0xFF if c >> (2 * pair) & 1 else 0, 0xFF if c >> (2 * pair + 1) & 1 else 0])
    return bytes(out)


def checker_tile(bpp, c):
    """colour c on a checker, texel 0 between"""
    out = bytearray()
    for pair in range(bpp // 2):
        for row in range(8):
            m = 0xAA if row & 1 else 0x55
            out += bytes([m if c >> (2 * pair) & 1 else 0, m if c >> (2 * pair + 1) & 1 else 0])
    return bytes(out)

BPP = [[2, 2, 2, 2], [4, 4, 2, 0], [4, 4, 0, 0], [8, 4, 0, 0], [8, 2, 0, 0], [4, 2, 0, 0], [4, 0, 0, 0], [8, 0, 0, 0]]
CHAR_BASE = lambda n: (n + 1) * 0x1000
MAP_BASE = lambda n: 0x6000 + n * 0x400
SIZES = {0: (1, 2), 1: (1, 4), 2: (1, 8), 3: (2, 4), 4: (2, 8), 5: (4, 8)}    # OBSEL's size selects 0-5: the tiles a side


class Scene:
    def __init__(self, name, seed=None):
        self.name = name
        self.r = random.Random(seed if seed is not None else name)
        self.t = snestest.Test(name.replace("-", "_"), frames=6, shots=(6,), rom_size=0x80000)
        self.f = []
        self.next_id = 1
        self.tm = self.ts = 0
        pal = bytearray()
        for _ in range(256):
            c = self.r.getrandbits(15)
            pal += bytes([c & 255, c >> 8])
        self.t.cgram(0, pal)
        self.f.append("pal " + pal.hex())
        self.screen = 15
        self.blank = False
        self.overscan = False
        self.direct = 0
        self.hdma_mask = 0
        self.next_ch = 1
        self.tables = []                # each HDMA channel's setup: (channel, mode, B-bus register, table bytes)
        self.vblank = []                # register writes the CPU makes in the vertical blank after the picture is on
        self.w_sel = [0] * 6

    def id(self):
        self.next_id += 1
        return self.next_id - 1

    def mode(self, m, bg3=0):
        self.m = m
        self.t.w(0x2105, m | bg3 << 3)
        self.f.append(f"mode {m} {bg3} 0")

    def bg(self, n, scroll=(0, 0), big=False, mosaic=1, main=True, sub=False, direct=False):
        bpp = BPP[self.m][n]
        count = 128
        data = self.r.randbytes(bpp * 8 * count)
        tid, mid = self.id(), self.id()
        self.t.vram(CHAR_BASE(n), data)
        words = bytearray()
        for _ in range(32 * 32):
            # a 16-wide or big tile also reads the tiles after its own: all within the set
            e = self.r.randrange(count - 17 if big or self.m in (5, 6) else count) | self.r.randrange(8) << 10 | self.r.getrandbits(1) << 13 | self.r.getrandbits(2) << 14
            words += bytes([e & 255, e >> 8])
        self.t.vram(MAP_BASE(n), words)
        self.t.w(0x2107 + n, (MAP_BASE(n) >> 10) << 2)
        nba = 0x210B + (n >> 1)
        self.nba = getattr(self, "nba", {})
        self.nba[nba] = self.nba.get(nba, 0) | (CHAR_BASE(n) >> 12) << (4 * (n & 1))
        self.t.w(nba, self.nba[nba])
        if big:
            self.bigs = getattr(self, "bigs", 0) | 1 << (4 + n)
            self.t.w(0x2105, self.m | self.bigs)
        self.t.w16(0x210D + 2 * n, scroll[0])
        self.t.w16(0x210E + 2 * n, scroll[1])
        self.tm |= (1 if main else 0) << n
        self.ts |= (1 if sub else 0) << n
        if mosaic > 1:
            self.mos = getattr(self, "mos", 0) | 1 << n | (mosaic - 1) << 4
            self.t.w(0x2106, self.mos)
        self.f.append(f"tiles {tid} {bpp} {count} {data.hex()}")
        self.f.append(f"map {mid} 32 32 {words.hex()}")
        self.f.append(f"layer {n} {tid} {mid} {int(main)} {int(sub)} {16 if big else 8} {scroll[0]} {scroll[1]} {mosaic} 1 {int(direct)} -1")
        return tid, mid

    def sprites(self, count, large=False, sub=False, limit=False, main=True):
        data = self.r.randbytes(32 * 256)
        tid = self.id()
        self.t.vram(0, data)
        self.t.w(0x2101, 0)            # 8 and 16, names at word 0
        self.f.append(f"tiles {tid} 4 256 {data.hex()}")
        oam, hi = bytearray(), [0] * 32
        for i in range(128):
            if i < count:
                x, y = self.r.randrange(256), self.r.randrange(224)
                big = large and self.r.getrandbits(1)
                tile, pal, prio, h, v = self.r.randrange(256), self.r.randrange(8), self.r.randrange(4), self.r.getrandbits(1), self.r.getrandbits(1)
                oam += bytes([x, y, tile, pal << 1 | prio << 4 | h << 6 | v << 7])
                hi[i >> 2] |= int(big) << (2 * (i & 3) + 1)
                sz = 2 if big else 1
                self.f.append(f"sprite {x} {y} {sz} {sz} {tid} {tile} {pal} {prio} {h} {v}")
            else:
                oam += bytes([0, 0xE0, 0, 0])
        self.t.oam(bytes(oam) + bytes(hi))
        self.tm |= (1 if main else 0) << 4
        self.ts |= (1 if sub else 0) << 4
        self.f.append(f"spriteshow {int(main)} {int(sub)}")
        self.f.append("limits 32 34 1")

    def sprites_band(self):
        """more on a line than the SNES draws"""
        data = self.r.randbytes(32 * 256)
        tid = self.id()
        self.t.vram(0, data)
        self.t.w(0x2101, 0)
        self.f.append(f"tiles {tid} 4 256 {data.hex()}")
        oam = bytearray()
        for i in range(128):
            x, y = self.r.randrange(240), 90 + self.r.randrange(30)
            tile, pal, prio = self.r.randrange(256), self.r.randrange(8), self.r.randrange(4)
            oam += bytes([x, y, tile, pal << 1 | prio << 4])
            self.f.append(f"sprite {x} {y} 1 1 {tid} {tile} {pal} {prio} 0 0")
        self.t.oam(bytes(oam) + bytes(32))
        self.tm |= 0x10
        self.f.append("limits 32 34 1")

    def window(self, l0, r0, l1, r1, sels, logic=0, mask_main=0, mask_sub=0, logics=None):
        """sels: six SNES selector nibbles, for BG1-4, sprites, colour; logics: a logic for each of the six
        (0 OR, 1 AND, 2 XOR, 3 XNOR), else `logic` for all"""
        logics = list(logics) if logics else [logic] * 6
        self.t.w(0x2126, l0); self.t.w(0x2127, r0); self.t.w(0x2128, l1); self.t.w(0x2129, r1)
        self.t.w(0x2123, sels[0] | sels[1] << 4)
        self.t.w(0x2124, sels[2] | sels[3] << 4)
        self.t.w(0x2125, sels[4] | sels[5] << 4)
        self.t.w(0x212A, logics[0] | logics[1] << 2 | logics[2] << 4 | logics[3] << 6)
        self.t.w(0x212B, logics[4] | logics[5] << 2)
        self.t.w(0x212E, mask_main)
        self.t.w(0x212F, mask_sub)
        def use(s, lg):
            return (2 and (s >> 1 & 1)) | (s & 1) << 1 | (s >> 3 & 1) << 2 | (s >> 2 & 1) << 3 | lg << 4
        mm = " ".join(str(mask_main >> i & 1) for i in range(5))
        ms = " ".join(str(mask_sub >> i & 1) for i in range(5))
        self.f.append(f"win {l0} {r0} {l1} {r1} " + " ".join(str(use(q, lg)) for q, lg in zip(sels, logics)) + f" {mm} {ms}")

    def blend(self, sub=False, half=False, fixed=False, layers=0x3F, clip=0, prevent=0, colour=(8, 4, 2)):
        self.t.w(0x2130, clip << 6 | prevent << 4 | (0 if fixed else 2) | self.direct)
        self.t.w(0x2131, int(sub) << 7 | int(half) << 6 | layers)
        self.t.w(0x2132, 0x20 | colour[0])
        self.t.w(0x2132, 0x40 | colour[1])
        self.t.w(0x2132, 0x80 | colour[2])
        c = colour[0] | colour[1] << 5 | colour[2] << 10
        self.f.append(f"blend {int(sub)} {int(half)} {int(fixed)} {layers} {clip} {prevent} {c}")

    def mode7(self, a, b, c, d, cx=128, cy=112, sx=0, sy=0, outside=0, fx=0, fy=0, ext=False, direct=False, mosaic=1):
        self.mode(7)
        chars = self.r.randbytes(256 * 64)
        tiles = bytes(self.r.randrange(256) for _ in range(128 * 128))
        tid, mid = self.id(), self.id()
        v = bytearray(0x8000)
        v[0::2] = tiles + bytes(0x4000 - len(tiles))
        v[1::2] = chars
        self.t.vram(0, v)
        self.t.w(0x211A, outside_sel(outside) | fx | fy << 1)
        for r, val in ((0x211B, a), (0x211C, b), (0x211D, c), (0x211E, d), (0x211F, cx), (0x2120, cy)):
            self.t.w16(r, val & 0xFFFF)
        self.t.w16(0x210D, sx & 0xFFFF)
        self.t.w16(0x210E, sy & 0xFFFF)
        if ext:
            self.t.w(0x2133, 0x40)
            self.tm |= 2
        if direct:
            self.direct = 1
            self.t.w(0x2130, 1)
        if mosaic > 1:
            self.t.w(0x2106, 1 | (mosaic - 1) << 4)
        self.tm |= 1
        words = b"".join(bytes([x, 0]) for x in tiles)
        self.f.append(f"tiles7 {tid} 8 256 {chars.hex()}")
        self.f.append(f"map {mid} 128 128 {words.hex()}")
        self.f.append(f"layer 0 0 0 1 0 8 0 0 {mosaic} 0 {int(direct)} -1")
        if ext:
            self.f.append("layer 1 0 0 1 0 8 0 0 1 0 0 -1")
        self.f.append(f"m7 {a} {b} {c} {d} {cx} {cy} {sx} {sy} {outside} {fx} {fy} {int(ext)} {tid} {mid}")

    def scroll_by_line(self, n, values, field):
        """HDMA on one channel; the engine's table by line"""
        self.reg_by_line(0x0D + 2 * n, values, field + n, twice=True)

    def reg_by_line(self, reg, values, field, ev=None, twice=False):
        """a register of the PPU set for each line from line 0 by HDMA (one byte, or twice for a
        register written twice), on the next free channel; the engine's `lines` field with its own
        values (ev, else the same)"""
        ch = self.next_ch
        self.next_ch += 1
        table = bytearray()
        for v in values:
            table += bytes([1, v & 255, v >> 8 & 255]) if twice else bytes([1, v & 255])
        table += b"\0"
        self.t.hdma(ch, 0x02 if twice else 0x00, reg & 255, table)
        self.tables.append((ch, 0x02 if twice else 0x00, reg & 255, bytes(table)))
        self.hdma_mask |= 1 << ch
        ev = values if ev is None else ev
        self.f.append(f"lines {field} 0 {len(values)} " + " ".join(str(v) for v in ev))

    def palette_by_line(self, entry, colours):
        """one palette entry's colour for each line: two channels, the address then the colour"""
        a = self.next_ch
        self.next_ch += 1
        b = self.next_ch
        self.next_ch += 1
        t1 = bytearray()
        t2 = bytearray()
        for c in colours:
            t1 += bytes([1, entry])
            t2 += bytes([1, c & 255, c >> 8 & 255])
        self.t.hdma(a, 0x00, 0x21, bytes(t1) + b"\0")
        self.t.hdma(b, 0x02, 0x22, bytes(t2) + b"\0")
        self.tables.append((a, 0x00, 0x21, bytes(t1) + b"\0"))
        self.tables.append((b, 0x02, 0x22, bytes(t2) + b"\0"))
        self.hdma_mask |= 1 << a | 1 << b
        self.f.append(f"lines {LINE['NSHVC_LINE_PALETTE'] + entry} 0 {len(colours)} " + " ".join(str(c) for c in colours))

    def bg_labelled(self, n, tile_bytes, entry, count):
        """a layer whose tiles and map the scene gives exactly: tile_bytes the set's planar data, entry(x, y)
        a map word"""
        bpp = BPP[self.m][n]
        tid, mid = self.id(), self.id()
        self.t.vram(CHAR_BASE(n), tile_bytes)
        words = bytearray()
        for y in range(32):
            for x in range(32):
                e = entry(x, y)
                words += bytes([e & 255, e >> 8])
        self.t.vram(MAP_BASE(n), words)
        self.t.w(0x2107 + n, (MAP_BASE(n) >> 10) << 2)
        nba = 0x210B + (n >> 1)
        self.nba = getattr(self, "nba", {})
        self.nba[nba] = self.nba.get(nba, 0) | (CHAR_BASE(n) >> 12) << (4 * (n & 1))
        self.t.w(nba, self.nba[nba])
        self.tm |= 1 << n
        self.f.append(f"tiles {tid} {bpp} {count} {tile_bytes.hex()}")
        self.f.append(f"map {mid} 32 32 {words.hex()}")
        self.f.append(f"layer {n} {tid} {mid} 1 0 8 0 0 1 1 0 -1")

    def sprites_given(self, tile_bytes, count, sprites, sel=0, limit=True):
        """sprites the scene gives exactly, in OAM order: (x, y, big, tile, pal, prio, h, v); tile_bytes the
        sprite tiles from VRAM word 0 (count of them, 256 or 512), sel the size select, OBSEL's bits 5-7"""
        tid = self.id()
        self.t.vram(0, tile_bytes)
        self.t.w(0x2101, sel << 5)
        small, large = SIZES[sel]
        self.f.append(f"tiles {tid} 4 {count} {tile_bytes.hex()}")
        oam, hi = bytearray(), [0] * 32
        for i in range(128):
            if i < len(sprites):
                x, y, big, tile, pal, prio, h, v = sprites[i]
                oam += bytes([x & 255, y, tile & 255, tile >> 8 & 1 | pal << 1 | prio << 4 | h << 6 | v << 7])
                hi[i >> 2] |= int(big) << (2 * (i & 3) + 1)
                sz = large if big else small
                self.f.append(f"sprite {x} {y} {sz} {sz} {tid} {tile} {pal} {prio} {h} {v}")
            else:
                oam += bytes([0, 0xE0, 0, 0])
        self.t.oam(bytes(oam) + bytes(hi))
        self.tm |= 0x10
        self.f.append(f"spriteshow 1 0")
        if limit:
            self.f.append("limits 32 34 1")

    def sprites_random(self, sel, second=False, count=128):
        """random sprites of one size select, over a random tile sheet of 256 tiles (512 with the second table)"""
        n = 512 if second else 256
        data = self.r.randbytes(32 * n)
        sprites = []
        for i in range(count):
            sprites.append((self.r.randrange(256), self.r.randrange(224), self.r.getrandbits(1), self.r.randrange(256) | (self.r.getrandbits(1) << 8 if second else 0),
                            self.r.randrange(8), self.r.randrange(4), self.r.getrandbits(1), self.r.getrandbits(1)))
        self.sprites_given(data, n, sprites, sel)

    def palette_spans(self, entry, spans):
        """one palette entry's colour for spans of lines by HDMA: spans (lines, colour), each a single table
        entry that holds for all its lines (the data is sent on its first line only)"""
        a = self.next_ch
        self.next_ch += 1
        b = self.next_ch
        self.next_ch += 1
        t1 = bytearray()
        t2 = bytearray()
        ev = []
        for n, c in spans:
            assert 1 <= n <= 127
            t1 += bytes([n, entry])
            t2 += bytes([n, c & 255, c >> 8 & 255])
            ev += [c] * n
        assert len(ev) == 224
        self.t.hdma(a, 0x00, 0x21, bytes(t1) + b"\0")
        self.t.hdma(b, 0x02, 0x22, bytes(t2) + b"\0")
        self.tables.append((a, 0x00, 0x21, bytes(t1) + b"\0"))
        self.tables.append((b, 0x02, 0x22, bytes(t2) + b"\0"))
        self.hdma_mask |= 1 << a | 1 << b
        self.f.append(f"lines {LINE['NSHVC_LINE_PALETTE'] + entry} 0 224 " + " ".join(str(c) for c in ev))

    def palette_cpu(self, entry, colour):
        """one palette entry's colour for the whole picture, written by the CPU in a vertical blank (the address,
        then the colour's two bytes)"""
        self.vblank += [(0x2121, entry), (0x2122, colour & 255), (0x2122, colour >> 8 & 255)]
        self.f.append(f"lines {LINE['NSHVC_LINE_PALETTE'] + entry} 0 224 " + " ".join([str(colour)] * 224))

    def dump(self, path):
        """the test ROM's step table and its HDMA tables, as bytes, in text"""
        with open(path, "w") as f:
            f.write(f"steps {len(self.t.steps)} bytes\n{bytes(self.t.steps).hex()}\n")
            f.write(f"irq steps {len(self.t.irq_steps)} bytes\n{bytes(self.t.irq_steps).hex()}\n")
            f.write(f"nmi steps {len(self.t.nmi_steps)} bytes\n{bytes(self.t.nmi_steps).hex()}\n")
            for ch, mode, reg, table in self.tables:
                f.write(f"hdma channel {ch} mode {mode} register 0x21{reg:02x} table {len(table)} bytes\n{table.hex()}\n")
            f.write(f"hdma enable mask 0x{self.hdma_mask:02x}\n")
            for o, data in self.t.blobs:
                f.write(f"blob at file offset 0x{o:x}, cpu address 0x{self.t.cpu_address(o):06x}, {len(data)} bytes\n{data.hex()}\n")

    def finish(self, outdir, dump=False):
        self.t.w(0x212C, self.tm)
        self.t.w(0x212D, self.ts)
        self.t.show(self.screen | (0x80 if self.blank else 0))
        if self.hdma_mask:
            self.t.w(0x420C, self.hdma_mask)
        if self.vblank:
            self.t.wait_frames(1)               # the start of a vertical blank: the writes go where the address says
            for addr, v in self.vblank:
                self.t.w(addr, v)
        self.t.wait_frames(250)
        self.t.wait_frames(250)
        self.f.insert(0, f"screen {self.screen} {int(getattr(self, 'interlaced', False))} {int(self.blank)} {int(self.overscan)}")
        os.makedirs(outdir, exist_ok=True)
        path = os.path.join(outdir, self.name + ".scene")
        with open(path, "w") as f:
            f.write("\n".join(self.f) + "\n")
        if dump:
            self.dump(os.path.join(outdir, self.name + ".rom.txt"))
        return self.t.build(outdir), path


def opt_scene(mode, sub=False):
    """sub: BG1 on the sub screen too (in a hires mode, so that the even half-columns hold it)"""
    s = Scene(f"offset-per-tile{mode}")
    s.mode(mode)
    tid0, mid0 = s.bg(0, scroll=(13, 7), sub=sub)
    words = bytearray()
    for _ in range(32 * 32):
        e = s.r.randrange(1024) | s.r.getrandbits(3) << 13
        words += bytes([e & 255, e >> 8])
    mid = s.id()
    s.t.vram(MAP_BASE(2), words)
    s.t.w(0x2109, (MAP_BASE(2) >> 10) << 2)
    s.t.w16(0x2111, 5)
    s.t.w16(0x2112, 3)
    s.f.append(f"map {mid} 32 32 {words.hex()}")
    # BG1 reads it; BG3 is not drawn but holds its scroll
    for i, line in enumerate(s.f):
        if line.startswith("layer 0 "):
            s.f[i] = line.rsplit(" ", 1)[0] + f" {mid}"
    s.f.append(f"layer 2 0 {mid} 0 0 8 5 3 1 1 0 -1")
    return s


def outside_sel(o):
    return {0: 0, 1: 0x80, 2: 0xC0}[o]


def scenes():
    for m in range(7):
        for name, big in (("", False), ("-big", True)):
            s = Scene(f"mode{m}{name}")
            s.mode(m)
            for n in range(4):
                if BPP[m][n]:
                    s.bg(n, scroll=(s.r.randrange(1024), s.r.randrange(1024)), big=big, main=True)
            yield s
    s = Scene("sprites"); s.mode(1); s.bg(0); s.sprites(128, large=True); yield s
    s = Scene("sprites-few"); s.mode(0); s.sprites(16); yield s
    s = Scene("mosaic"); s.mode(1); s.bg(0, mosaic=4); s.bg(1, mosaic=4); yield s
    s = Scene("window"); s.mode(1); s.bg(0); s.bg(1); s.sprites(32); s.window(40, 200, 90, 160, [2, 8, 10, 0, 2, 0], 0, 3, 0); yield s
    s = Scene("window-and"); s.mode(1); s.bg(0); s.bg(1); s.window(40, 200, 90, 160, [10, 3, 0, 0, 0, 0], 1, 3, 0); yield s
    s = Scene("sprites-blend-main"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True); s.sprites(64); s.blend(); yield s
    s = Scene("sprites-blend-both"); s.mode(1); s.bg(0); s.sprites(64, sub=True); s.blend(half=True); yield s
    s = Scene("sprites-sub-only"); s.mode(1); s.bg(0); s.sprites(64, main=False, sub=True); s.blend(); yield s
    s = Scene("add-sub"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True); s.blend(); yield s
    s = Scene("sub-half"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True); s.blend(sub=True, half=True); yield s
    s = Scene("add-fixed"); s.mode(1); s.bg(0); s.blend(fixed=True); yield s
    s = Scene("colour-window"); s.mode(1); s.bg(0); s.window(60, 180, 0, 0, [0, 0, 0, 0, 0, 2]); s.blend(fixed=True, clip=1, prevent=2); yield s
    s = Scene("mode7"); s.mode7(256, 0, 0, 256); yield s
    s = Scene("mode7-rot"); s.mode7(181, -181, 181, 181, sx=40, sy=30); yield s
    s = Scene("mode7-clear"); s.mode7(90, 40, -40, 90, outside=1); yield s
    s = Scene("mode7-tile0"); s.mode7(90, 40, -40, 90, outside=2); yield s
    s = Scene("mode7-flip"); s.mode7(256, 0, 0, 256, fx=1, fy=1); yield s
    s = Scene("direct-colour"); s.mode(3); s.bg(0, direct=True); s.t.w(0x2130, 1); yield s
    s = Scene("bg3-priority"); s.mode(1, 1); s.bg(0); s.bg(1); s.bg(2); yield s
    s = Scene("pseudo-hires"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True); s.t.w(0x2133, 8); s.f.append("mode 1 0 1"); yield s
    for m in (2, 4): yield opt_scene(m)
    s = Scene("sprites-limit"); s.mode(0); s.sprites_band(); yield s
    s = Scene("interlace"); s.mode(5); s.bg(0); s.bg(1); s.t.w(0x2133, 1); s.interlaced = True; yield s
    s = Scene("hdma-scroll"); s.mode(1); s.bg(0); s.scroll_by_line(0, [(i * 3) & 1023 for i in range(224)], 0); yield s


def priority_scene(name, mode, bg3=0):
    """labelled content: every layer solid, its two tile priorities in blocks of 4 x 4 tiles in a checker,
    and sprites of each of the four priorities over them, so each layer-and-priority against each sprite
    priority is seen; no sprite overlaps another"""
    s = Scene(name)
    s.mode(mode, bg3)
    for n in range(4):
        bpp = BPP[mode][n]
        if bpp:
            tiles = solid_tile(bpp, 1) * 2
            s.bg_labelled(n, tiles, lambda x, y, n=n: 1 | n << 10 | ((x // 4 + y // 4 + n) & 1) << 13, 2)
    sheet = solid_tile(4, 1) * 256
    sprites = []
    for i in range(32):
        sprites.append((12 + (i % 8) * 30, 10 + (i // 8) * 52, 0, 0, i % 8, i // 8, 0, 0))
    s.sprites_given(sheet, 256, sprites)
    return s


def first_sprite_scene():
    """labelled content: pairs of sprites on one spot, the earlier in OAM and the later with every pair of
    priorities, one pair in four with the earlier a checker (texel 0 between) and a solid layer under
    them: the first sprite to show a pixel keeps it, whatever the priorities and the layer"""
    s = Scene("sprite-first")
    s.mode(1)
    s.bg_labelled(0, solid_tile(4, 1) * 2, lambda x, y: 1 | ((x // 3 + y // 3) & 1) << 13, 2)
    sheet = bytearray()
    for t in range(256):
        sheet += checker_tile(4, 2) if t % 16 in (4, 5) else solid_tile(4, 1)
    sprites = []
    for i in range(16):
        pa, pb = i // 4, i % 4
        x, y = 16 + (i % 8) * 28, 20 + (i // 8) * 80
        sprites.append((x, y, 0, 4 if i & 1 else 0, i % 8, pa, 0, 0))
        sprites.append((x + 5, y + 3, 0, 0 if i & 1 else 4, (i + 3) % 8, pb, 0, 0))
    s.sprites_given(bytes(sheet), 256, sprites)
    return s


def more_scenes():
    # ---- colour maths ----
    for c in range(4):
        s = Scene(f"cmath-clip-{c}"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True)
        s.window(60, 180, 0, 0, [0, 0, 0, 0, 0, 2]); s.blend(clip=c); yield s
    for c in range(1, 4):
        s = Scene(f"cmath-prevent-{c}"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True)
        s.window(60, 180, 0, 0, [0, 0, 0, 0, 0, 2]); s.blend(prevent=c); yield s
    for name, layers in (("bg1", 0x01), ("bg2", 0x02), ("sprites", 0x10), ("backdrop", 0x20)):
        s = Scene(f"cmath-only-{name}"); s.mode(1); s.bg(0, sub=True); s.bg(1, sub=True); s.sprites(48, sub=True)
        s.blend(layers=layers); yield s
    s = Scene("cmath-sub-backdrop"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True); s.blend(half=True, colour=(20, 6, 11)); yield s
    s = Scene("cmath-subtract"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True); s.blend(sub=True); yield s
    s = Scene("cmath-hires"); s.mode(5); s.bg(0); s.bg(1, main=False, sub=True); s.blend(); yield s
    # flags a hires row's even half-columns take from the dot before: layers that take part and not, a colour window
    s = Scene("cmath-hires-flags"); s.mode(5); s.bg(0, sub=True); s.bg(1, sub=True)
    s.window(61, 181, 0, 0, [0, 0, 0, 0, 0, 2]); s.blend(layers=0x01, prevent=2); yield s
    s = Scene("cmath-hires-clip"); s.mode(5); s.bg(0, sub=True); s.bg(1, sub=True)
    s.window(61, 181, 0, 0, [0, 0, 0, 0, 0, 2]); s.blend(clip=1, half=True); yield s
    s = Scene("cmath-pseudo-hires"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True); s.blend(half=True)
    s.t.w(0x2133, 8); s.f.append("mode 1 0 1"); yield s
    for b in (11, 7, 3, 1, 0):
        s = Scene(f"brightness-{b}"); s.mode(1); s.bg(0); s.bg(1); s.sprites(32); s.screen = b; yield s
    s = Scene("blank"); s.mode(1); s.bg(0); s.bg(1); s.blank = True; yield s
    # ---- windows ----
    s = Scene("window-xor"); s.mode(1); s.bg(0); s.bg(1); s.sprites(32); s.window(40, 200, 90, 160, [10, 10, 0, 0, 10, 0], 2, 3, 0); yield s
    s = Scene("window-xnor"); s.mode(1); s.bg(0); s.bg(1); s.sprites(32); s.window(40, 200, 90, 160, [10, 10, 0, 0, 10, 0], 3, 3, 0); yield s
    s = Scene("window-w2-invert"); s.mode(1); s.bg(0); s.bg(1); s.window(40, 200, 90, 160, [8 | 4, 2 | 8 | 4, 0, 0, 0, 0], 0, 3, 0); yield s
    s = Scene("window-logics"); s.mode(0); s.bg(0); s.bg(1); s.bg(2); s.sprites(32)
    s.window(40, 200, 90, 160, [10, 10, 10, 0, 10, 0], 0, 0x17, 0, logics=[0, 1, 2, 0, 3, 0]); yield s
    s = Scene("window-sub-mask"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True); s.sprites(32, main=False, sub=True)
    s.window(40, 200, 90, 160, [0, 2, 0, 0, 2, 0], 0, 0, 0x12); s.blend(); yield s
    # ---- Mode 7, whole-number scale, no rotation ----
    s = Scene("mode7-line-matrix"); s.mode7(256, 0, 0, 256)
    vals = [0x80 if (i // 28) & 1 else 0x100 for i in range(224)]
    s.reg_by_line(0x1B, vals, LINE["NSHVC_LINE_M7_A"], twice=True); s.reg_by_line(0x1E, vals, LINE["NSHVC_LINE_M7_D"], twice=True); yield s
    s = Scene("mode7-line-scroll"); s.mode7(256, 0, 0, 256)
    vals = [(i * 2) & 1023 for i in range(224)]
    s.reg_by_line(0x0D, vals, LINE["NSHVC_LINE_M7_SCROLL_X"], twice=True); yield s
    s = Scene("mode7-extbg"); s.mode7(256, 0, 0, 256, ext=True); yield s
    s = Scene("mode7-extbg-maths"); s.mode7(256, 0, 0, 256, ext=True); s.blend(fixed=True, layers=0x03); yield s
    s = Scene("mode7-mosaic"); s.mode7(256, 0, 0, 256, mosaic=4); yield s
    s = Scene("mode7-window-maths"); s.mode7(256, 0, 0, 256); s.window(40, 200, 0, 0, [2, 0, 0, 0, 0, 0], 0, 1, 0)
    s.blend(fixed=True, layers=0x21, colour=(12, 3, 9)); yield s
    s = Scene("mode7-direct"); s.mode7(256, 0, 0, 256, direct=True); yield s
    # ---- by line ----
    s = Scene("line-scroll-y-bg2"); s.mode(1); s.bg(0); s.bg(1)
    s.reg_by_line(0x0E + 2, [(i * 5) & 1023 for i in range(224)], LINE["NSHVC_LINE_SCROLL_Y"] + 1, twice=True); yield s
    s = Scene("line-scroll-x-bg2"); s.mode(1); s.bg(0); s.bg(1)
    s.scroll_by_line(1, [(i * 3 + 40) & 1023 for i in range(224)], 0); yield s
    s = Scene("line-palette"); s.mode(1); s.bg(0); s.bg(1)
    cols = [s.r.getrandbits(15) for _ in range(224)]
    s.palette_by_line(5, cols); yield s
    # the same picture as line-palette, an entry no texel uses changed on every line: a leak would show
    s = Scene("line-palette-unused", seed="line-palette"); s.mode(1); s.bg(0); s.bg(1)
    cols = [s.r.getrandbits(15) for _ in range(224)]
    s.palette_by_line(200, cols); yield s
    # the per-line palette's two other ways: the CPU in a vertical blank, and an HDMA entry holding for 100 lines
    s = Scene("line-palette-cpu", seed="line-palette"); s.mode(1); s.bg(0); s.bg(1)
    cols = [s.r.getrandbits(15) for _ in range(224)]
    s.palette_cpu(5, cols[0]); yield s
    s = Scene("line-palette-span", seed="line-palette"); s.mode(1); s.bg(0); s.bg(1)
    cols = [s.r.getrandbits(15) for _ in range(224)]
    s.palette_spans(5, [(100, cols[0]), (124, cols[1])]); yield s
    s = Scene("line-window-edges"); s.mode(1); s.bg(0); s.bg(1); s.window(40, 200, 0, 0, [2, 2, 0, 0, 0, 0], 0, 3, 0)
    s.reg_by_line(0x26, [20 + (i * 2) % 120 for i in range(224)], LINE["NSHVC_LINE_WIN1_LEFT"]); yield s
    s = Scene("line-blend"); s.mode(1); s.bg(0); s.bg(1, main=False, sub=True); s.blend()
    vals = [(i // 14 & 1) << 7 | (i // 28 & 1) << 6 | (0x3F if (i // 7) & 1 else 0x21) for i in range(224)]
    s.reg_by_line(0x31, vals, LINE["NSHVC_LINE_SUBTRACT"], [v >> 7 & 1 for v in vals])
    s.f.append(f"lines {LINE['NSHVC_LINE_HALF']} 0 224 " + " ".join(str(v >> 6 & 1) for v in vals))
    s.f.append(f"lines {LINE['NSHVC_LINE_BLEND_LAYERS']} 0 224 " + " ".join(str(v & 0x3F) for v in vals)); yield s
    s = Scene("line-mode"); s.mode(1); s.bg(0); s.bg(1)
    vals = [2 if (i // 28) & 1 else 1 for i in range(224)]
    s.reg_by_line(0x05, vals, LINE["NSHVC_LINE_MODE"]); yield s
    s = Scene("line-brightness"); s.mode(1); s.bg(0); s.bg(1)
    vals = [15 - (i // 15) for i in range(224)]
    s.reg_by_line(0x00, vals, LINE["NSHVC_LINE_BRIGHTNESS"]); yield s
    s = Scene("line-blank"); s.mode(1); s.bg(0); s.bg(1)
    vals = [0x8F if (i // 20) & 1 else 0x0F for i in range(224)]
    s.reg_by_line(0x00, vals, LINE["NSHVC_LINE_BRIGHTNESS"], [v & 15 for v in vals])
    s.f.append(f"lines {LINE['NSHVC_LINE_BLANK']} 0 224 " + " ".join(str(v >> 7 & 1) for v in vals)); yield s
    # ---- sprites ----
    for sel in range(6):
        s = Scene(f"sprites-size{sel}"); s.mode(1); s.sprites_random(sel); yield s
    s = Scene("sprites-second-table"); s.mode(1); s.sprites_random(0, second=True); yield s
    s = Scene("sprites-second-table-large"); s.mode(1); s.sprites_random(5, second=True, count=48); yield s
    s = Scene("sprites-flips-large"); s.mode(1)
    sheet = s.r.randbytes(32 * 256)
    sprites = [(10 + (i % 4) * 60, 12 + (i // 4) * 100, 1, 0 if i < 4 else 8, 3, 0, i & 1, i >> 1 & 1) for i in range(8)]
    s.sprites_given(sheet, 256, sprites, sel=5); yield s
    yield priority_scene("sprite-priority-mode0", 0)
    yield priority_scene("sprite-priority-mode1", 1)
    yield priority_scene("sprite-priority-mode1-bg3", 1, 1)
    yield first_sprite_scene()
    # ---- modes ----
    yield opt_scene(6, sub=True)
    s = Scene("direct-colour-mode4"); s.mode(4); s.bg(0, direct=True); s.bg(1); s.direct = 1; s.t.w(0x2130, 1); yield s
    s = Scene("overscan"); s.mode(1); s.bg(0, scroll=(11, 5)); s.bg(1, scroll=(70, 33)); s.sprites(32)
    s.t.w(0x2133, 4); s.overscan = True; yield s


def all_scenes():
    yield from scenes()
    yield from more_scenes()


def run(cmd, env=None):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60, env=env)
    if r.returncode:
        raise RuntimeError(f"{' '.join(cmd)}: {r.stdout}{r.stderr}")


def ppm_size(path):
    """the width and height in a binary PPM's header"""
    with open(path, "rb") as f:
        head = f.read(32).split()
    return int(head[1]), int(head[2])


def compare(a, b):
    with open(a, "rb") as f:
        x = f.read()
    with open(b, "rb") as f:
        y = f.read()
    if x == y:
        return None
    if len(x) != len(y):
        return f"sizes {len(x)} and {len(y)}"
    diff = sum(1 for p, q in zip(x[15:], y[15:]) if p != q)
    return f"{diff // 3 + 1} pixels differ"


# Scenes whose pictures cannot match the reference's byte for byte, with the
# reason. A scene here is reported as KNOWN, never as passing, and one that
# starts to match is reported, so the list is kept true.
AA = ("Mode 7 is anti-aliased at every window size: a pixel that straddles texels mixes them, "
      "where the console takes one texel; not held by any check yet")
KNOWN = {"mode7-rot": AA, "mode7-clear": AA, "mode7-tile0": AA}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oracle", required=True)
    ap.add_argument("--core", required=True)
    ap.add_argument("--player", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--prove", action="store_true")
    ap.add_argument("--dump-rom", action="store_true", help="beside each scene's files, NAME.rom.txt: the test ROM's step table and HDMA tables as bytes")
    a = ap.parse_args()
    only = set(filter(None, a.only.split(",")))
    failed = passed = known = 0
    for s in all_scenes():
        if only and s.name not in only:
            continue
        od = os.path.join(a.out, s.name)
        rom, scene = s.finish(od, a.dump_rom)
        if a.prove:
            with open(scene) as f:
                text = f.read()
            with open(scene, "w") as f:
                f.write(text + "lines 0 0 1 5\n")
        run([a.oracle, "--core", a.core, "--rom", rom, "--frames", "6", "--shot", "6", "--out", od] + (["--overscan"] if s.overscan else []))
        updates = 6 if getattr(s, "interlaced", False) else 1
        w, h = ppm_size(os.path.join(od, "frame_6.ppm"))
        env = dict(os.environ, RR_SIZE=f"{w}x{h}", SCENE_ASPECT="-1")
        run([a.player, "--headless", "--updates", str(updates), "--scene", scene, "--shots", od], env=env)
        why = compare(os.path.join(od, "frame_6.ppm"), os.path.join(od, f"{updates - 1:06d}.ppm"))
        if why and s.name in KNOWN and not a.prove:
            known += 1
            print(f"KNOWN {s.name}: {why} ({KNOWN[s.name]})")
        elif why:
            failed += 1
            print(f"FAIL {s.name}: {why}")
        elif s.name in KNOWN:
            failed += 1
            print(f"FAIL {s.name}: listed as KNOWN but matches: take it off the list")
        else:
            passed += 1
            print(f"ok   {s.name}")
    print(f"{passed} passed, {failed} failed, {known} differ as KNOWN lists")
    if a.prove:
        print("(prove: every scene must fail)" )
        return 0 if not passed and failed else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
