# Random EE integer code (32 and 64-bit ops, forward branches with delay slots,
# loads/stores) ending with a checksum of both halves of every register:
# results must not depend on JIT optimizations. Ends with s6 = 0xD0E, s7 = checksum.
# Usage: make_ee_fuzz_elf.py out.elf SEED [COUNT] [--loops]
#   --loops: also counted loops (t9) around random bodies, some with forward
#            branches inside (exercises loop register promotion)
#   --calls: also calls (jal, jalr) to leaf and non-leaf functions (ra saved on the
#            stack), some returning past their call site (they change ra)
#   --spr: loads and stores in the scratchpad (0x70000000: memory handlers, not RAM)
#   --likely: also likely branches (beql...: the delay slot runs only when taken),
#            some loops closed by bnel. Off by default: the recorded checksums of the
#            host tests are for programs without them.
import os, random, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_test_elf as m

R = {n: i for i, n in enumerate('zero at v0 v1 a0 a1 a2 a3 t0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 s6 s7 t8 t9 k0 k1 gp sp fp ra'.split())}
POOL = [R[n] for n in 't0 t1 t2 t3 t4 t5 t6 t7 s0 s1 s2 s3 s4 s5 v0 v1'.split()] + [0]
rng = random.Random(int(sys.argv[2]))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
loops = '--loops' in sys.argv
likely = '--likely' in sys.argv
calls = '--calls' in sys.argv
spr = '--spr' in sys.argv  # loads and stores in the scratchpad (not RAM: the JIT's slow path)
count = int(args[2]) if len(args) > 2 else 3000
code = []

def I(op, rs, rt, imm): code.append((op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF))
def Rr(fn, rs, rt, rd, sa=0): code.append((rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn)
def reg(): return rng.choice(POOL)
def dst(): return rng.choice(POOL[:-1])
A0 = R['a0']

def alu():
    k = rng.randrange(24)
    if k == 0: I(0x09, reg(), dst(), rng.randrange(65536))           # addiu
    elif k == 1: Rr(0x21, reg(), reg(), dst())                        # addu
    elif k == 2: Rr(0x23, reg(), reg(), dst())                        # subu
    elif k == 3: Rr(rng.choice([0, 2, 3]), 0, reg(), dst(), rng.randrange(32))  # sll/srl/sra
    elif k == 4: Rr(rng.choice([0x24, 0x25, 0x26, 0x27]), reg(), reg(), dst())  # and/or/xor/nor
    elif k == 5: I(rng.choice([0x0C, 0x0D, 0x0E]), reg(), dst(), rng.randrange(65536))  # andi/ori/xori
    elif k == 6: I(0x0F, 0, dst(), rng.randrange(65536))             # lui
    elif k == 7: Rr(rng.choice([0x2A, 0x2B]), reg(), reg(), dst())    # slt/sltu
    elif k == 8: I(rng.choice([0x0A, 0x0B]), reg(), dst(), rng.randrange(65536))  # slti/sltiu
    elif k == 9: Rr(0x2D, reg(), 0, dst())                            # move (daddu rd, rs, zero)
    elif k == 10: Rr(0x2D, reg(), reg(), dst())                       # daddu
    elif k == 11: Rr(0x2F, reg(), reg(), dst())                       # dsubu
    elif k == 12: Rr(rng.choice([0x3C, 0x3F, 0x3E, 0x38]), 0, reg(), dst(), rng.randrange(32))  # dsll32/dsra32/dsrl32/dsll
    elif k == 13: Rr(rng.choice([0x0A, 0x0B]), reg(), reg(), dst())   # movz/movn
    elif k == 14: I(0x23, A0, dst(), rng.randrange(64) * 4)            # lw
    elif k == 15: I(0x2B, A0, reg(), rng.randrange(64) * 4)            # sw
    elif k == 16: I(0x37, A0, dst(), rng.randrange(32) * 8)            # ld
    elif k == 17: I(0x3F, A0, reg(), rng.randrange(32) * 8)            # sd
    elif k == 18: I(rng.choice([0x20, 0x21, 0x24, 0x25]), A0, dst(), rng.randrange(128) * 2)  # lb/lh/lbu/lhu
    elif k == 19: I(0x27, A0, dst(), rng.randrange(64) * 4)            # lwu
    elif k == 20: Rr(0x25, reg(), 0, dst())                            # or move
    elif k == 21: Rr(0x14 if rng.random() < 0.5 else 0x16, reg(), reg(), dst())  # dsllv/dsrlv
    elif k == 22: I(0x19, reg(), dst(), rng.randrange(65536))          # daddiu
    else: Rr(rng.choice([0x04, 0x06, 0x07]), reg(), reg(), dst())      # sllv/srlv/srav

def branch(skip):
    k = rng.randrange(13 if likely else 7)
    if k == 0: I(0x04, reg(), reg(), skip)        # beq
    elif k == 1: I(0x05, reg(), reg(), skip)      # bne
    elif k == 2: I(0x06, reg(), 0, skip)          # blez
    elif k == 3: I(0x07, reg(), 0, skip)          # bgtz
    elif k == 4: I(0x01, reg(), 0, skip)          # bltz
    elif k == 5: I(0x01, reg(), 1, skip)          # bgez
    elif k == 6: I(0x05, reg(), 0, skip)          # bne rs, zero
    # Likely forms: the delay slot runs only when taken
    elif k == 7: I(0x14, reg(), reg(), skip)      # beql
    elif k == 8: I(0x15, reg(), rng.choice([0, reg()]), skip)  # bnel
    elif k == 9: I(0x16, reg(), 0, skip)          # blezl
    elif k == 10: I(0x17, reg(), 0, skip)         # bgtzl
    elif k == 11: I(0x01, reg(), 2, skip)         # bltzl
    else: I(0x01, reg(), 3, skip)                 # bgezl

def li(r, v):
    I(0x0F, 0, r, v >> 16); I(0x0D, r, r, v & 0xFFFF)

li(R['sp'], 0x01F00000)
li(A0, 0x70000000 if spr else 0x00300000)
for r in POOL[:-1]:
    li(r, rng.randrange(1 << 32))
    if rng.random() < 0.3:
        Rr(0x3C, 0, r, r, 0)                     # dsll32: upper half not a sign extension
        I(0x0D, r, r, rng.randrange(65536))
T9 = R['t9']
RA, SP, K1 = R['ra'], R['sp'], R['k1']
# Functions (--calls): kind 0 leaf, 1 leaf with a forward branch, 2 non-leaf (calls a
# leaf, ra on the stack), 3 leaf returning 2 instructions past its call site
FUNCS = [0, 1, 2, 3, 0, 2] if calls else []
patches = []  # (code index, function, 'jal' | 'li')
def call_site():
    f = rng.randrange(len(FUNCS))
    if rng.random() < 0.7:
        patches.append((len(code), f, 'jal')); code.append(0)
    else:
        patches.append((len(code), f, 'li')); code.append(0); code.append(0)
        Rr(0x09, K1, 0, RA)                      # jalr ra, k1
    alu()                                        # delay slot
    if FUNCS[f] == 3:
        alu(); alu()                             # skipped by the callee
for i in range(count):
    if calls and rng.random() < 0.05:
        call_site()
    elif loops and rng.random() < 0.03:
        I(0x0D, 0, T9, rng.randrange(2, 20))     # ori t9, zero, N
        top = len(code)
        for j in range(rng.randrange(3, 30)):
            if rng.random() < 0.1:
                skip = rng.randrange(1, 3)
                branch(skip); alu()
                for k in range(skip): alu()
            else:
                alu()
        I(0x09, T9, T9, 0xFFFF)                  # addiu t9, t9, -1
        back = 0x15 if (likely and rng.random() < 0.3) else 0x05
        I(back, T9, 0, top - (len(code) + 1))    # bne(l) t9, zero, top
        alu()                                    # delay slot
    elif rng.random() < 0.12:
        skip = rng.randrange(1, 4)
        branch(skip)
        alu()                                    # delay slot
        for j in range(skip): alu()
    else:
        alu()
# Checksum: s7 = rotl(s7, 1) ^ lo ^ hi for every register
li(R['s7'], 0x12345678)
for r in POOL[:-1]:
    Rr(0x26, R['s7'], r, R['s7'])                # xor s7, s7, r (low half)
    Rr(0x3F, 0, r, R['t8'], 0)                   # dsra32 t8, r, 0 (upper half)
    Rr(0x26, R['s7'], R['t8'], R['s7'])
    Rr(0x00, 0, R['s7'], R['t9'], 1)
    Rr(0x02, 0, R['s7'], R['t8'], 31)
    Rr(0x25, R['t9'], R['t8'], R['s7'])
li(R['s6'], 0xD0E)
I(0x04, 0, 0, 0xFFFF); code.append(0)            # b . ; nop
addresses = []
for f, kind in enumerate(FUNCS):
    addresses.append(m.BASE + len(code) * 4)
    if kind == 2:
        I(0x09, SP, SP, 0xFFF0)                  # addiu sp, sp, -16
        I(0x3F, SP, RA, 0)                       # sd ra, 0(sp)
        for j in range(rng.randrange(1, 6)): alu()
        leaf = rng.choice([g for g, k in enumerate(FUNCS) if k in (0, 1)])
        patches.append((len(code), leaf, 'jal')); code.append(0)
        alu()
        for j in range(rng.randrange(1, 6)): alu()
        I(0x37, SP, RA, 0)                       # ld ra, 0(sp)
        Rr(0x08, RA, 0, 0)                       # jr ra
        I(0x09, SP, SP, 16)                      # addiu sp, sp, 16 (delay slot)
        continue
    for j in range(rng.randrange(2, 12)): alu()
    if kind == 1:
        skip = rng.randrange(1, 3)
        branch(skip); alu()
        for k in range(skip): alu()
        alu()
    if kind == 3:
        I(0x09, RA, RA, 8)                       # addiu ra, ra, 8
    Rr(0x08, RA, 0, 0)                           # jr ra
    alu()                                        # delay slot
for index, f, how in patches:
    target = addresses[f]
    if how == 'jal':
        code[index] = (0x03 << 26) | ((target >> 2) & 0x03FFFFFF)
    else:
        code[index] = (0x0F << 26) | (K1 << 16) | (target >> 16)
        code[index + 1] = (0x0D << 26) | (K1 << 21) | (K1 << 16) | (target & 0xFFFF)
image = bytearray(0x40000)
for i, w in enumerate(code):
    struct.pack_into('<I', image, i * 4, w)
m.write_elf(sys.argv[1], bytes(image))
