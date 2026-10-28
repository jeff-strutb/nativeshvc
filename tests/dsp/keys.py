"""
keys.py: the notes a sound driver plays, nativeshvc's against the
reference's, with the driver's own work taking no time (contract 2.0).

A test cartridge uploads an SPC700 driver through the boot ROM and starts
it. The driver keeps a beat on timer 0 (100 a second) and on each beat does
what a driver does: a key-off (KOF, a voice or two, then 0), and two voices
set up and keyed on, each its sample (SRCN), its pitch, then KON, with the
driver's work between them (a pause of some 160 cycles after each, so on
the console each key event reaches the DSP before the next DSP write; one
beat in four keys off a voice and keys it on again). Its tables cycle
through 16 patterns, every voice in each role.

The reference runs the assembly (tests/oracle --spclog); nativeshvc runs
its C (tests/dsp/keys_spc.c, in tests/rom/testcart), whose pauses take no
time and whose beat is a wait (spc.h spc_wait_until), with nativeshvc's
rule that a DSP write waits until the DSP has taken the KON or KOFF before
it. Both runs' writes are played through the mixer (tests/dsp/key_events)
and each voice's key events read off it: every voice must have the same
events in the same order (each key-on with the same sample, from the BRR
block it plays), each on the same beat as the reference's (within the
driver's work in a beat: MAX_SHIFT samples, a beat being 320).

  python3 tests/dsp/keys.py        (about 30 seconds)

Needs NSHVC_ORACLE_CORE and NSHVC_ASAR, as tests/rom/run.py.
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tests", "rom"))
import snestest  # noqa: E402

OUT = os.path.join(ROOT, "build", "tests", "keys")
BIN = os.path.join(ROOT, "build", "null", "tests")
ORACLE = os.environ.get("NSHVC_ORACLE", os.path.join(BIN, "oracle"))
CART = os.environ.get("NSHVC_TESTCART", os.path.join(BIN, "testcart"))
EVENTS = os.environ.get("NSHVC_KEY_EVENTS", os.path.join(BIN, "key_events"))
CORE = os.environ.get("NSHVC_ORACLE_CORE")
FRAMES = 600
ORG = 0x0200
MAX_SHIFT = 32          # samples: 1 ms, the driver's work in a beat on the console

# the 16 patterns: voice A, B (A + 3), the samples they play, their pitch's
# low byte; KOF: voice A + 5, and A itself one beat in four
PAT = []
for x in range(16):
    a, b = x & 7, (x + 3) & 7
    kof = 1 << ((x + 5) & 7) | (1 << a if x % 4 == 0 else 0)
    PAT.append((a, (x >> 3) & 1, (x * 37) & 0xFF, b, ((x >> 3) ^ 1) & 1, (x * 91 + 7) & 0xFF, kof))

PROGRAM = r"""
arch spc700
norom
org 0
base $0200

macro dsp(reg, v)
    mov $f2,#<reg>
    mov $f3,#<v>
endmacro

; the driver's work between two key events: no access (a pause)
macro pause()
    mov y,#$28
?p:
    dbnz y,?p
endmacro

macro key_on(field)
    mov a,$0680+<field>+x
    mov y,a
    xcn a
    mov $11,a
    or a,#$04
    mov $f2,a
    mov a,$0690+<field>+x
    mov $f3,a
    mov a,$11
    or a,#$02
    mov $f2,a
    mov a,$06A0+<field>+x
    mov $f3,a
    mov a,$11
    or a,#$03
    mov $f2,a
    mov $f3,#$10
    mov $f2,#$4C
    mov a,$06F8+y
    mov $f3,a
    %pause()
endmacro

start:
    mov x,#$EF
    mov sp,x
    %dsp($6C, $20)
    %dsp($0C, $7F)
    %dsp($1C, $7F)
    %dsp($2C, $00)
    %dsp($3C, $00)
    %dsp($4D, $00)
    %dsp($3D, $00)
    %dsp($2D, $00)
    %dsp($5D, $06)
VOICES
    mov $FA,#$50
    mov $F1,#$01
    mov $10,#$00
beat:
    mov a,$FD
    beq beat
    inc $10
    mov a,$10
    and a,#$0F
    mov x,a
    mov $f2,#$5C
    mov a,$06E0+x
    mov $f3,a
    %pause()
    mov $f3,#$00
    %key_on($00)
    %key_on($30)
    jmp beat

; the directory (past the code): two samples, each looping on itself
org $400
base $0600
    dw $0700, $0700, $0710, $0710

; the patterns: A, its SRCN, its pitch; B, its SRCN, its pitch; KOF
org $480
base $0680
TABLES

; each voice's bit
org $4F8
base $06F8
    db $01, $02, $04, $08, $10, $20, $40, $80

; the samples: a block each, looping
org $500
base $0700
    db $B3, $77, $77, $77, $77, $99, $99, $99, $99
org $510
base $0710
    db $A3, $7F, $7F, $00, $00, $81, $81, $00, $00
"""


def program(outdir):
    voices = "".join("    %%dsp($%X0, $40)\n    %%dsp($%X1, $40)\n    %%dsp($%X5, $FF)\n    %%dsp($%X6, $F0)\n"
                     % (v, v, v, v) for v in range(8))
    tables = "".join("    db %s\n" % ", ".join("$%02X" % p[k] for p in PAT) for k in range(7))
    asm = os.path.join(outdir, "keys.asm")
    bin_path = os.path.join(outdir, "keys.bin")
    with open(asm, "w") as f:
        f.write(PROGRAM.replace("VOICES\n", voices).replace("TABLES\n", tables))
    subprocess.run([snestest.ASAR, "--no-title-check", asm, bin_path], check=True, stdout=subprocess.DEVNULL)
    with open(bin_path, "rb") as f:
        return f.read()


def cartridge(outdir):
    """the upload by the boot ROM's protocol, then the S-CPU waits (WAI)"""
    code = program(outdir)
    t = snestest.Test("dsp_keys", frames=FRAMES, shots=(), prologue=False)
    t.poll(0x2140, 0xAA)
    t.w(0x2142, ORG & 0xFF)
    t.w(0x2143, ORG >> 8)
    t.w(0x2141, 0x01)
    t.w(0x2140, 0xCC)
    t.poll(0x2140, 0xCC)
    for i, b in enumerate(code):
        t.w(0x2141, b)
        t.w(0x2140, i & 0xFF)
        t.poll(0x2140, i & 0xFF)
    t.w(0x2142, ORG & 0xFF)
    t.w(0x2143, ORG >> 8)
    t.w(0x2141, 0x00)
    t.w(0x2140, (len(code) + 2) & 0xFF or 2)
    return t


def events(log):
    """each voice's key events: (sample, kind, block)"""
    out = {v: [] for v in range(8)}
    r = subprocess.run([EVENTS, log], capture_output=True, text=True, check=True, timeout=600)
    for line in r.stdout.splitlines():
        w = line.split()
        out[int(w[1])].append((int(w[0]), w[2], w[3] if len(w) > 3 else ""))
    for v in out:
        out[v].sort()
    return out


def main():
    if not CORE or not snestest.ASAR:
        sys.exit("keys.py: set NSHVC_ORACLE_CORE and NSHVC_ASAR")
    os.makedirs(OUT, exist_ok=True)
    t = cartridge(OUT)
    rom = t.build(OUT)
    ref_log, nat_log = os.path.join(OUT, "ref.spclog"), os.path.join(OUT, "nat.spclog")
    subprocess.run([ORACLE, "--core", CORE, "--rom", rom, "--frames", str(FRAMES), "--out", OUT,
                    "--spclog", ref_log], check=True, timeout=600)
    script = os.path.join(OUT, "script.txt")
    with open(script, "w") as f:
        f.write("wait %d\n" % FRAMES)
    nd = os.path.join(OUT, "native")
    os.makedirs(nd, exist_ok=True)
    subprocess.run([CART, "--rom", rom, "--headless", "--script", script, "--out", nd], check=True, timeout=600,
                   env=dict(os.environ, NSHVC_SPCLOG=nat_log))
    ref, nat = events(ref_log), events(nat_log)
    bad, worst, total = 0, 0, 0
    for v in range(8):
        a, b = ref[v], nat[v]
        n = min(len(a), len(b))
        # the runs start their beats at different times (the upload's): each
        # event against the voice's first
        a0, b0 = (a[0][0], b[0][0]) if n else (0, 0)
        for k in range(n):
            shift = (b[k][0] - b0) - (a[k][0] - a0)
            worst = max(worst, abs(shift))
            if a[k][1:] != b[k][1:] or abs(shift) > MAX_SHIFT:
                if bad < 10:
                    print("voice %d event %d: reference %s at %d, nativeshvc %s at %d (shift %d)"
                          % (v, k, " ".join(a[k][1:]), a[k][0] - a0, " ".join(b[k][1:]), b[k][0] - b0, shift))
                bad += 1
        total += n
        # the last few may fall either side of the run's end
        if abs(len(a) - len(b)) > 4:
            print("voice %d: %d events on the reference, %d on nativeshvc" % (v, len(a), len(b)))
            bad += 1
    if total < 1000:
        print("too few events: %d" % total)
        bad += 1
    print("%d key events, %d differ; the largest shift %d samples (within %d)" % (total, bad, worst, MAX_SHIFT))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
