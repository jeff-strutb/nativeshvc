"""
snestest.py: test cartridges for nativeshvc, run on both sides.

A test is a list of steps (writes to the console's registers, DMA from the
ROM, waits for a line or for frames, reads logged to work RAM) and the data
its DMA moves. build() makes a ROM of it: a small interpreter in 65816
assembly (asar) that runs the steps from a table in the ROM. The same ROM
runs on the oracle (bsnes) and on nativeshvc, where the cartridge's C
(tests/rom/testcart.c) is the same interpreter written against the machine
model, reading the same table. So both consoles see the same accesses in
the same order; what each one shows, plays and answers is compared.

Steps in the table (bank $00, from $9000; the IRQ's from $E000, the NMI's
from $F000, writes only):
  01 aaaa vv        write vv to $00:aaaa
  04 llll           wait until the beam is on line llll (polled: SLHV, OPVCT)
  05 nn             wait for the start of nn vertical blanks
  06 aaaaaa         read $aaaaaa, the byte logged to $7E:2000 + n
  07 aaaaaa vv      write vv to $aaaaaa
  08                CLI: the IRQ unmasked
  09                WAI: until an interrupt
  0A aaaa vv        wait until $00:aaaa reads vv (polled: LDA ($00); CMP; BNE)
  0B nn mm          a delay: nn turns of a 60-clock loop, then mm of a 74-clock
                    one (a start's phase set to 2 clocks)
  0C nn mm          turns of four NOPs, DEC, BNE: nn, then 256 more for each mm
                    past 1 (testcart.c states the loop in stretches of
                    cycles: machine.h cycs)
  0D vv             write vv to MDMAEN ($420B), then four NOPs (the same)
  0E vv             write vv to WRMPYB ($4203), then read RDDIV and RDMPY
                    ($4214-$4217) at once, each stored to $10-$13
  0F vv             the same with WRDIVB ($4206)
  10 vv             0E with a NOP before the reads
  11 vv             vv to WRMPYB and at once to WRDIVB (the second while the
                    first is under way), then the reads
  00                stop (the CPU loops for ever)

The cartridge's mapping (LoROM, HiROM, ExHiROM) puts the interpreter's
32 KB at $00:8000 and the data where each mapping reaches it; the rest of
the ROM is a pattern of its own offsets, for tests of the mapping.
"""
import os
import random
import subprocess

ASAR = os.environ.get("NSHVC_ASAR", "")

INTERP = r"""
lorom
org $008000
reset:
    sei
    clc
    xce
    rep #$10
    sep #$20
    ldx #$01FF
    txs
    lda #$00
    pha
    plb
    ldx #$9000
    ldy #$0000
next:
    lda $0000,x
    inx
    cmp #$01
    bne +
    jmp op_write
+
    cmp #$04
    bne +
    jmp op_line
+
    cmp #$05
    bne +
    jmp op_frames
+
    cmp #$06
    bne +
    jmp op_read
+
    cmp #$07
    bne +
    jmp op_wlong
+
    cmp #$08
    bne +
    jmp op_cli
+
    cmp #$09
    bne +
    jmp op_wai
+
    cmp #$0A
    bne +
    jmp op_poll
+
    cmp #$0B
    bne +
    jmp op_delay
+
    cmp #$0C
    bne +
    jmp op_nops
+
    cmp #$0D
    bne +
    jmp op_dmago
+
    cmp #$0E
    bne +
    jmp op_mul
+
    cmp #$0F
    bne +
    jmp op_div
+
    cmp #$10
    bne +
    jmp op_mul_nop
+
    cmp #$11
    bne +
    jmp op_mul_div
+
halt:
    wai
    bra halt

op_write:
    rep #$20
    lda $0000,x
    sta $00
    sep #$20
    lda $0002,x
    sta ($00)
    inx
    inx
    inx
    jmp next

op_line:
    rep #$20
    lda $0000,x
    sta $02
    sep #$20
    inx
    inx
.poll:
    lda $213F
    lda $2137
    lda $213D
    sta $04
    lda $213D
    and #$01
    sta $05
    rep #$20
    lda $04
    cmp $02
    sep #$20
    bne .poll
    jmp next

op_frames:
    lda $0000,x
    sta $06
    inx
.out:
    lda $4212
    bmi .out
.in:
    lda $4212
    bpl .in
    dec $06
    bne .out
    jmp next

op_read:
    rep #$20
    lda $0000,x
    sta $08
    sep #$20
    lda $0002,x
    sta $0A
    inx
    inx
    inx
    lda [$08]
    phx
    tyx
    sta $7E2000,x
    plx
    iny
    jmp next

op_cli:
    cli
    jmp next

op_poll:
    rep #$20
    lda $0000,x
    sta $00
    sep #$20
    lda $0002,x
    sta $02
    inx
    inx
    inx
.p:
    lda ($00)
    cmp $02
    bne .p
    jmp next

op_delay:
    lda $0000,x
    sta $06
    lda $0001,x
    sta $07
    inx
    inx
    lda $06
    beq .b
.a:
    dec $06
    bne .a
.b:
    lda $07
    beq .d
.c:
    nop
    dec $07
    bne .c
.d:
    jmp next

op_nops:
    lda $0000,x
    sta $06
    lda $0001,x
    sta $07
    inx
    inx
.l:
    nop
    nop
    nop
    nop
    dec $06
    bne .l
    dec $07
    bne .l
    jmp next

op_mul:
    lda $0000,x
    sta $4203
    jmp alu_reads

op_div:
    lda $0000,x
    sta $4206
    jmp alu_reads

op_mul_nop:
    lda $0000,x
    sta $4203
    nop
    jmp alu_reads

op_mul_div:
    lda $0000,x
    sta $4203
    sta $4206
    jmp alu_reads

alu_reads:
    lda $4214
    sta $10
    lda $4215
    sta $11
    lda $4216
    sta $12
    lda $4217
    sta $13
    inx
    jmp next

op_dmago:
    lda $0000,x
    sta $420B
    nop
    nop
    nop
    nop
    inx
    jmp next

op_wai:
    wai
    jmp next

op_wlong:
    rep #$20
    lda $0000,x
    sta $08
    sep #$20
    lda $0002,x
    sta $0A
    lda $0003,x
    sta [$08]
    inx
    inx
    inx
    inx
    jmp next

; the IRQ: its steps (writes, 01 aaaa vv) from where the last IRQ left off,
; up to a 00; the NMI: its steps from the start every time, and the IRQ's
; back to their start
irq:
    rep #$30
    pha
    phx
    phy
    sep #$20
    lda $4211
    ldx $10
.step:
    lda $0000,x
    beq .done
    rep #$20
    lda $0001,x
    sta $0C
    sep #$20
    lda $0003,x
    sta ($0C)
    inx
    inx
    inx
    inx
    bra .step
.done:
    inx
    stx $10
    rep #$30
    ply
    plx
    pla
    rti

nmi:
    rep #$30
    pha
    phx
    phy
    sep #$20
    lda $4210
    ldx #$E000
    stx $10
    ldx #$F000
.step:
    lda $0000,x
    beq .done
    rep #$20
    lda $0001,x
    sta $0C
    sep #$20
    lda $0003,x
    sta ($0C)
    inx
    inx
    inx
    inx
    bra .step
.done:
    rep #$30
    ply
    plx
    pla
    rti

stub:
    rti

org $00FFEA
    dw nmi
org $00FFEE
    dw irq
org $00FFFC
    dw reset, stub
"""

TABLE = 0x9000               # the steps, bank $00
MAP_MODE = {"lorom": 0x20, "hirom": 0x21, "exhirom": 0x25, "exlorom": 0x30}


def pattern(o):
    """the byte the ROM holds at offset o, where no test data is"""
    return (o ^ o >> 8 ^ o >> 15 ^ o >> 21 ^ 0x5A) & 0xFF


class Test:
    def __init__(self, name, frames=6, shots=(6,), seed=None, mapping="lorom", rom_size=0x40000, sram=0, pal=False,
                 prologue=True):
        self.name = name
        self.frames = frames
        self.shots = list(shots)
        self.steps = bytearray()
        self.mapping = mapping
        self.rom_size = rom_size
        self.sram = sram
        self.pal = pal
        self.blobs = []           # (file offset, bytes)
        self.next_file = {"lorom": 0x8000, "exlorom": 0x8000}.get(mapping, self.code_offset() + 0x10000)
        self.rng = random.Random(seed if seed is not None else name)
        self.reads = 0
        self.irq_steps = bytearray()
        self.nmi_steps = bytearray()
        if prologue:
            self.prologue()

    # ---- steps ----
    def w(self, addr, v):
        self.steps += bytes([0x01, addr & 0xFF, addr >> 8 & 0xFF, v & 0xFF])

    def w16(self, addr, v):
        """a register written twice (low byte, then high)"""
        self.w(addr, v & 0xFF)
        self.w(addr, v >> 8 & 0xFF)

    def w2(self, addr, v):
        """a 16-bit value to a pair of registers (addr low, addr + 1 high)"""
        self.w(addr, v & 0xFF)
        self.w(addr + 1, v >> 8 & 0xFF)

    def wait_line(self, line):
        self.steps += bytes([0x04, line & 0xFF, line >> 8 & 0xFF])

    def wait_frames(self, n):
        assert 1 <= n <= 255
        self.steps += bytes([0x05, n])

    def irq_w(self, addr, v):
        """a write the next IRQ makes (irq_end() ends that IRQ's)"""
        self.irq_steps += bytes([0x01, addr & 0xFF, addr >> 8 & 0xFF, v & 0xFF])

    def irq_end(self):
        self.irq_steps += b"\0"

    def nmi_w(self, addr, v):
        """a write every NMI makes"""
        self.nmi_steps += bytes([0x01, addr & 0xFF, addr >> 8 & 0xFF, v & 0xFF])

    def cli(self):
        self.steps += b"\x08"

    def wai(self):
        self.steps += b"\x09"

    def wlong(self, addr, v):
        self.steps += bytes([0x07, addr & 0xFF, addr >> 8 & 0xFF, addr >> 16 & 0xFF, v & 0xFF])

    def poll(self, addr, v):
        self.steps += bytes([0x0A, addr & 0xFF, addr >> 8 & 0xFF, v & 0xFF])

    def delay(self, n, m):
        self.steps += bytes([0x0B, n & 0xFF, m & 0xFF])

    def nops(self, n, m=1):
        self.steps += bytes([0x0C, n & 0xFF, m & 0xFF])

    def dmago(self, channels):
        self.steps += bytes([0x0D, channels & 0xFF])

    def alu(self, op, v):
        self.steps += bytes([op, v & 0xFF])

    def read(self, addr):
        self.steps += bytes([0x06, addr & 0xFF, addr >> 8 & 0xFF, addr >> 16 & 0xFF])
        self.reads += 1

    # ---- data ----
    def code_offset(self):
        """the file offset of $00:8000"""
        return {"lorom": 0, "hirom": 0x8000, "exhirom": 0x408000, "exlorom": 0x400000}[self.mapping]

    def cpu_address(self, o):
        """an S-CPU address that reads file offset o"""
        if self.mapping == "lorom":
            return (o >> 15) << 16 | 0x8000 | (o & 0x7FFF)
        if self.mapping == "exlorom":
            if o < 0x400000:
                return (0x80 | o >> 15) << 16 | 0x8000 | (o & 0x7FFF)
            return ((o - 0x400000) >> 15) << 16 | 0x8000 | (o & 0x7FFF)
        if self.mapping == "hirom":
            return (0xC0 | o >> 16) << 16 | (o & 0xFFFF)
        return ((0xC0 | o >> 16) if o < 0x400000 else (0x40 | (o - 0x400000) >> 16)) << 16 | (o & 0xFFFF)

    def blob(self, data):
        """data put in the ROM: its 24-bit S-CPU address"""
        data = bytes(data)
        assert len(data) <= 0x8000
        if (self.next_file & 0x7FFF) + len(data) > 0x8000:
            self.next_file = (self.next_file | 0x7FFF) + 1
        o = self.next_file
        assert o + len(data) <= self.rom_size, "test data past the ROM"
        self.blobs.append((o, data))
        self.next_file += len(data)
        return self.cpu_address(o)

    def dma(self, ch, dmap, bbad, src, count):
        base = 0x4300 + ch * 0x10
        self.w(base + 0, dmap)
        self.w(base + 1, bbad)
        self.w(base + 2, src & 0xFF)
        self.w(base + 3, src >> 8 & 0xFF)
        self.w(base + 4, src >> 16 & 0xFF)
        self.w(base + 5, count & 0xFF)
        self.w(base + 6, count >> 8 & 0xFF)
        self.w(0x420B, 1 << ch)

    def vram(self, word_addr, data, vmain=0x80):
        data = bytes(data)
        self.w(0x2115, vmain)
        self.w2(0x2116, word_addr)
        for i in range(0, len(data), 0x8000):
            part = data[i:i + 0x8000]
            self.dma(0, 0x01, 0x18, self.blob(part), len(part) & 0xFFFF)

    def cgram(self, index, data):
        data = bytes(data)
        self.w(0x2121, index)
        self.dma(0, 0x00, 0x22, self.blob(data), len(data))

    def oam(self, data, addr=0):
        data = bytes(data)
        self.w2(0x2102, addr)
        self.dma(0, 0x00, 0x04, self.blob(data), len(data))

    def hdma(self, ch, dmap, bbad, table, indirect_bank=None):
        """an HDMA channel's table (bytes, in the ROM); enabled by HDMAEN"""
        base = 0x4300 + ch * 0x10
        src = self.blob(table)
        self.w(base + 0, dmap)
        self.w(base + 1, bbad)
        self.w(base + 2, src & 0xFF)
        self.w(base + 3, src >> 8 & 0xFF)
        self.w(base + 4, src >> 16 & 0xFF)
        if indirect_bank is not None:
            self.w(base + 7, indirect_bank)

    # ---- the start every test shares: forced blank, the PPU's registers
    # cleared, VRAM, CGRAM and OAM cleared, no interrupts ----
    def prologue(self):
        self.w(0x2100, 0x80)
        self.w(0x4200, 0x00)
        self.w(0x0010, 0x00)          # the IRQ's steps from their start
        self.w(0x0011, 0xE0)
        self.w(0x420C, 0x00)
        for r in range(0x2101, 0x2134):
            if r in (0x2104, 0x2118, 0x2119, 0x2122):
                continue
            n = 2 if (0x210D <= r <= 0x2114 or 0x211B <= r <= 0x2120) else 1
            for _ in range(n):
                self.w(r, 0x00)
        zero = self.blob(b"\0\0")
        self.w(0x2115, 0x80)
        self.w2(0x2116, 0)
        self.dma(0, 0x09, 0x18, zero, 0)          # 64 KB of VRAM, a fixed source
        self.w(0x2121, 0)
        self.dma(0, 0x08, 0x22, zero, 512)        # CGRAM
        hidden = bytearray()
        for i in range(128):
            hidden += bytes([0x00, 0xE0, 0x00, 0x00])
        hidden += bytes(32)
        self.oam(hidden)

    def show(self, inidisp=0x0F):
        self.w(0x2100, inidisp)

    # ---- random data ----
    def rand_bytes(self, n):
        return bytes(self.rng.getrandbits(8) for _ in range(n))

    def rand_colors(self, n):
        out = bytearray()
        for _ in range(n):
            c = self.rng.getrandbits(15)
            out += bytes([c & 0xFF, c >> 8])
        return out

    # ---- the ROM ----
    def build(self, outdir):
        os.makedirs(outdir, exist_ok=True)
        asm = os.path.join(outdir, self.name + ".asm")
        rom_path = os.path.join(outdir, self.name + ".sfc")
        with open(asm, "w") as f:
            f.write(INTERP)
        rom = bytearray(pattern(o) for o in range(self.rom_size))
        subprocess.run([ASAR, "--no-title-check", "--fix-checksum=off", asm, rom_path], check=True,
                       stdout=subprocess.DEVNULL)
        with open(rom_path, "rb") as f:
            code = f.read()
        c = self.code_offset()
        bank0 = bytearray(0x8000)
        bank0[:len(code)] = code[:0x8000]
        steps = self.steps + b"\0"
        assert TABLE + len(steps) <= 0xE000, "the steps run into the IRQ's"
        bank0[TABLE - 0x8000:TABLE - 0x8000 + len(steps)] = steps
        irq = self.irq_steps + b"\0"
        nmi = self.nmi_steps + b"\0"
        assert len(irq) <= 0x1000 and len(nmi) <= 0xF00
        bank0[0xE000 - 0x8000:0xE000 - 0x8000 + len(irq)] = irq
        bank0[0xF000 - 0x8000:0xF000 - 0x8000 + len(nmi)] = nmi
        rom[c:c + 0x8000] = bank0
        for o, data in self.blobs:
            rom[o:o + len(data)] = data
        header(rom, self.name, c + 0x7FC0, MAP_MODE[self.mapping], self.sram, self.pal)
        with open(rom_path, "wb") as f:
            f.write(rom)
        return rom_path


def header(rom, title, h, map_mode, sram, pal=False):
    """the header at $00:FFC0 (file offset h), with its checksum"""
    t = title.upper().encode("ascii", "replace")[:21].ljust(21, b" ")
    rom[h:h + 21] = t
    rom[h + 0x15] = map_mode      # SlowROM
    rom[h + 0x16] = 0x02 if sram else 0x00      # ROM, RAM, battery
    size = 0
    while (1024 << size) < len(rom):
        size += 1
    rom[h + 0x17] = size
    ram = 0
    while sram and (1024 << ram) < sram:
        ram += 1
    rom[h + 0x18] = ram if sram else 0
    rom[h + 0x19] = 0x02 if pal else 0x01      # Europe (PAL) or North America
    rom[h + 0x1A] = 0x00
    rom[h + 0x1B] = 0x00
    rom[h + 0x1C:h + 0x20] = b"\xFF\xFF\x00\x00"
    s = checksum(rom)
    rom[h + 0x1C] = (s ^ 0xFFFF) & 0xFF
    rom[h + 0x1D] = (s ^ 0xFFFF) >> 8
    rom[h + 0x1E] = s & 0xFF
    rom[h + 0x1F] = s >> 8


def checksum(rom):
    """the header's sum: a size that is not a power of two counts its last
       part repeated up to the next power of two"""
    n = len(rom)
    p = 1
    while p * 2 <= n:
        p *= 2
    if p == n:
        return sum(rom) & 0xFFFF
    rest = rom[p:]
    k = p // len(rest) if len(rest) else 0
    return (sum(rom[:p]) + sum(rest) * k) & 0xFFFF
