#!/usr/bin/env python3
"""scenes.py: each feature scene is made twice from one description: a test ROM
for bsnes (through tests/oracle) and a scene file for the engine's API
(player.c). The pictures must match byte for byte.

  scenes.py --oracle ORACLE --core CORE --player PLAYER --out DIR [--only A,B]
  scenes.py ... --prove         one scene changed on the engine's side only: must fail
"""
import argparse, os, random, subprocess, sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "rom"))
import snestest

BPP = [[2, 2, 2, 2], [4, 4, 2, 0], [4, 4, 0, 0], [8, 4, 0, 0], [8, 2, 0, 0], [4, 2, 0, 0], [4, 0, 0, 0], [8, 0, 0, 0]]
CHAR_BASE = lambda n: (n + 1) * 0x1000
MAP_BASE = lambda n: 0x6000 + n * 0x400


class Scene:
    def __init__(self, name):
        self.name = name
        self.r = random.Random(name)
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

    def sprites(self, count, large=False, sub=False, limit=False):
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
        self.tm |= 0x10
        self.ts |= (1 if sub else 0) << 4
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

    def window(self, l0, r0, l1, r1, sels, logic=0, mask_main=0, mask_sub=0):
        """sels: six SNES selector nibbles, for BG1-4, sprites, colour"""
        self.t.w(0x2126, l0); self.t.w(0x2127, r0); self.t.w(0x2128, l1); self.t.w(0x2129, r1)
        self.t.w(0x2123, sels[0] | sels[1] << 4)
        self.t.w(0x2124, sels[2] | sels[3] << 4)
        self.t.w(0x2125, sels[4] | sels[5] << 4)
        self.t.w(0x212A, logic * 0x55 & 255)
        self.t.w(0x212B, logic * 5)
        self.t.w(0x212E, mask_main)
        self.t.w(0x212F, mask_sub)
        def use(s):
            return (2 and (s >> 1 & 1)) | (s & 1) << 1 | (s >> 3 & 1) << 2 | (s >> 2 & 1) << 3 | logic << 4
        mm = " ".join(str(mask_main >> i & 1) for i in range(5))
        ms = " ".join(str(mask_sub >> i & 1) for i in range(5))
        self.f.append(f"win {l0} {r0} {l1} {r1} " + " ".join(str(use(s)) for s in sels) + f" {mm} {ms}")

    def blend(self, sub=False, half=False, fixed=False, layers=0x3F, clip=0, prevent=0, colour=(8, 4, 2)):
        self.t.w(0x2130, clip << 6 | prevent << 4 | (0 if fixed else 2))
        self.t.w(0x2131, int(sub) << 7 | int(half) << 6 | layers)
        self.t.w(0x2132, 0x20 | colour[0])
        self.t.w(0x2132, 0x40 | colour[1])
        self.t.w(0x2132, 0x80 | colour[2])
        c = colour[0] | colour[1] << 5 | colour[2] << 10
        self.f.append(f"blend {int(sub)} {int(half)} {int(fixed)} {layers} {clip} {prevent} {c}")

    def mode7(self, a, b, c, d, cx=128, cy=112, sx=0, sy=0, outside=0, fx=0, fy=0, ext=False):
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
        self.tm |= 1
        words = b"".join(bytes([x, 0]) for x in tiles)
        self.f.append(f"tiles7 {tid} 8 256 {chars.hex()}")
        self.f.append(f"map {mid} 128 128 {words.hex()}")
        self.f.append(f"layer 0 0 0 1 0 8 0 0 1 0 0 -1")
        self.f.append(f"m7 {a} {b} {c} {d} {cx} {cy} {sx} {sy} {outside} {fx} {fy} {int(ext)} {tid} {mid}")

    def scroll_by_line(self, n, values, field):
        """HDMA on one channel; the engine's table by line"""
        table = bytearray()
        for v in values:
            table += bytes([1, v & 255, v >> 8 & 255])
        table += b"\0"
        self.t.hdma(1, 0x02, 0x0D + 2 * n, table)
        self.hdma = True
        self.f.append(f"lines {field + n} 0 {len(values)} " + " ".join(str(v) for v in values))

    def finish(self, outdir):
        self.t.w(0x212C, self.tm)
        self.t.w(0x212D, self.ts)
        self.t.show(0x0F)
        if getattr(self, "hdma", False):
            self.t.w(0x420C, 2)
        self.t.wait_frames(250)
        self.t.wait_frames(250)
        self.f.insert(0, f"screen {self.screen} {int(getattr(self, 'interlaced', False))}")
        os.makedirs(outdir, exist_ok=True)
        path = os.path.join(outdir, self.name + ".scene")
        with open(path, "w") as f:
            f.write("\n".join(self.f) + "\n")
        return self.t.build(outdir), path


def opt_scene(mode):
    s = Scene(f"offset-per-tile{mode}")
    s.mode(mode)
    tid0, mid0 = s.bg(0, scroll=(13, 7))
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


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if r.returncode:
        raise RuntimeError(f"{' '.join(cmd)}: {r.stdout}{r.stderr}")


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oracle", required=True)
    ap.add_argument("--core", required=True)
    ap.add_argument("--player", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--only", default="")
    ap.add_argument("--prove", action="store_true")
    a = ap.parse_args()
    only = set(filter(None, a.only.split(",")))
    failed = passed = 0
    for s in scenes():
        if only and s.name not in only:
            continue
        od = os.path.join(a.out, s.name)
        rom, scene = s.finish(od)
        if a.prove:
            with open(scene) as f:
                text = f.read()
            with open(scene, "w") as f:
                f.write(text + "lines 0 0 1 5\n")
        run([a.oracle, "--core", a.core, "--rom", rom, "--frames", "6", "--shot", "6", "--out", od])
        updates = 6 if getattr(s, "interlaced", False) else 1
        run([a.player, "--headless", "--updates", str(updates), "--scene", scene, "--shots", od])
        why = compare(os.path.join(od, "frame_6.ppm"), os.path.join(od, f"{updates - 1:06d}.ppm"))
        if why:
            failed += 1
            print(f"FAIL {s.name}: {why}")
        else:
            passed += 1
            print(f"ok   {s.name}")
    print(f"{passed} passed, {failed} failed")
    if a.prove:
        print("(prove: every scene must fail)" )
        return 0 if not passed and failed else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
