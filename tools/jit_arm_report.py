#!/usr/bin/env python3
"""ARM instructions per EE instruction in a game's hot code, measured on a PC.

Usage: jit_arm_report.py RUN_DIR [--dump build-host/jit_arm_dump] [--top 40]

Takes a Vita run's EE hotspots (eehot.txt, 32-byte buckets weighted by samples)
and its EE RAM (eeram.bin), finds the block around each hot bucket (from the
previous branch's delay slot to the next one, as the EE executor partitions
code), compiles it with the Vita's ARM code generator (jit_arm_dump) and
disassembles it (arm-linux-gnueabihf-objdump). Reports, weighted by samples:
  - ARM instructions per EE instruction inside a chain of linked blocks (from the
    link entry: the prolog only runs when entering from C++)
  - where they go: loads/stores of guest registers (the context, r11/fp),
    the cycle quota, branch and exit handling
A tier 2 change shows here before any Vita run.
"""
import os, re, struct, subprocess, sys, tempfile, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def branch_targets(ram, lo=0x100000, hi=0x1000000):
    """Static targets of jal/j and conditional branches: blocks start there too."""
    targets = set()
    hi = min(hi, len(ram) - 4)
    words = struct.unpack_from('<%dI' % ((hi - lo) // 4), ram, lo)
    for i, op in enumerate(words):
        a = lo + i * 4
        top = op >> 26
        if top in (0x02, 0x03):
            targets.add(((a + 4) & 0xF0000000) | ((op & 0x03FFFFFF) << 2))
        elif top in (0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17) or (top == 0x01 and ((op >> 16) & 0x1F) in (0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13)):
            offset = op & 0xFFFF
            if offset & 0x8000:
                offset -= 0x10000
            targets.add(a + 4 + offset * 4)
    return targets


def block_start(ram, address, targets):
    """Back to the instruction after the previous branch's delay slot, or to a branch target."""
    def word(a):
        return struct.unpack_from('<I', ram, a)[0]

    def is_branch(op):
        top = op >> 26
        if top in (0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17):
            return True
        if top == 0x00 and (op & 0x3F) in (0x08, 0x09):  # jr, jalr
            return True
        if top == 0x01 and ((op >> 16) & 0x1F) in (0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13):
            return True
        if top == 0x11 and ((op >> 21) & 0x1F) == 0x08:  # bc1
            return True
        return False
    a = address
    for _ in range(256):
        if a in targets:
            break
        prev = a - 4
        if prev < 4:
            break
        if is_branch(word(prev - 4)):  # prev is a delay slot: a starts a block
            break
        if is_branch(word(prev)):      # a is a delay slot: its block starts before the branch
            a = prev
            continue
        a = prev
    return a


def classify(lines, link_entry):
    counts = collections.Counter()
    total = 0
    for text in lines[link_entry:]:
        total += 1
        if re.search(r'\[fp, #(4|12|8)\]|\[fp\]$|\[fp\]\s', text):
            counts['quota/PC/exception/delay fields'] += 1
        elif re.search(r'\[fp', text):
            counts['guest registers (context loads/stores)'] += 1
        elif re.match(r'(b|bl|ble|bgt|beq|bne|bcc|bcs|blt|bge)\b', text) or 'pc' in text.split(',')[0]:
            counts['branches/jumps'] += 1
        else:
            counts['compute'] += 1
    return total, counts


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        raise SystemExit(__doc__)
    run = args[0]
    dump = sys.argv[sys.argv.index('--dump') + 1] if '--dump' in sys.argv else '/root/build-host/jit_arm_dump'
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 40
    ram = open(os.path.join(run, 'eeram.bin'), 'rb').read()
    lines = open(os.path.join(run, 'eehot.txt')).read().splitlines()[1:]
    buckets = []
    for l in lines[:top]:
        p = l.split()
        a = int(p[0], 16)
        if a < 0x100000 or a >= len(ram):
            continue
        buckets.append((a, float(p[1].rstrip('%'))))
    targets = branch_targets(ram)
    starts = collections.OrderedDict()
    for a, w in buckets:
        s = block_start(ram, a, targets)
        starts[s] = starts.get(s, 0) + w
    with tempfile.TemporaryDirectory() as tmp:
        out = subprocess.run([dump, os.path.join(run, 'eeram.bin')] + ['%X' % s for s in starts] + ['--out', tmp],
                             capture_output=True, text=True).stdout
        info = {}
        for line in out.splitlines():
            s, e, n, size = line.split()
            info[int(s, 16)] = (int(n), int(size))
        weighted_arm = weighted_ee = 0.0
        totals = collections.Counter()
        rows = []
        for s, w in starts.items():
            path = os.path.join(tmp, '%08X.bin' % s)
            if s not in info or not os.path.exists(path):
                continue
            dis = subprocess.run(['arm-linux-gnueabihf-objdump', '-D', '-b', 'binary', '-marm', path], capture_output=True, text=True).stdout
            text = [l.split('\t', 2)[2].strip() for l in dis.splitlines() if re.match(r'\s*[0-9a-f]+:\t[0-9a-f]{8} \t', l)]
            # Link entry: after the chained prolog's "sub sp, sp, #512"
            link = next((i + 1 for i, t in enumerate(text) if t.startswith('sub\tsp, sp, #512')), 0)
            # Stop at the literal pool / slow path end: count up to the last "ldr pc" or "mov sp, r3" epilog
            end = len(text)
            for i, t in enumerate(text):
                if t.startswith('ldmfd\tsp!, {r4'):
                    end = i + 1
                    break
            n_ee = info[s][0]
            arm, counts = classify(text[:end], link)
            weighted_arm += w * arm
            weighted_ee += w * n_ee
            totals.update({k: v * w for k, v in counts.items()})
            rows.append((w, s, n_ee, arm))
    if weighted_ee == 0:
        raise SystemExit('no blocks compiled')
    print('Hot EE blocks of %s: %d blocks, %.1f%% of EE samples' % (run, len(rows), sum(r[0] for r in rows)))
    print('ARM instructions per EE instruction (from the link entry, weighted by samples): %.2f' % (weighted_arm / weighted_ee))
    all_w = sum(totals.values())
    for k, v in totals.most_common():
        print('  %-42s %5.1f%%' % (k, 100.0 * v / all_w))
    print('Heaviest blocks (share, start, EE instructions, ARM instructions):')
    for w, s, n, arm in sorted(rows, reverse=True)[:10]:
        print('  %5.1f%%  %08X  %3d  %4d  (%.1f per EE instruction)' % (w, s, n, arm, arm / max(1, n)))


if __name__ == '__main__':
    main()
