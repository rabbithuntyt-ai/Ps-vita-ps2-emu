#!/usr/bin/env python3
"""VU1 micro program -> C++ (ahead of time), for one known game program.

Usage: vu_aot.py vu1micro.txt BEGIN END ENTRY[,ENTRY...] NAME > out.cpp

vu1micro.txt is the PC runner's disassembly (--vu1-stats writes it: address, upper word,
lower word, upper | lower). BEGIN/END (hex) bound the code translated; ENTRY are the program
start addresses the function accepts. The output defines
    int NAME(VU_AOT_REGS& r, uint8_t* mem, uint32_t entry, VU_AOT_KICK kick, void* ctx)
(src/common/VuAot.h) and NAME_code[], the code it was made from (the caller compares micro
memory with it before using the function).

Semantics follow Play's VU JIT (Source/ee/VUShared.cpp, MA_VU_Lower.cpp):
- A pair's upper and lower both read the registers as they were before the pair; the upper
  result is committed after the lower (the upper wins when both write a register).
- With the I bit the lower word is loaded into I after the upper read the old I.
- VF results are visible to the next pair (the VU interlocks on them). Q is taken as ready
  at its next use (checked: every use in the translated code is 7+ pairs after its DIV).
- The pair after a branch (delay slot) runs before the jump; after an E bit, one more pair.
- VI registers are 32-bit as in Play (ILW loads a word, MTIR moves the whole lane).
- CLIP: flags = (previous << 6) | new six bits (x>|w|, x<-|w|, y.., z..).
Unsupported instructions, a delay slot holding a branch, or a branch reading a VI written by
the pair just before it (a VU hazard Play models separately) stop the generator.
"""
import re
import sys

LANES = 'xyzw'


def lanes(mask):
    return [LANES.index(c) for c in mask]


def parse_vf(op):
    m = re.fullmatch(r'VF(\d+)([xyzw]*)', op.strip())
    if not m:
        raise ValueError('not a VF operand: ' + op)
    return int(m.group(1)), m.group(2)


def parse_vi(op):
    m = re.fullmatch(r'VI(\d+)', op.strip())
    if not m:
        raise ValueError('not a VI operand: ' + op)
    return int(m.group(1))


def imm16(text):
    v = int(text.strip().lstrip('$'), 16) & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def parse_mem(op):
    # $FFFD(VI5)x
    m = re.fullmatch(r'\$([0-9A-Fa-f]+)\(VI(\d+)\)([xyzw]*)', op.strip())
    if not m:
        raise ValueError('not a memory operand: ' + op)
    return imm16(m.group(1)), int(m.group(2)), m.group(3)


class Pair:
    def __init__(self, address, upper_word, lower_word, upper, lower):
        self.address = address
        self.uw = upper_word
        self.lw = lower_word
        self.upper = upper.strip()
        self.lower = lower.strip()
        self.ibit = bool(upper_word & 0x80000000)
        self.ebit = bool(upper_word & 0x40000000)
        if upper_word & 0x38000000:
            raise ValueError('%04X: M/D/T bits not supported' % address)


def load(path, begin, end):
    pairs = {}
    for line in open(path):
        m = re.match(r'([0-9A-F]{4}) ([0-9A-F]{8}) ([0-9A-F]{8})  (.*?)\|(.*)$', line.rstrip('\n'))
        if not m:
            continue
        address = int(m.group(1), 16)
        if address < begin or address >= end or address in pairs:
            continue
        pairs[address] = Pair(address, int(m.group(2), 16), int(m.group(3), 16), m.group(4), m.group(5))
    for a in range(begin, end, 8):
        if a not in pairs:
            raise ValueError('missing pair %04X' % a)
    return pairs


def split_op(text):
    text = text.strip()
    if not text:
        return 'NOP', []
    parts = text.split(None, 1)
    name = parts[0]
    ops = [o.strip() for o in parts[1].split(',')] if len(parts) > 1 else []
    return name, ops


# ---------------------------------------------------------------- upper instructions

def upper_source(op, lane):
    """Second source of an upper op: VFn (broadcast if one lane given), I or Q."""
    op = op.strip()
    if op == 'I':
        return 's.i.f'
    if op == 'Q':
        return 's.q.f'
    reg, mask = parse_vf(op)
    if len(mask) == 1:
        return 's.vf[%d].f[%d]' % (reg, LANES.index(mask))
    return 's.vf[%d].f[%d]' % (reg, lane)


def upper_code(p):
    """(statements computing temporaries, statements committing them, registers written)."""
    name, ops = split_op(p.upper)
    if name == 'NOP':
        return [], [], set()
    compute, commit = [], []
    if name in ('MUL', 'ADD', 'MADD', 'MAX', 'MINI'):
        fd, mask = parse_vf(ops[0])
        fs, _ = parse_vf(ops[1])
        for l in lanes(mask):
            a = 's.vf[%d].f[%d]' % (fs, l)
            b = upper_source(ops[2], l)
            if name == 'MUL':
                e = '%s * %s' % (a, b)
            elif name == 'ADD':
                e = '%s + %s' % (a, b)
            elif name == 'MADD':
                e = 's.acc.f[%d] + %s * %s' % (l, a, b)
            elif name == 'MAX':
                e = 'VuMax(%s, %s)' % (a, b)
            else:
                e = 'VuMini(%s, %s)' % (a, b)
            compute.append('const float u%d = %s;' % (l, e))
            if fd != 0:
                commit.append('s.vf[%d].f[%d] = u%d;' % (fd, l, l))
        return compute, commit, {('vf', fd)}
    if name in ('MULA', 'MADDA', 'ADDA'):
        m = re.fullmatch(r'ACC([xyzw]+)', ops[0])
        mask = m.group(1)
        fs, _ = parse_vf(ops[1])
        for l in lanes(mask):
            a = 's.vf[%d].f[%d]' % (fs, l)
            b = upper_source(ops[2], l)
            if name == 'MULA':
                e = '%s * %s' % (a, b)
            elif name == 'MADDA':
                e = 's.acc.f[%d] + %s * %s' % (l, a, b)
            else:
                e = '%s + %s' % (a, b)
            compute.append('const float u%d = %s;' % (l, e))
            commit.append('s.acc.f[%d] = u%d;' % (l, l))
        return compute, commit, {('acc', 0)}
    if name in ('ITOF0', 'FTOI0', 'FTOI4'):
        ft, mask = parse_vf(ops[0])
        fs, _ = parse_vf(ops[1])
        for l in lanes(mask):
            if name == 'ITOF0':
                compute.append('const float u%d = static_cast<float>(s.vf[%d].i[%d]);' % (l, fs, l))
                if ft != 0:
                    commit.append('s.vf[%d].f[%d] = u%d;' % (ft, l, l))
            else:
                scale = '' if name == 'FTOI0' else ' * 16.0f'
                compute.append('const int32_t u%d = VuFtoi(s.vf[%d].f[%d]%s);' % (l, fs, l, scale))
                if ft != 0:
                    commit.append('s.vf[%d].i[%d] = u%d;' % (ft, l, l))
        return compute, commit, {('vf', ft)}
    if name == 'CLIP':
        fs, mask = parse_vf(ops[0])
        ft, wmask = parse_vf(ops[1])
        if mask != 'xyz' or wmask != 'w':
            raise ValueError('%04X: CLIP form' % p.address)
        compute.append('const uint32_t uclip = VuClip(s.vf[%d], s.vf[%d].f[3]);' % (fs, ft))
        commit.append('s.clip = (s.clip << 6) | uclip;')
        return compute, commit, {('clip', 0)}
    raise ValueError('%04X: upper %s not supported' % (p.address, p.upper))


# ---------------------------------------------------------------- lower instructions

def lower_code(p):
    """(statements, VI registers written, branch info or None)."""
    if p.ibit:
        return ['s.i.u = 0x%08Xu;' % p.lw], set(), None
    name, ops = split_op(p.lower)
    w = set()
    out = []

    def vi_dst(n, expr):
        if n != 0:
            out.append('s.vi[%d] = %s;' % (n, expr))
            w.add(n)

    if name == 'NOP':
        return [], w, None
    if name == 'LQ':
        ft, mask = parse_vf(ops[0])
        off, base, _ = parse_mem(ops[1])
        if ft != 0:
            out.append('{ const uint8_t* a = mem + (((s.vi[%d] + %d) << 4) & 0x3FFF);' % (base, off))
            for l in lanes(mask):
                out.append('  std::memcpy(&s.vf[%d].u[%d], a + %d, 4);' % (ft, l, l * 4))
            out.append('}')
        return out, w, None
    if name == 'SQ':
        fs, mask = parse_vf(ops[0])
        off, base, _ = parse_mem(ops[1])
        out.append('{ uint8_t* a = mem + (((s.vi[%d] + %d) << 4) & 0x3FFF);' % (base, off))
        for l in lanes(mask):
            out.append('  std::memcpy(a + %d, &s.vf[%d].u[%d], 4);' % (l * 4, fs, l))
        out.append('}')
        return out, w, None
    if name == 'ILW':
        it = parse_vi(ops[0])
        off, base, field = parse_mem(ops[1])
        if len(field) != 1:
            raise ValueError('%04X: ILW field' % p.address)
        if it != 0:
            out.append('{ uint32_t v; std::memcpy(&v, mem + ((((s.vi[%d] + %d) << 4) + %d) & 0x3FFF), 4); s.vi[%d] = v; }'
                       % (base, off, LANES.index(field) * 4, it))
            w.add(it)
        return out, w, None
    if name == 'ISW':
        it = parse_vi(ops[0])
        off, base, field = parse_mem(ops[1])
        out.append('{ uint8_t* a = mem + (((s.vi[%d] + %d) << 4) & 0x3FFF); const uint32_t v = s.vi[%d] & 0xFFFF;' % (base, off, it))
        for l in lanes(field):
            out.append('  std::memcpy(a + %d, &v, 4);' % (l * 4))
        out.append('}')
        return out, w, None
    if name == 'IADDIU':
        vi_dst(parse_vi(ops[0]), 's.vi[%d] + 0x%Xu' % (parse_vi(ops[1]), int(ops[2].lstrip('$'), 16) & 0xFFFF))
        return out, w, None
    if name == 'IADDI':
        vi_dst(parse_vi(ops[0]), 's.vi[%d] + static_cast<uint32_t>(%d)' % (parse_vi(ops[1]), imm16(ops[2])))
        return out, w, None
    if name == 'IADD':
        vi_dst(parse_vi(ops[0]), 's.vi[%d] + s.vi[%d]' % (parse_vi(ops[1]), parse_vi(ops[2])))
        return out, w, None
    if name == 'IAND':
        vi_dst(parse_vi(ops[0]), 's.vi[%d] & s.vi[%d]' % (parse_vi(ops[1]), parse_vi(ops[2])))
        return out, w, None
    if name == 'MTIR':
        fs, field = parse_vf(ops[1])
        vi_dst(parse_vi(ops[0]), 's.vf[%d].u[%d]' % (fs, LANES.index(field)))
        return out, w, None
    if name == 'MFIR':
        ft, mask = parse_vf(ops[0])
        vs = parse_vi(ops[1])
        if ft != 0:
            for l in lanes(mask):
                out.append('s.vf[%d].i[%d] = static_cast<int16_t>(s.vi[%d]);' % (ft, l, vs))
        return out, w, None
    if name in ('MR32', 'MOVE'):
        ft, mask = parse_vf(ops[0])
        fs, _ = parse_vf(ops[1])
        if ft != 0:
            src = ['s.vf[%d].u[%d]' % (fs, ((l + 1) & 3) if name == 'MR32' else l) for l in range(4)]
            out.append('{ const uint32_t t[4] = {%s};' % ', '.join(src))
            for l in lanes(mask):
                out.append('  s.vf[%d].u[%d] = t[%d];' % (ft, l, l))
            out.append('}')
        return out, w, None
    if name == 'XTOP':
        vi_dst(parse_vi(ops[0]), 'r.top')
        return out, w, None
    if name == 'XGKICK':
        out.append('kick(ctx, s.vi[%d]);' % parse_vi(ops[0]))
        return out, w, None
    if name == 'FCAND':
        if parse_vi(ops[0]) != 1:
            raise ValueError('FCAND target')
        vi_dst(1, '(s.clip & 0x%Xu) != 0 ? 1u : 0u' % (int(ops[1].lstrip('$'), 16)))
        return out, w, None
    if name == 'DIV':
        if ops[0] != 'Q':
            raise ValueError('DIV form')
        fs, ff = parse_vf(ops[1])
        ft, tf = parse_vf(ops[2])
        out.append('s.q = VuDiv(s.vf[%d].u[%d], s.vf[%d].u[%d]);' % (fs, LANES.index(ff), ft, LANES.index(tf)))
        return out, w, None
    if name == 'B':
        return out, w, ('B', None, None, int(ops[0].lstrip('$'), 16))
    if name in ('IBNE', 'IBEQ'):
        return out, w, (name, parse_vi(ops[0]), parse_vi(ops[1]), int(ops[2].lstrip('$'), 16))
    raise ValueError('%04X: lower %s not supported' % (p.address, p.lower))


def pair_body(p):
    uc, ucommit, _ = upper_code(p)
    lc, _, branch = lower_code(p)
    lines = []
    if uc or ucommit or lc:
        lines.append('{')
        lines += ['  ' + x for x in uc]
        lines += ['  ' + x for x in lc]
        lines += ['  ' + x for x in ucommit]
        lines.append('}')
    return lines, branch


def reachable(pairs, entries, begin, end):
    """Pairs a program started at one of the entries can execute."""
    seen = set()
    work = list(entries)
    while work:
        a = work.pop()
        if a in seen:
            continue
        if a < begin or a >= end:
            raise ValueError('reaches %04X, outside the translated range' % a)
        seen.add(a)
        p = pairs[a]
        _, _, branch = lower_code(p)
        if branch:
            # the delay slot runs inlined in the branch
            work.append(branch[3])
            if branch[0] != 'B':
                work.append(a + 16)
        elif not p.ebit:
            work.append(a + 8)
        # after an E bit the next pair runs inlined, then the program ends
    return seen


def generate(path, begin, end, entries, name):
    pairs = load(path, begin, end)
    live = reachable(pairs, entries, begin, end)
    out = []
    out.append('// Generated by tools/vu_aot.py from %s (0x%04X-0x%04X): do not edit.' % (path.split('/')[-1], begin, end))
    out.append('#include "VuAot.h"')
    out.append('')
    words = []
    for a in range(begin, end, 8):
        words.append('0x%08Xu, 0x%08Xu' % (pairs[a].lw, pairs[a].uw))
    out.append('extern const uint32_t %s_code[%d];' % (name, len(words) * 2))
    out.append('extern const uint32_t %s_begin, %s_end;' % (name, name))
    out.append('const uint32_t %s_code[%d] = {' % (name, len(words) * 2))
    for i in range(0, len(words), 4):
        out.append('    ' + ', '.join(words[i:i + 4]) + ',')
    out.append('};')
    out.append('const uint32_t %s_begin = 0x%04X, %s_end = 0x%04X;' % (name, begin, name, end))
    out.append('')
    out.append('int %s(VU_AOT_REGS& r, uint8_t* mem, uint32_t entry, VU_AOT_KICK kick, void* ctx)' % name)
    out.append('{')
    out.append('\tVU_AOT_REGS s = r;')
    out.append('\tswitch(entry)')
    out.append('\t{')
    for e in entries:
        out.append('\tcase 0x%04X: goto L_%04X;' % (e, e))
    out.append('\tdefault: return 0;')
    out.append('\t}')

    for a in range(begin, end, 8):
        if a not in live:
            continue
        p = pairs[a]
        body, branch = pair_body(p)
        out.append('L_%04X: // %s | %s' % (a, p.upper, p.lower if not p.ibit else 'LOI'))
        out += ['\t' + x for x in body]
        prev = pairs.get(a - 8)
        if branch:
            kind, t, sreg, target = branch
            if target < begin or target >= end:
                raise ValueError('%04X: branch out of range' % a)
            if prev is not None and kind != 'B':
                _, written, _ = lower_code(prev)
                if t in written or sreg in written:
                    raise ValueError('%04X: branch reads a VI written by the pair before' % a)
            slot = pairs[a + 8]
            sbody, sbranch = pair_body(slot)
            if sbranch or slot.ebit:
                raise ValueError('%04X: branch or E bit in a delay slot' % a)
            if kind == 'B':
                cond = 'true'
            elif kind == 'IBNE':
                cond = 's.vi[%d] != s.vi[%d]' % (t, sreg)
            else:
                cond = 's.vi[%d] == s.vi[%d]' % (t, sreg)
            out.append('\t{')
            out.append('\t\tconst bool take = %s;' % cond)
            out += ['\t\t' + x for x in sbody]
            out.append('\t\tif(take) goto L_%04X;' % target)
            if kind != 'B':
                out.append('\t\tgoto L_%04X;' % (a + 16))
            out.append('\t}')
        if p.ebit:
            slot = pairs[a + 8]
            sbody, sbranch = pair_body(slot)
            if sbranch:
                raise ValueError('%04X: branch after an E bit' % a)
            out += ['\t' + x for x in sbody]
            out.append('\tgoto done;')
    out.append('\treturn 0; // ran off the end of the translated code')
    out.append('done:')
    out.append('\tr = s;')
    out.append('\treturn 1;')
    out.append('}')
    # Labels only used by fall-through or not at all: silence the warning
    return '\n'.join(out) + '\n'


def main():
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    path = sys.argv[1]
    begin = int(sys.argv[2], 16)
    end = int(sys.argv[3], 16)
    entries = [int(e, 16) for e in sys.argv[4].split(',')]
    sys.stdout.write(generate(path, begin, end, entries, sys.argv[5]))


if __name__ == '__main__':
    main()
