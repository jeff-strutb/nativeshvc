"""
ext_check.py: nshvc_ext_screen held to its two promises, on every test
cartridge (tests/rom/run.py builds them):
  centre     on a larger screen the console's picture, at the centre, is
             exactly the console's own (from nativeshvc at 256 x 224), but
             for a hires line's first half pixel, black at the screen's
             own left edge;
  continues  the extra columns are drawn by the console's rules: a
             cartridge's left 256 columns on a 512-wide screen are the
             console's picture of the same scene scrolled 128 to the left
             (the cartridges that scroll BG1 alone, no windows, sprites,
             offset-per-tile or mosaic: scroll_*).

  python3 tests/rom/ext_check.py
"""
import os, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import run, cases, snestest  # noqa: E402

SIZES = [(512, 224), (320, 256), (1024, 480), (258, 226)]


def shot(rom, screen, d):
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ)
    if screen:
        env["NSHVC_TEST_SCREEN"] = "%dx%d" % screen
    script = os.path.join(d, "s.txt")
    open(script, "w").write("wait 6\nshot f\n")
    subprocess.run([run.CART, "--rom", rom, "--headless", "--script", script, "--out", d], env=env, check=True,
                   capture_output=True, timeout=120)
    return run.ppm(os.path.join(d, "f.ppm"))


def crop(img, x0, y0, w, h):
    W, H, p = img
    return b"".join(p[((y0 + y) * W + x0) * 3:((y0 + y) * W + x0 + w) * 3] for y in range(h))


def main():
    bad = 0
    tests = [t for t in cases.all_tests() if not t.name.startswith(("irq_", "probe_cg", "probe_scroll", "map_"))]
    for t in tests:
        rom = os.path.join(run.OUT, t.name, t.name + ".sfc")
        if not os.path.exists(rom):
            continue
        base = shot(rom, None, os.path.join(run.OUT, t.name, "ext_base"))
        bw, bh, _ = base
        for sz in SIZES:
            if sz[1] < bh:
                continue
            img = shot(rom, sz, os.path.join(run.OUT, t.name, "ext_%dx%d" % sz))
            W, H, _ = img
            wide = 2 if bw == 512 else 1
            # interlaced: the fields woven, twice the screen's lines
            tall = 2 if bh >= 448 else 1
            if (W, H) != (sz[0] * wide, sz[1] * tall):
                print("%-24s %dx%d: the picture is %dx%d" % (t.name, sz[0], sz[1], W, H))
                bad += 1
                continue
            x0 = (sz[0] - 256) // 2 * wide
            # the console's line 1 at the screen's row (lines - console's) / 2
            y0 = (sz[1] - bh // tall) // 2 * tall
            a, b = bytearray(crop(img, x0, y0, bw, bh)), bytearray(base[2])
            if wide == 2 and sz[0] != 256:
                # a hires line's first half pixel is black: the screen's
                # own, which on a larger one is left of the centre
                for y in range(bh):
                    a[y * bw * 3:y * bw * 3 + 3] = b[y * bw * 3:y * bw * 3 + 3]
            if a != b:
                print("%-24s %dx%d: the centre differs from the console's picture" % (t.name, sz[0], sz[1]))
                bad += 1
    print("centre: %d differ" % bad)
    # continues: BG1 alone, scrolled
    cbad = 0
    for i in range(4):
        hofs = 200 + i * 77
        a = scroll_case("scroll_%d" % i, hofs, i)
        b = scroll_case("scroll_%d_left" % i, (hofs - 128) & 0x3FF, i)
        ra, rb = a.build(os.path.join(run.OUT, a.name)), b.build(os.path.join(run.OUT, b.name))
        wide = shot(ra, (512, 224), os.path.join(run.OUT, a.name, "ext"))
        hw = shot(rb, None, os.path.join(run.OUT, b.name, "hw"))
        if crop(wide, 0, 0, 256, 224) != hw[2]:
            print("scroll_%d: the left columns are not the scene scrolled" % i)
            cbad += 1
    print("continues: %d differ" % cbad)
    # pages: a cartridge's VRAM and every layer's reads moved to page 3 of
    # 256 KB give the console's own picture, and read back the same
    pbad = 0
    for t in tests:
        rom = os.path.join(run.OUT, t.name, t.name + ".sfc")
        if not os.path.exists(rom):
            continue
        d0 = os.path.join(run.OUT, t.name, "ext_base")
        d3 = os.path.join(run.OUT, t.name, "ext_page3")
        os.makedirs(d3, exist_ok=True)
        env = dict(os.environ, NSHVC_TEST_VRAM_PAGE="3")
        open(os.path.join(d3, "s.txt"), "w").write("wait 6\nshot f\ndump f\n")
        open(os.path.join(d0, "s.txt"), "w").write("wait 6\nshot f\ndump f\n")
        subprocess.run([run.CART, "--rom", rom, "--headless", "--script", os.path.join(d3, "s.txt"), "--out", d3],
                       env=env, check=True, capture_output=True)
        subprocess.run([run.CART, "--rom", rom, "--headless", "--script", os.path.join(d0, "s.txt"), "--out", d0],
                       check=True, capture_output=True)
        same = all(open(os.path.join(d0, "f" + e), "rb").read() == open(os.path.join(d3, "f" + e), "rb").read()
                   for e in (".ppm", ".wram"))
        v0 = open(os.path.join(d0, "f.vram"), "rb").read()
        v3 = open(os.path.join(d3, "f.vram"), "rb").read()
        if not same or v3[3 * 0x10000:4 * 0x10000] != v0[:0x10000]:
            print("%-24s page 3: differs from page 0" % t.name)
            pbad += 1
    print("pages: %d differ" % pbad)
    # page registers: two scenes, one in each page (written through $21C5),
    # BG1's page switched by HDMA to $21C0 at line 100: the picture is the
    # first scene's above, the second's below
    rbad = 0
    for split in (100, 37):
        a, b = page_scene("pgA", 0, 0), page_scene("pgB", 0, 1)
        both = page_scene("pgAB_%d" % split, split, None)
        ra, rb, rab = (x.build(os.path.join(run.OUT, x.name)) for x in (a, b, both))
        env = {"NSHVC_TEST_VRAM_KB": "128"}
        ia = shot_env(ra, env, os.path.join(run.OUT, a.name, "s"))
        ib = shot_env(rb, env, os.path.join(run.OUT, b.name, "s"))
        iab = shot_env(rab, env, os.path.join(run.OUT, both.name, "s"))
        row = 256 * 3
        want = ia[2][:(split - 1) * row] + ib[2][(split - 1) * row:]
        if iab[2] != want or ia[2] == ib[2]:
            print("page registers, split at line %d: the picture is not the two scenes" % split)
            rbad += 1
    print("page registers: %d differ" % rbad)
    sys.exit(1 if bad or cbad or pbad or rbad else 0)


def shot_env(rom, envd, d):
    os.makedirs(d, exist_ok=True)
    env = dict(os.environ, **envd)
    open(os.path.join(d, "s.txt"), "w").write("wait 6\nshot f\n")
    subprocess.run([run.CART, "--rom", rom, "--headless", "--script", os.path.join(d, "s.txt"), "--out", d], env=env,
                   check=True, capture_output=True, timeout=120)
    return run.ppm(os.path.join(d, "f.ppm"))


def page_scene(name, split, only):
    """BG1 from VRAM page 0 (scene 0) and page 1 (scene 1), written
       through $21C5; shown from page `only`, or switched by HDMA to $21C0
       at line `split` (page 0 above, 1 below)"""
    t = snestest.Test(name, seed=77)
    import random
    scenes = [random.Random(500 + k) for k in range(2)]
    for k in range(2):
        t.w(0x21C5, k)
        t.vram(0x0000, bytes(scenes[k].getrandbits(8) for _ in range(0x10000)))
    t.w(0x21C5, 0)
    t.cgram(0, t.rand_colors(256))
    t.w(0x2105, 0x01)
    t.w(0x2107, 0x11)
    t.w(0x210B, 0x04)
    t.w(0x212C, 0x01)
    if only is not None:
        t.w(0x21C0, only)
    else:
        table = bytes([split - 1, 0, 1, 1, 0])
        t.hdma(5, 0x00, 0xC0, table)
        t.w(0x420C, 0x20)
    t.show()
    return t


def scroll_case(name, hofs, seed):
    t = snestest.Test(name, seed=1000 + seed)
    t.vram(0x0000, t.rand_bytes(0x10000))
    t.cgram(0, t.rand_colors(256))
    t.w(0x2105, 0x01)
    t.w(0x2107, 0x01)          # a 64-wide map
    t.w(0x210B, 0x02)
    t.w16(0x210D, hofs)
    t.w16(0x210E, 37)
    t.w(0x212C, 0x01)
    t.show()
    return t


if __name__ == "__main__":
    snestest.ASAR = os.environ.get("NSHVC_ASAR", snestest.ASAR)
    main()
