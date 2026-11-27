#!/usr/bin/env python3
"""Guest register loads and stores in a game's hot EE blocks, weighted by EE samples.

Usage: jit_reg_traffic.py RUN_DIR [--dump /root/build-host/jit_arm_dump] [--top 200]

Compiles the hot blocks (as jit_arm_report.py does) and counts, for each EE GPR, the ARM loads and
stores of its context slot (CMIPS::m_State.nGPR at offset 16, 16 bytes a register) in the code
from the link entry on. Weighted by the block's share of EE samples. Shows which registers a
cross-block register cache would save the most traffic on.
"""
import os, re, subprocess, sys, tempfile, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import jit_arm_report as jr

NAMES = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
         's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    run = args[0]
    dump = sys.argv[sys.argv.index('--dump') + 1] if '--dump' in sys.argv else '/root/build-host/jit_arm_dump'
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 200
    ram = open(os.path.join(run, 'eeram.bin'), 'rb').read()
    lines = open(os.path.join(run, 'eehot.txt')).read().splitlines()[1:]
    buckets = []
    for l in lines[:top]:
        p = l.split()
        if len(p) < 2 or not p[1].endswith('%'):
            continue
        a = int(p[0], 16)
        if 0x100000 <= a < len(ram):
            buckets.append((a, float(p[1].rstrip('%'))))
    targets = jr.branch_targets(ram)
    starts = collections.OrderedDict()
    for a, w in buckets:
        s = jr.block_start(ram, a, targets)
        starts[s] = starts.get(s, 0) + w
    loads = collections.Counter()
    stores = collections.Counter()
    total_weight = 0.0
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([dump, os.path.join(run, 'eeram.bin')] + ['%X' % s for s in starts] + ['--out', tmp], capture_output=True, text=True)
        for s, w in starts.items():
            path = os.path.join(tmp, '%08X.bin' % s)
            if not os.path.exists(path):
                continue
            dis = subprocess.run(['arm-linux-gnueabihf-objdump', '-D', '-b', 'binary', '-marm', path], capture_output=True, text=True).stdout
            text = [l.split('\t', 2)[2].strip() for l in dis.splitlines() if re.match(r'\s*[0-9a-f]+:\t[0-9a-f]{8} \t', l)]
            # From the link entry (after the chained prolog's "sub sp, sp, #512")
            entry = next((i + 1 for i, t in enumerate(text) if t.startswith('sub\tsp, sp, #512')), 0)
            total_weight += w
            for t in text[entry:]:
                m = re.match(r'(ldr|str|ldrd|strd)\t[^,]+(?:, [^,]+)?, \[fp, #(\d+)\]', t)
                if not m:
                    continue
                offset = int(m.group(2))
                if 16 <= offset < 16 + 32 * 16:
                    reg = (offset - 16) // 16
                    (loads if m.group(1).startswith('ldr') else stores)[reg] += w
    print('hot blocks: %d, %.1f%% of EE samples' % (len(starts), total_weight))
    print('guest register traffic in their code (loads, stores), weighted by samples (per 100% of EE time):')
    rows = sorted(set(loads) | set(stores), key=lambda r: -(loads[r] + stores[r]))
    scale = 100.0 / total_weight if total_weight else 0
    for r in rows[:16]:
        print('  %-4s  loads %6.1f  stores %6.1f' % (NAMES[r], loads[r] * scale, stores[r] * scale))


if __name__ == '__main__':
    main()
