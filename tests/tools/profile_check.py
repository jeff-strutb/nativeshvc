"""
profile_check.py: tools/nshvc-profile's fit held to known answers. Frames and a
chip's jobs are made from known weights; the fit must give those weights back,
a kind with too few accesses must come out 0, and the C table the tool writes
must build against nshvc.h and carry the weights.

  python3 tests/tools/profile_check.py
"""
import importlib.machinery, importlib.util, os, random, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
loader = importlib.machinery.SourceFileLoader("nshvc_profile", os.path.join(ROOT, "tools", "nshvc-profile"))
spec = importlib.util.spec_from_loader("nshvc_profile", loader)
P = importlib.util.module_from_spec(spec)
loader.exec_module(P)

fails = 0


def check(ok, what, got, want):
    global fails
    print("%s %s (%s, want %s)" % ("ok  " if ok else "FAIL", what, got, want))
    fails += not ok


rng = random.Random(7)
# the S-CPU: eight kinds' weights in clocks; write-rom never made (0 accesses)
true = [8.0, 28.0, 12.3, 34.7, 31.6, 0.0, 45.7, 33.3]
frames = []
for f in range(400):
    n = [rng.randrange(200, 3000) if k != 5 else 0 for k in range(8)]
    gap = sum(w * c for w, c in zip(true, n))
    bus = sum(8 * c for c in n)
    frames.append((f, n, gap, bus))
totals = [sum(fr[1][k] for fr in frames) for k in range(8)]
measured = [t >= P.MIN_ACCESSES for t in totals]
w, rows = P.fit_cpu(frames, measured)
check(all(abs(a - b) < 1e-6 for a, b in zip(w, true)), "the S-CPU's weights come back exactly",
      ["%.4f" % x for x in w], true)
check(not measured[5] and w[5] == 0.0, "a kind never made is not measured: 0", w[5], 0.0)
e = P.errors(rows, w)
check(e is not None and abs(e[1]) < 1e-9, "their error on those frames is 0", e and e[1], 0)

# the objective: noisy frames of one kind, whose least squares on the error
# in clocks has a closed form, w = sum(n y) / sum(n n); a fit on relative
# error gives another value
frames = []
for f in range(300):
    c = rng.randrange(100, 4000)
    n = [c, 0, 0, 0, 0, 0, 0, 0]
    gap = 20.0 * c + rng.uniform(-30000, 30000)
    frames.append((f, n, gap, 8 * c))
w1, _ = P.fit_cpu(frames, [True] + [False] * 7)
want = sum(fr[1][0] * fr[2] for fr in frames if fr[2] + fr[3] >= P.ACTIVE * P.FRAME) / \
    sum(fr[1][0] ** 2 for fr in frames if fr[2] + fr[3] >= P.ACTIVE * P.FRAME)
check(abs(w1[0] - want) < 1e-9, "the fit is least squares on each frame's error in clocks", w1[0], want)

# a chip: two programs fitted, one with too few jobs (a multiplier), the default
ops = ["LOAD", "STORE", "FLUSH", "RPIX"]
progs = {0x01A000: (120.0, [5.25, 7.5, 3.0, 9.0]), 0x02B000: (40.0, [6.0, 0.0, 4.5, 5.0])}
with tempfile.TemporaryDirectory() as d:
    jobs = os.path.join(d, "jobs.txt")
    with open(jobs, "w") as f:
        f.write("nshvc-chip-jobs 1 superfx ops %s speed 5\n" % " ".join(ops))
        for entry, (fixed, wts) in progs.items():
            for _ in range(60):
                c = [rng.randrange(0, 500) for _ in ops]
                f.write("job %06X %.3f %s\n" % (entry, fixed + sum(a * b for a, b in zip(wts, c)), " ".join(map(str, c))))
        for _ in range(5):
            c = [rng.randrange(1, 50) for _ in ops]
            f.write("job 03C000 %.3f %s\n" % (2.0 * 5 * sum(c), " ".join(map(str, c))))
    name, gops, speed, default, crow = P.fit_chip(jobs)
    got = {r[0]: r for r in crow}
    for entry, (fixed, wts) in progs.items():
        r = got[entry]
        ok = r[3] == "fit" and abs(r[1] - fixed) < 1e-3 and all(abs(a - b) < 1e-4 for a, b in zip(r[2], wts))
        check(ok, "program %06X's fixed and weights come back" % entry, (round(r[1], 3), [round(x, 4) for x in r[2]]),
              (fixed, wts))
    r = got[0x03C000]
    check(r[3] == "multiplier" and all(abs(x - 10.0) < 1e-9 for x in r[2]),
          "a program with too few jobs: its multiplier on the speed", [round(x, 4) for x in r[2]], [10.0] * 4)

    # the files: the profile names the ops; the C builds and carries the weights
    prof, csrc = os.path.join(d, "p.txt"), os.path.join(d, "p.c")
    chip = (name, gops, speed, default, crow)
    P.write_profile(prof, [jobs], w, chip, False, None, None)
    text = open(prof).read()
    check("chip superfx ops LOAD STORE FLUSH RPIX" in text and "checked unchecked" in text,
          "the profile names the chip's ops and says step 2 did not run", "", "")
    P.write_c(csrc, "t", w, chip)
    main = os.path.join(d, "m.c")
    with open(main, "w") as f:
        f.write('#include <stdio.h>\n#include "nshvc/nshvc.h"\nextern const nshvc_cost_profile t_cost_profile;\n'
                'int main(void) { const nshvc_cost_profile *p = &t_cost_profile; int k;\n'
                ' for (k = 0; k < 4; k++) printf("%u %u ", p->cpu_tenths[0][k], p->cpu_tenths[1][k]);\n'
                ' printf("%d %d %X %u %u\\n", p->chip_ops, p->program_count, p->programs[0].entry,\n'
                '        p->programs[0].fixed, p->programs[0].weight[0]); return 0; }\n')
    exe = os.path.join(d, "m")
    r = subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", os.path.join(ROOT, "include"), csrc, main, "-o", exe],
                       capture_output=True, text=True)
    check(r.returncode == 0, "the C table builds against nshvc.h", r.stderr.strip()[:200], "")
    if r.returncode == 0:
        out = subprocess.run([exe], capture_output=True, text=True).stdout.split()
        want = []
        for k in range(4):
            want += [str(round(true[k] * 10)), str(round(true[4 + k] * 10))]
        want += ["4", "3", "1A000", "120", str(round(5.25 * 4096))]
        check(out == want, "the C table carries the weights", out, want)

print("%d check(s) failed" % fails)
sys.exit(1 if fails else 0)
