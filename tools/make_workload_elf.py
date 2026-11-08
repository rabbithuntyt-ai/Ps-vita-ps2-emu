# Endless EE workloads shaped like common game code, for measuring how many
# host (ARM) instructions the emulator needs per PS2 instruction
# (tools/perf_gap.py). Each kernel loops forever and keeps a running result
# in s7 so nothing is dead code.
# Usage: make_workload_elf.py out.elf KERNEL [--check]   (KERNEL: int, float, mmi, call)
#   --check: run one pass, then s6 = 0xD0E and s7 = checksum (for tests);
#            float takes its matrix from random bits (NaN/Inf/huge values
#            exercise the clamping)
#   int   - table-driven CRC32 over a 4KB buffer (decompression/checksum style:
#           byte loads, table lookups, shifts, a branch per byte)
#   float - 4x4 matrix times 64 vectors with the FPU (transform code)
#   mmi   - averaging two 16-pixel rows with 128-bit MMI (MPEG motion
#           compensation style: lq, pextlb/pextub, paddh, psrlh, ppacb, sq)
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
def and_(rd, rs, rt): Rt(0x24, rs, rt, rd)
def sll(rd, rt, sa): Rt(0x00, 'zero', rt, rd, sa)
def srl(rd, rt, sa): Rt(0x02, 'zero', rt, rd, sa)
def subu(rd, rs, rt): Rt(0x23, rs, rt, rd)
def move(rd, rs): addu(rd, rs, 'zero')
def mem(op): return lambda rt, off, base: I(op, base, rt, off)
lw, lbu, sw, sb, lq, sq = (mem(o) for o in (0x23, 0x24, 0x2B, 0x28, 0x1E, 0x1F))
def lwc1(ft, off, base): code.append((0x31 << 26) | (R[base] << 21) | (ft << 16) | (off & 0xFFFF))
def swc1(ft, off, base): code.append((0x39 << 26) | (R[base] << 21) | (ft << 16) | (off & 0xFFFF))
def cop1s(fn, fd, fs, ft): code.append((0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn)
def mul_s(fd, fs, ft): cop1s(0x02, fd, fs, ft)
def add_s(fd, fs, ft): cop1s(0x00, fd, fs, ft)
def mfc1(rt, fs): code.append((0x11 << 26) | (0x00 << 21) | (R[rt] << 16) | (fs << 11))
def mmi(fn, sub, rs, rt, rd): code.append((0x1C << 26) | (R[rs] << 21) | (R[rt] << 16) | (R[rd] << 11) | (sub << 6) | fn)
def paddh(rd, rs, rt): mmi(0x08, 0x04, rs, rt, rd)
def pextlb(rd, rs, rt): mmi(0x08, 0x1A, rs, rt, rd)
def ppacb(rd, rs, rt): mmi(0x08, 0x1B, rs, rt, rd)
def pextub(rd, rs, rt): mmi(0x28, 0x1A, rs, rt, rd)
def psrlh(rd, rt, sa): mmi(0x36, sa, 'zero', rt, rd)
def nop(): code.append(0)
def label(n): labels[n] = len(code)
def bne(rs, rt, n): fix.append((len(code), n)); I(0x05, rs, rt, 0)
def beq(rs, rt, n): fix.append((len(code), n)); I(0x04, rs, rt, 0)
jfix, lafix = [], []
def jal(n): jfix.append((len(code), n)); code.append(0x03 << 26)
def jr(rs): Rt(0x08, rs, 'zero', 'zero')
def jalr(rs): Rt(0x09, rs, 'zero', 'ra')
def la(rt, n): lafix.append((len(code), n)); lui(rt, 0); ori(rt, rt, 0)

kernel = sys.argv[2]
check = '--check' in sys.argv
DATA, TABLE, OUT = 0x00200000, 0x00300000, 0x00380000
lui('sp', 0x01F0)
li('s0', DATA); li('s1', TABLE); li('s2', OUT)
move('s7', 'zero')

# Pseudo-random 4KB input (xorshift), shared by all kernels
move('a0', 's0'); li('t9', 1024); li('t0', 0x2545F491)
label('fill')
sll('t1', 't0', 13); xor('t0', 't0', 't1'); srl('t1', 't0', 17); xor('t0', 't0', 't1'); sll('t1', 't0', 5); xor('t0', 't0', 't1')
sw('t0', 0, 'a0'); addiu('a0', 'a0', 4); addiu('t9', 't9', -1); bne('t9', 'zero', 'fill'); nop()

if kernel == 'int':
    # CRC32 table (reflected, 0xEDB88320)
    move('t0', 'zero'); li('t5', 0xEDB88320); move('a0', 's1')
    label('tab')
    move('t1', 't0'); li('t2', 8)
    label('bit')
    andi('t3', 't1', 1); srl('t1', 't1', 1); subu('t3', 'zero', 't3'); and_('t3', 't3', 't5'); xor('t1', 't1', 't3')
    addiu('t2', 't2', -1); bne('t2', 'zero', 'bit'); nop()
    sw('t1', 0, 'a0'); addiu('a0', 'a0', 4); addiu('t0', 't0', 1); li('t4', 256); bne('t0', 't4', 'tab'); nop()
    label('outer')
    move('a0', 's0'); li('t9', 4096); li('t0', 0xFFFFFFFF)
    label('crc')
    lbu('t1', 0, 'a0'); xor('t1', 't1', 't0'); andi('t1', 't1', 0xFF); sll('t1', 't1', 2); addu('t1', 't1', 's1')
    lw('t1', 0, 't1'); srl('t0', 't0', 8); xor('t0', 't0', 't1')
    addiu('a0', 'a0', 1); addiu('t9', 't9', -1); bne('t9', 'zero', 'crc'); nop()
    xor('s7', 's7', 't0'); sb('s7', 0, 's0')   # feed the result back into the data
    beq('zero', 'zero', 'outer'); nop()
elif kernel == 'float':
    # Matrix from the data (as small floats), then transform 64 vec4s
    label('outer')
    for r in range(16):
        lwc1(r, r * 4, 's0' if check else 's1')  # matrix in f0..f15 (table area: zero; --check: random bits)
    move('a0', 's0'); move('a1', 's2'); li('t9', 64)
    label('vec')
    lwc1(16, 0, 'a0'); lwc1(17, 4, 'a0'); lwc1(18, 8, 'a0'); lwc1(19, 12, 'a0')
    for row in range(4):
        mul_s(20, 0 + row * 4, 16); mul_s(21, 1 + row * 4, 17); mul_s(22, 2 + row * 4, 18); mul_s(23, 3 + row * 4, 19)
        add_s(20, 20, 21); add_s(22, 22, 23); add_s(24, 20, 22)
        swc1(24, row * 4, 'a1')
    addiu('a0', 'a0', 16); addiu('a1', 'a1', 16); addiu('t9', 't9', -1); bne('t9', 'zero', 'vec'); nop()
    mfc1('t0', 24); xor('s7', 's7', 't0')
    if check:
        move('a1', 's2'); li('t9', 256)
        label('sum')
        lw('t0', 0, 'a1'); sll('t1', 's7', 1); srl('t2', 's7', 31); xor('s7', 't1', 't2'); xor('s7', 's7', 't0')
        addiu('a1', 'a1', 4); addiu('t9', 't9', -1); bne('t9', 'zero', 'sum'); nop()
        li('s6', 0xD0E)
        label('done'); beq('zero', 'zero', 'done'); nop()
    beq('zero', 'zero', 'outer'); nop()
elif kernel == 'mmi':
    label('outer')
    move('a0', 's0'); addiu('a1', 's0', 0x800); move('a2', 's2'); li('t9', 128)
    label('row')
    lq('t0', 0, 'a0'); lq('t1', 0, 'a1')
    pextlb('t2', 'zero', 't0'); pextub('t3', 'zero', 't0')
    pextlb('t4', 'zero', 't1'); pextub('t5', 'zero', 't1')
    paddh('t2', 't2', 't4'); paddh('t3', 't3', 't5')
    psrlh('t2', 't2', 1); psrlh('t3', 't3', 1)
    ppacb('t6', 't3', 't2')
    sq('t6', 0, 'a2')
    addiu('a0', 'a0', 16); addiu('a1', 'a1', 16); addiu('a2', 'a2', 16); addiu('t9', 't9', -1); bne('t9', 'zero', 'row'); nop()
    lw('t0', 0, 's2'); xor('s7', 's7', 't0'); sw('s7', 0, 's0')
    beq('zero', 'zero', 'outer'); nop()
elif kernel == 'call':
    # C++ engine style: small functions called with jal and left with jr ra, a
    # non-leaf one with a stack frame, and a call through a pointer (jalr).
    # Every call and return is an indirect or unlinked jump for the JIT.
    label('outer')
    move('a0', 's0'); li('t9', 256)
    label('loop')
    lw('a1', 0, 'a0')
    jal('leaf'); nop()
    xor('s7', 's7', 'v0')
    jal('nonleaf'); move('a2', 'v0')
    addu('s7', 's7', 'v0')
    la('t8', 'leaf'); jalr('t8'); move('a1', 's7')
    xor('s7', 's7', 'v0')
    addiu('a0', 'a0', 4); addiu('t9', 't9', -1); bne('t9', 'zero', 'loop'); nop()
    sw('s7', 0, 's0')
    beq('zero', 'zero', 'outer'); nop()
    label('leaf')       # v0 = a1 * 3 + 1
    sll('v0', 'a1', 1); addu('v0', 'v0', 'a1'); jr('ra'); addiu('v0', 'v0', 1)
    label('nonleaf')    # v0 = leaf(leaf(a2))
    addiu('sp', 'sp', -16); sw('ra', 0, 'sp')
    jal('leaf'); move('a1', 'a2')
    jal('leaf'); move('a1', 'v0')
    lw('ra', 0, 'sp'); jr('ra'); addiu('sp', 'sp', 16)
else:
    raise SystemExit('unknown kernel ' + kernel)

for pos, n in fix:
    code[pos] |= (labels[n] - pos - 1) & 0xFFFF
for pos, n in jfix:
    code[pos] |= ((m.BASE + labels[n] * 4) >> 2) & 0x03FFFFFF
for pos, n in lafix:
    address = m.BASE + labels[n] * 4
    code[pos] |= (address >> 16) & 0xFFFF
    code[pos + 1] |= address & 0xFFFF
image = bytearray(0x1000)
for i, w in enumerate(code):
    struct.pack_into('<I', image, i * 4, w)
m.write_elf(sys.argv[1], bytes(image))
