# IPU movie-decode throughput probe: decodes the same intra picture with IDEC
# over and over, the way MPEG players drive the IPU (BCLR, DMA3 out, DMA4 in,
# IDEC, poll IPU_CTRL busy, wait for DMA3). Ends each run with
# s6 = pictures decoded, s7 = checksum of the first decoded picture.
# Usage: make_ipu_elf.py out.elf [MACROBLOCKS]
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_test_elf as m

R = {n: i for i, n in enumerate('zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra'.split())}
code, labels, fix = [], {}, []
def I(op, rs, rt, imm): code.append((op << 26) | (R[rs] << 21) | (R[rt] << 16) | (imm & 0xFFFF))
def Rt(fn, rs, rt, rd, sa=0): code.append((R[rs] << 21) | (R[rt] << 16) | (R[rd] << 11) | (sa << 6) | fn)
def lui(rt, imm): I(0x0F, 'zero', rt, imm)
def ori(rt, rs, imm): I(0x0D, rs, rt, imm)
def andi(rt, rs, imm): I(0x0C, rs, rt, imm)
def addiu(rt, rs, imm): I(0x09, rs, rt, imm)
def li(rt, v): lui(rt, (v >> 16) & 0xFFFF); ori(rt, rt, v & 0xFFFF)
def addu(rd, rs, rt): Rt(0x21, rs, rt, rd)
def xor(rd, rs, rt): Rt(0x26, rs, rt, rd)
def or_(rd, rs, rt): Rt(0x25, rs, rt, rd)
def sll(rd, rt, sa): Rt(0x00, 'zero', rt, rd, sa)
def srl(rd, rt, sa): Rt(0x02, 'zero', rt, rd, sa)
def move(rd, rs): addu(rd, rs, 'zero')
def lw(rt, off, base): I(0x23, base, rt, off)
def sw(rt, off, base): I(0x2B, base, rt, off)
def nop(): code.append(0)
def label(n): labels[n] = len(code)
def bne(rs, rt, n): fix.append((len(code), n)); I(0x05, rs, rt, 0)
def beq(rs, rt, n): fix.append((len(code), n)); I(0x04, rs, rt, 0)
def bltz(rs, n): fix.append((len(code), n)); code.append((0x01 << 26) | (R[rs] << 21))


class Bits:
    def __init__(self): self.bits = []
    def put(self, value, count):
        for i in range(count - 1, -1, -1): self.bits.append((value >> i) & 1)
    def code(self, text): self.bits.extend(int(c) for c in text)
    def to_bytes(self):
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            chunk = self.bits[i:i + 8] + [0] * (8 - len(self.bits[i:i + 8]))
            out.append(int(''.join(map(str, chunk)), 2))
        return bytes(out)


# MPEG-2 tables B.12/B.13 (dc size) and B.14 (AC coefficients)
LUMA_DC = {0: '100', 1: '00', 2: '01', 3: '101'}
CHROMA_DC = {0: '00', 1: '01', 2: '10', 3: '110'}

def dc_diff(bits, table, size, diff):
    bits.code(table[size])
    if size:
        bits.put(diff if diff > 0 else diff + (1 << size) - 1, size)

def picture(mbs):
    bits = Bits()
    seed = 0x1234567
    def rnd():
        nonlocal seed
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        return seed >> 8
    for mb in range(mbs):
        if mb: bits.code('1')        # macroblock_address_increment = 1
        bits.code('1')               # macroblock_type: intra
        for block in range(6):
            size = rnd() % 4
            diff = 0
            if size:
                lo, hi = 1 << (size - 1), (1 << size) - 1
                diff = lo + rnd() % (hi - lo + 1)
                if rnd() & 1: diff = -diff
            dc_diff(bits, LUMA_DC if block < 4 else CHROMA_DC, size, diff)
            for _ in range(rnd() % 4):  # a few AC coefficients
                bits.code('11' if rnd() & 1 else '011')  # run 0 / run 1, level 1
                bits.put(rnd() & 1, 1)                   # sign
            bits.code('10')          # end of block
    while len(bits.bits) % 8: bits.bits.append(0)
    data = bits.to_bytes() + b'\x00\x00\x01\xB3'
    return data + b'\0' * (-len(data) % 16)


mbs = int(sys.argv[2]) if len(sys.argv) > 2 else 960
assert mbs * 64 <= 0xFFFF
STREAM, OUT = 0x00110000, 0x00400000
stream = picture(mbs)
IDEC = (1 << 28) | (1 << 16)  # IDEC, qsc 1, RGB32 output

lui('sp', 0x01F0)
li('s0', 0x10002000)  # IPU
li('s1', 0x1000B000)  # DMA3 (from IPU)
li('s2', 0x1000B400)  # DMA4 (to IPU)
li('s3', STREAM)
li('s4', OUT)
move('s6', 'zero'); move('s7', 'zero')
label('picture')
sw('zero', 0, 's0')                       # BCLR
label('bclr'); lw('t0', 0x10, 's0'); bltz('t0', 'bclr'); nop()
sw('s4', 0x10, 's1'); li('t0', mbs * 64); sw('t0', 0x20, 's1'); li('t0', 0x100); sw('t0', 0, 's1')
sw('s3', 0x10, 's2'); li('t0', len(stream) // 16); sw('t0', 0x20, 's2'); li('t0', 0x101); sw('t0', 0, 's2')
li('t0', IDEC); sw('t0', 0, 's0')
label('busy'); lw('t0', 0x10, 's0'); bltz('t0', 'busy'); nop()
label('dma3'); lw('t0', 0, 's1'); andi('t0', 't0', 0x100); bne('t0', 'zero', 'dma3'); nop()
bne('s6', 'zero', 'next'); nop()
move('a0', 's4'); li('t9', mbs * 256)
label('sum')
lw('t1', 0, 'a0'); sll('t2', 's7', 1); srl('t3', 's7', 31); or_('s7', 't2', 't3'); xor('s7', 's7', 't1')
addiu('a0', 'a0', 4); addiu('t9', 't9', -1); bne('t9', 'zero', 'sum'); nop()
label('next')
addiu('s6', 's6', 1)
beq('zero', 'zero', 'picture'); nop()
for pos, n in fix:
    code[pos] |= (labels[n] - pos - 1) & 0xFFFF

image = bytearray(STREAM - 0x00100000 + len(stream))
for i, w in enumerate(code):
    struct.pack_into('<I', image, i * 4, w)
image[STREAM - 0x00100000:] = stream
m.write_elf(sys.argv[1], bytes(image))
