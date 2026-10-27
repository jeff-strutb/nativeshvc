"""
first.py: where a Stunt Race FX play on nativeshvc first differs from the
original cartridge on the reference: the first frame whose work RAM (the
stack left out) or Super FX RAM differs, the bytes that differ at its end,
and for each work RAM byte the cartridge's instruction that last wrote it in
that frame (its routine, by the decomp's routine table). That instruction
points to the game's C (its routine does not do what the cartridge's does)
or to the console (it stores what it read from hardware).

  python3 tests/srfx/first.py [PLAY...]

As tests/srfx/same.py: SRFX, NSHVC_ORACLE_CORE.
"""
import concurrent.futures, glob, os, re, shutil, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import same  # noqa: E402

ROUTINES = os.path.join(os.path.dirname(same.EXE), "gen", "routines.c")
STACK = range(0x100, 0x300)


def expand(path):
    """a play's frames' buttons, as the script runners take them: the
       first is taken before power-on"""
    lines = [l.split("#")[0].split() for l in open(path)]
    out = []

    def run(lo, hi):
        i = lo
        while i < hi:
            w = lines[i]
            if not w:
                i += 1
                continue
            c = w[0]
            if c == "wait":
                out.extend([None] * int(w[1]))
            elif c in ("hold", "press"):
                n = int(w[2]) if len(w) > 2 else (2 if c == "press" else 1)
                out.extend([w[1]] * n)
                if c == "press":
                    out.extend([None] * 2)
            elif c == "repeat":
                depth, j = 0, i + 1
                while j < hi:
                    c2 = lines[j][:1]
                    if c2 == ["repeat"]:
                        depth += 1
                    elif c2 == ["end"]:
                        if depth == 0:
                            break
                        depth -= 1
                    j += 1
                for _ in range(int(w[1])):
                    run(i + 1, j)
                i = j
            elif c not in ("shot", "dump", "save", "load", "end"):
                raise SystemExit("first.py: a step it does not take: %s" % c)
            i += 1

    run(0, len(lines))
    return out


def upto(frames, n, path):
    """a script of a play's first frames that dumps the RAMs at frame n's end
       (the first frame's buttons are taken before power-on: n in all)"""
    with open(path, "w") as f:
        for b in frames[:n]:
            f.write("hold %s 1\n" % b if b else "wait 1\n")
        f.write("dump d\n")


def hashes(cmd, out):
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    h = os.path.join(out, "hashes.txt")
    p = subprocess.run(cmd + ["--out", out, "--hashes", h], capture_output=True, text=True, timeout=3600)
    if p.returncode:
        raise RuntimeError("%s exit %d: %s" % (os.path.basename(cmd[0]), p.returncode, p.stderr[-300:]))
    return [l.split() for l in open(h)]


def routine_names():
    t = []
    for m in re.finditer(r"\{0x([0-9A-Fa-f]{6}), abi_\w+, \"(\w+)\"", open(ROUTINES).read()):
        t.append((int(m.group(1), 16), m.group(2)))
    return sorted(t)


def name_of(table, pc):
    best = None
    for a, n in table:
        if a <= pc and (pc >> 16) == (a >> 16):
            best = (a, n)
    return "%s+%X" % (best[1], pc - best[0]) if best else "?"


def one(path, table):
    name = os.path.splitext(os.path.basename(path))[0]
    base = os.path.join(same.WORK, name, "first")
    s = same.script_for(path)
    a = hashes([same.ORACLE, "--core", same.CORE, "--rom", same.ROM, "--script", s], os.path.join(base, "bsnes"))
    b = hashes([same.EXE, "--rom", same.ROM, "--script", s, "--headless"], os.path.join(base, "nshvc"))
    fw = next((int(x[0]) for x, y in zip(a, b) if x[1] != y[1]), None)
    fg = next((int(x[0]) for x, y in zip(a, b) if x[2] != y[2]), None)
    if fw is None and fg is None:
        return name, ["same: every frame's RAM (%d frames)" % min(len(a), len(b))]
    # work RAM's first difference leads: the Super FX's RAM can differ for a
    # frame while a program is under way and still end the same
    f = fw if fw is not None else fg
    frames = expand(path)
    sc = os.path.join(base, "upto.sfx")
    upto(frames, f, sc)
    da, db = os.path.join(base, "dump_bsnes"), os.path.join(base, "dump_nshvc")
    for d in (da, db):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    subprocess.run([same.ORACLE, "--core", same.CORE, "--rom", same.ROM, "--script", sc, "--out", da],
                   check=True, capture_output=True, timeout=3600)
    subprocess.run([same.EXE, "--rom", same.ROM, "--script", sc, "--headless", "--out", db],
                   check=True, capture_output=True, timeout=3600)
    lines = ["first differs at frame %d (work RAM from %s, Super FX RAM from %s)" % (f, fw, fg)]
    wa, wb = open(os.path.join(da, "d.wram"), "rb").read(), open(os.path.join(db, "d.wram"), "rb").read()
    ga, gb = open(os.path.join(da, "d.sram"), "rb").read(), open(os.path.join(db, "d.sram"), "rb").read()
    dw = [i for i in range(len(wa)) if wa[i] != wb[i] and i not in STACK]
    dg = [i for i in range(min(len(ga), len(gb))) if ga[i] != gb[i]]
    lines.append("work RAM: %d bytes differ: %s" % (len(dw), " ".join("$%05X %02X/%02X" % (i, wa[i], wb[i])
                                                                     for i in dw[:10])))
    lines.append("Super FX RAM: %d bytes differ: %s" % (len(dg), " ".join("$%04X %02X/%02X" % (i, ga[i], gb[i])
                                                                         for i in dg[:10])))
    if dw:
        p = subprocess.run([same.ORACLE, "--core", same.CORE, "--rom", same.ROM, "--script", sc,
                            "--out", os.path.join(base, "watch"), "--watch",
                            "%d-%d:%s" % (f, f, ",".join("%X" % i for i in dw[:64]))],
                           capture_output=True, text=True, timeout=3600)
        last = {}
        for l in p.stderr.splitlines():
            m = re.match(r"frame (\d+) pc ([0-9A-F]+) (\w+) \$([0-9A-F]+) <- ([0-9A-F]+)", l)
            if m:
                last[int(m.group(4), 16)] = (int(m.group(2), 16), m.group(3))
        writers = {}
        for i in dw[:64]:
            k = "%s %06X %s" % ((name_of(table, last[i][0]),) + last[i]) if i in last else "not written this frame"
            writers.setdefault(k, []).append(i)
        for k, v in writers.items():
            lines.append("  %s: %s" % (k, " ".join("$%05X" % i for i in v[:8])))
    return name, lines


def main(argv):
    if not same.CORE:
        sys.exit("first.py: set NSHVC_ORACLE_CORE")
    table = routine_names()
    plays = argv[1:] or sorted(glob.glob(os.path.join(same.SRFX, "tools", "stuntracefx", "plays", "*.sfx")))
    plays = [p for p in plays if "poke" not in open(p).read()]
    with concurrent.futures.ThreadPoolExecutor(max(1, (os.cpu_count() or 2) // 2)) as ex:
        for name, lines in ex.map(lambda p: one(p, table), plays):
            print(name)
            for l in lines:
                print("  " + l)
            sys.stdout.flush()


if __name__ == "__main__":
    sys.exit(main(sys.argv))
