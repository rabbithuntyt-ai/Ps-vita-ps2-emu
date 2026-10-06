# EE memory access benchmark/correctness probe: loads and stores of every size
# through mapped pages (fast path) and kseg1 addresses (handler path).
# Usage: make_membench_elf.py out.elf ITERATIONS [fast]
# (fast: every access through the page table). Ends with s6 = 0xD0E, s7 = checksum.
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_test_elf as m
R = {n: i for i, n in enumerate('zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra'.split())}
code = []
labels = {}
fix = []
def I(op, rs, rt, imm): code.append((op << 26) | (R[rs] << 21) | (R[rt] << 16) | (imm & 0xFFFF))
def Rt(fn, rs, rt, rd, sa=0): code.append((R[rs] << 21) | (R[rt] << 16) | (R[rd] << 11) | (sa << 6) | fn)
def lui(rt, imm): I(0x0F, 'zero', rt, imm)
def ori(rt, rs, imm): I(0x0D, rs, rt, imm)
def addiu(rt, rs, imm): I(0x09, rs, rt, imm)
def li(rt, v): lui(rt, v >> 16); ori(rt, rt, v & 0xFFFF)
def addu(rd, rs, rt): Rt(0x21, rs, rt, rd)
def xor(rd, rs, rt): Rt(0x26, rs, rt, rd)
def or_(rd, rs, rt): Rt(0x25, rs, rt, rd)
def sll(rd, rt, sa): Rt(0x00, 'zero', rt, rd, sa)
def srl(rd, rt, sa): Rt(0x02, 'zero', rt, rd, sa)
def move(rd, rs): addu(rd, rs, 'zero')
def mem(op):
    return lambda rt, off, base: I(op, base, rt, off)
lb, lh, lw, lbu, lhu, sb, sh, sw = (mem(o) for o in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B))
ld, sd, lq, sq = (mem(o) for o in (0x37, 0x3F, 0x1E, 0x1F))
def dsrl32(rd, rt, sa): Rt(0x3E, 'zero', rt, rd, sa)
def lwc1(ft, off, base): code.append((0x31 << 26) | (R[base] << 21) | (ft << 16) | (off & 0xFFFF))
def swc1(ft, off, base): code.append((0x39 << 26) | (R[base] << 21) | (ft << 16) | (off & 0xFFFF))
def nop(): code.append(0)
def label(n): labels[n] = len(code)
def bne(rs, rt, n): fix.append((len(code), n)); I(0x05, rs, rt, 0)
def beq(rs, rt, n): fix.append((len(code), n)); I(0x04, rs, rt, 0)

iters = int(sys.argv[2])
lui('sp', 0x01F0)
lui('s0', 0x0030)
lui('s1', 0xA040 if len(sys.argv) < 4 else 0x0040)
lui('s2', 0x0040)  # same memory through the page table
li('s3', iters)
li('s7', 0x12345678)
label('outer')
move('a0', 's0'); addiu('t9', 'zero', 256); move('t0', 's7')
label('fill')
sll('t1', 't0', 13); xor('t0', 't0', 't1'); srl('t1', 't0', 17); xor('t0', 't0', 't1'); sll('t1', 't0', 5); xor('t0', 't0', 't1')
sw('t0', 0, 'a0'); addiu('a0', 'a0', 4); addiu('t9', 't9', -1); bne('t9', 'zero', 'fill'); nop()
move('a0', 's0'); move('a1', 's1'); move('a2', 's2'); addiu('t9', 'zero', 128)
label('proc')
lw('t0', 0, 'a0'); lb('t1', 1, 'a0'); lbu('t2', 2, 'a0'); lh('t3', 4, 'a0'); lhu('t4', 6, 'a0')
addu('t5', 't0', 't1'); addu('t5', 't5', 't2'); xor('t5', 't5', 't3'); addu('t5', 't5', 't4')
sw('t5', 0, 'a1'); sh('t3', 4, 'a1'); sb('t1', 6, 'a1')
lw('t6', 0, 'a2'); lhu('t7', 4, 'a2'); lbu('t8', 6, 'a2')
addu('s7', 's7', 't6'); xor('s7', 's7', 't7'); addu('s7', 's7', 't8')
lw('t6', 0, 'a1'); lb('t7', 6, 'a1'); lh('t8', 4, 'a1')
xor('s7', 's7', 't6'); addu('s7', 's7', 't7'); addu('s7', 's7', 't8')
sw('t5', 8, 'a2'); sh('t1', 12, 'a2'); sb('t3', 14, 'a2'); lw('t6', 12, 'a1'); xor('s7', 's7', 't6')
lwc1(2, 0, 'a0'); swc1(2, 8, 'a2'); lw('t6', 8, 'a1'); addu('s7', 's7', 't6')
swc1(2, 12, 'a1'); lw('t7', 12, 'a2'); xor('s7', 's7', 't7'); lwc1(3, 4, 'a1'); swc1(3, 0, 'a0')
ld('t6', 0, 'a0'); sd('t6', 0x4000, 'a1'); ld('t7', 0x4000, 'a2'); xor('s7', 's7', 't7'); dsrl32('t7', 't7', 0); addu('s7', 's7', 't7')
sd('t7', 0x4008, 'a2'); ld('t8', 0x4008, 'a1'); dsrl32('t6', 't8', 0); xor('s7', 's7', 't6'); xor('s7', 's7', 't8')
lq('t0', 0, 'a0'); sq('t0', 0x4010, 'a1'); lq('t1', 0x4010, 'a2'); sq('t1', 0x4020, 'a2')
lw('t2', 0x4028, 'a1'); lw('t3', 0x402C, 'a1'); xor('s7', 's7', 't2'); addu('s7', 's7', 't3')
lq('t4', 0x4020, 'a1'); sq('t4', 0x4030, 'a2'); lw('t2', 0x4038, 'a2'); addu('s7', 's7', 't2')
sd('zero', 0x4040, 'a2'); sq('zero', 0x4050, 'a1'); lw('t2', 0x4044, 'a1'); addu('s7', 's7', 't2'); lw('t2', 0x405C, 'a2'); addu('s7', 's7', 't2')
sll('t6', 's7', 1); srl('t7', 's7', 31); or_('s7', 't6', 't7')
addiu('a0', 'a0', 8); addiu('a1', 'a1', 16); addiu('a2', 'a2', 16)
addiu('t9', 't9', -1); bne('t9', 'zero', 'proc'); nop()
addiu('s3', 's3', -1); bne('s3', 'zero', 'outer'); nop()
li('s6', 0xD0E)
label('done')
beq('zero', 'zero', 'done'); nop()
for pos, n in fix:
    code[pos] |= (labels[n] - pos - 1) & 0xFFFF
BASE = 0x00100000
image = bytearray(0x10000)
for i, w in enumerate(code):
    struct.pack_into('<I', image, i * 4, w)
m.write_elf(sys.argv[1], bytes(image))
