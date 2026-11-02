#!/usr/bin/env python3
# The "performance gap": host (ARM) instructions the emulator executes per PS2
# EE instruction, for endless game-like EE workloads, measured exactly under
# qemu-arm with the ARM (Vita-equivalent) build. 1.0 would be one-to-one.
#
# Each workload runs twice (FRAMES_A and FRAMES_B emulated frames); the
# difference cancels start-up and compilation, leaving the steady-state cost:
#   gap = (ARM instructions B - A) / (EE instructions B - A)
# It counts everything the emulation thread runs (JIT code, memory handlers,
# timers, IOP and housekeeping), not just generated code; other host threads
# (the runner's polling loop, profiler) are left out. qemu-user runs each host
# thread as its own virtual CPU ("Trace N:"): the emulation thread is the one
# executing the most ARM-state (JIT) code.
#
# Usage: perf_gap.py --runner build-arm/vitaps2_host [--sysroot /usr/arm-linux-gnueabihf]
#                    [--history docs/perf/history.csv --svg docs/perf/gap.svg --label TEXT]
import argparse, collections, csv, datetime, os, re, subprocess, sys, tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
WORKLOADS = [
    # key, title, generator command (output path appended)
    ('int', 'Integer (CRC loop)', ['make_workload_elf.py', None, 'int']),
    ('float', 'Float (matrix x vector)', ['make_workload_elf.py', None, 'float']),
    ('mmi', '128-bit MMI (pixel blend)', ['make_workload_elf.py', None, 'mmi']),
    ('mem', 'Loads/stores (all sizes)', ['make_membench_elf.py', None, '100000000']),
]
FRAMES_A, FRAMES_B = 2, 4


def count_arm_instructions(stream):
    """Executed guest instructions of the emulation thread, from
    'qemu-arm -d in_asm,exec,nochain' output."""
    blocks, flags, runs = {}, {}, collections.Counter()  # runs: (cpu, pc)
    block_pc, buf = None, ''
    ee = None

    def finish():
        nonlocal block_pc, buf
        if block_pc is not None and buf:
            blocks[block_pc] = bytes.fromhex(buf)  # a re-translation replaces the old code
        block_pc, buf = None, ''

    for raw in stream:
        line = raw.decode('utf-8', 'replace')
        if line.startswith('IN:'):
            finish()
            block_pc = -1
        elif block_pc == -1 and line.startswith('0x') and line.rstrip().endswith(':'):
            block_pc = int(line.split(':')[0], 16)  # the block's own address (threads interleave)
        elif line.startswith('OBJD-T:'):
            buf += line.split(':', 1)[1].strip()
        elif line.startswith('Trace'):
            finish()
            inner = line[line.index('[') + 1:line.index(']')].split('/')
            pc = int(inner[1], 16)
            cpu = int(line.split()[1].rstrip(':'))
            runs[(cpu, pc)] += 1
            flags[pc] = int(inner[0], 16)
        elif line.startswith('EE executed'):
            ee = int(line.split()[2])
        elif not line.strip():
            finish()

    def size(pc):
        code = blocks.get(pc, b'')
        if not flags[pc] & 0x00800000:  # ARM state: 4 bytes per instruction
            return len(code) // 4
        i = n = 0  # Thumb-2: 16 or 32-bit encodings
        while i + 1 < len(code):
            hw = code[i] | (code[i + 1] << 8)
            i += 4 if (hw >> 11) in (0x1D, 0x1E, 0x1F) else 2
            n += 1
        return n

    per_cpu, jit = collections.Counter(), collections.Counter()
    for (cpu, pc), n in runs.items():
        per_cpu[cpu] += size(pc) * n
        if not flags[pc] & 0x00800000:
            jit[cpu] += size(pc) * n
    emu = max(jit, key=jit.get)
    return per_cpu[emu], ee


def run(runner, sysroot, elf, frames):
    cmd = ['qemu-arm']
    if sysroot:
        cmd += ['-L', sysroot]
    cmd += ['-d', 'in_asm,exec,nochain', '-D', '/dev/stdout', runner, elf, '--frames', str(frames), '--timeout', '1200', '--bench-exit']
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    arm, ee = count_arm_instructions(proc.stdout)
    proc.wait()
    if ee is None:
        raise SystemExit('no "EE executed" line from %s' % elf)
    return arm, ee


def measure(runner, sysroot):
    results = {}
    with tempfile.TemporaryDirectory() as tmp:
        for key, title, gen in WORKLOADS:
            elf = os.path.join(tmp, key + '.elf')
            args = [sys.executable, os.path.join(TOOLS, gen[0]), elf] + gen[2:]
            subprocess.run(args, check=True)
            arm_a, ee_a = run(runner, sysroot, elf, FRAMES_A)
            arm_b, ee_b = run(runner, sysroot, elf, FRAMES_B)
            gap = (arm_b - arm_a) / max(1, ee_b - ee_a)
            results[key] = gap
            print('%-6s %-28s %.2f ARM instructions per PS2 instruction' % (key, title, gap), flush=True)
    return results


# ---------------------------------------------------------------- chart (SVG)
# Line chart of the gap over time, one series per workload, with the
# one-to-one line. Light and dark come from the viewer's color scheme.
# Palette: reference categorical slots 1-4 (validated light + dark); text uses
# ink tokens, series identity is carried by line, legend and end labels.
SERIES = {'int': 1, 'float': 2, 'mmi': 3, 'mem': 4}


def render_svg(rows, path):
    keys = [k for k, _, _ in WORKLOADS]
    titles = {k: t for k, t, _ in WORKLOADS}
    W, H = 760, 400
    left, right, top, bottom = 56, 150, 116, 40
    pw, ph = W - left - right, H - top - bottom
    values = [float(r[k]) for r in rows for k in keys if r.get(k)]
    ymax = max([1.0] + values)
    step = next(s for s in (0.5, 1, 2, 5, 10, 20, 50) if ymax / s <= 6)
    ymax = step * (int(ymax / step) + 1)
    n = len(rows)

    def x(i):
        return left + (pw * i / (n - 1) if n > 1 else pw / 2)

    def y(v):
        return top + ph - ph * v / ymax

    latest = rows[-1]
    avg = sum(float(latest[k]) for k in keys) / len(keys)
    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" role="img" '
               'aria-labelledby="t d" font-family="-apple-system,BlinkMacSystemFont,Segoe UI,Helvetica,Arial,sans-serif">' % (W, H, W, H))
    out.append('<title id="t">VitaPS2 performance gap</title>')
    out.append('<desc id="d">ARM instructions the emulator needs per PS2 instruction, per workload, over time. '
               'Latest: %s. Lower is better; 1.0 is one-to-one.</desc>' %
               ', '.join('%s %.2f' % (titles[k], float(latest[k])) for k in keys))
    out.append('''<style>
svg{--surface:#fcfcfb;--ink1:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;
--s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a;--s4:#eda100}
@media (prefers-color-scheme: dark){svg{--surface:#1a1a19;--ink1:#f0efec;--ink2:#c3c2b7;--muted:#898781;
--grid:#2c2c2a;--axis:#383835;--s1:#3987e5;--s2:#d95926;--s3:#199e70;--s4:#c98500}}
.bg{fill:var(--surface)} .h{fill:var(--ink1)} .t2{fill:var(--ink2)} .m{fill:var(--muted)}
.grid{stroke:var(--grid);stroke-width:1} .axis{stroke:var(--axis);stroke-width:1}
.ref{stroke:var(--ink2);stroke-width:1.5;stroke-dasharray:4 4}
.line{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
.dot{stroke:var(--surface);stroke-width:2}
</style>''')
    out.append('<rect class="bg" width="%d" height="%d" rx="8"/>' % (W, H))
    # Headline
    out.append('<text class="t2" x="%d" y="30" font-size="13">PS2 → Vita performance gap (lower is better, 1.0 = one-to-one)</text>' % left)
    out.append('<text class="h" x="%d" y="66" font-size="30" font-weight="600">%.2f×</text>' % (left, avg))
    out.append('<text class="t2" x="%d" y="66" font-size="13">ARM instructions per PS2 instruction, average of %d workloads · %s</text>'
               % (left + 98, len(keys), latest.get('label', '')))
    # Grid + y ticks
    v = 0.0
    while v <= ymax + 1e-9:
        cls = 'axis' if v == 0 else 'grid'
        out.append('<line class="%s" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/>' % (cls, left, left + pw, y(v), y(v)))
        out.append('<text class="m" x="%d" y="%.1f" font-size="11" text-anchor="end">%g</text>' % (left - 8, y(v) + 4, v))
        v += step
    # One-to-one reference
    out.append('<line class="ref" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/>' % (left, left + pw, y(1), y(1)))
    out.append('<text class="t2" x="%d" y="%.1f" font-size="11">one-to-one</text>' % (left + pw + 8, y(1) + 4))
    # X labels: first and last date
    out.append('<text class="m" x="%d" y="%d" font-size="11">%s</text>' % (left, H - 20, rows[0]['date']))
    if n > 1:
        out.append('<text class="m" x="%d" y="%d" font-size="11" text-anchor="end">%s</text>' % (left + pw, H - 20, rows[-1]['date']))
    # Series
    ends = []
    for k in keys:
        pts = [(x(i), y(float(r[k]))) for i, r in enumerate(rows) if r.get(k)]
        if not pts:
            continue
        color = 'var(--s%d)' % SERIES[k]
        if len(pts) > 1:
            out.append('<polyline class="line" style="stroke:%s" points="%s"/>' %
                       (color, ' '.join('%.1f,%.1f' % p for p in pts)))
        out.append('<circle class="dot" style="fill:%s" cx="%.1f" cy="%.1f" r="4.5"/>' % (color, pts[-1][0], pts[-1][1]))
        ends.append([pts[-1][1], k, float(rows[-1][k])])
    # End labels with leader lines, spread so they never overlap
    ends.sort()
    gap = 18
    for i in range(1, len(ends)):
        ends[i].append(None)
    placed = []
    for yy, k, val, *_ in ends:
        ly = max(yy, placed[-1] + gap) if placed else yy
        placed.append(ly)
    overflow = placed[-1] - (top + ph) if placed else 0
    if overflow > 0:
        placed = [p - overflow for p in placed]
    for (yy, k, val, *_), ly in zip(ends, placed):
        color = 'var(--s%d)' % SERIES[k]
        lx = left + pw + 14
        out.append('<path d="M%.1f %.1f L%.1f %.1f" style="stroke:%s;stroke-width:1;fill:none"/>' % (left + pw + 5, yy, lx - 3, ly, color))
        out.append('<text class="h" x="%d" y="%.1f" font-size="12" font-weight="600">%.2f</text>' % (lx, ly + 4, val))
        out.append('<text class="t2" x="%d" y="%.1f" font-size="12">%s</text>' % (lx + 34, ly + 4, k))
    # Legend, 2 x 2 (key: short line in the series color, text in ink)
    for i, k in enumerate(keys):
        color = 'var(--s%d)' % SERIES[k]
        lx = left + (i % 2) * 250
        ly = 84 + (i // 2) * 16
        out.append('<line x1="%d" x2="%d" y1="%d" y2="%d" style="stroke:%s;stroke-width:3;stroke-linecap:round"/>' % (lx, lx + 14, ly, ly, color))
        out.append('<text class="t2" x="%d" y="%d" font-size="11">%s – %s</text>' % (lx + 20, ly + 4, k, titles[k]))
    out.append('</svg>')
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--runner', required=True)
    ap.add_argument('--sysroot', default='/usr/arm-linux-gnueabihf')
    ap.add_argument('--history')
    ap.add_argument('--svg')
    ap.add_argument('--label', default='')
    ap.add_argument('--render-only', action='store_true', help='redraw the SVG from the history')
    args = ap.parse_args()
    keys = [k for k, _, _ in WORKLOADS]
    rows = []
    if args.history and os.path.exists(args.history):
        with open(args.history) as f:
            rows = list(csv.DictReader(f))
    if not args.render_only:
        results = measure(args.runner, args.sysroot)
        row = {'date': datetime.date.today().isoformat(), 'label': args.label}
        row.update({k: '%.3f' % results[k] for k in keys})
        rows.append(row)
        if args.history:
            os.makedirs(os.path.dirname(args.history) or '.', exist_ok=True)
            with open(args.history, 'w', newline='') as f:
                w = csv.DictWriter(f, fieldnames=['date', 'label'] + keys)
                w.writeheader()
                w.writerows(rows)
    if args.svg and rows:
        os.makedirs(os.path.dirname(args.svg) or '.', exist_ok=True)
        render_svg(rows, args.svg)


if __name__ == '__main__':
    main()
