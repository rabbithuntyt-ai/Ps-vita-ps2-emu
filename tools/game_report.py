#!/usr/bin/env python3
"""One report for a game run on the Vita (tools/vita_game_test.sh collects the files).

Usage: game_report.py RUN_DIR [--goal-movie 30] [--goal-menu 60]

Reads what the run left in RUN_DIR (perf.csv, mpeghle.txt, eehot.txt, iophot.txt,
syscalls.txt, eeram.bin, iopram.bin, f_*.ppm, crash.txt, crashes_new.txt) and writes
RUN_DIR/report.md and RUN_DIR/sheet.png. Python 3 standard library only.

What it answers, in order:
  - did it crash, and where it stopped drawing
  - load phases: first picture on screen, first full-speed second, first movie picture
  - per phase: emulated fps, displayed fps, thread loads, waits, JIT compiling
  - movies: pictures per second and the worst 5 s window, against the goal
    (a movie shows one picture per 2 vblanks: a locked 30 needs 60 emulated fps)
  - where the time goes: EE and IOP hotspots (named through a symbol table in EE
    RAM when the game ships one), kernel calls, returns to the dispatcher
"""
import bisect, collections, csv, glob, os, re, struct, sys, zlib


def read_perf(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return []
    with open(path) as f:
        rows = list(csv.DictReader(f))
    out = []
    for r in rows:
        try:
            r['_t'] = float(r['t'])
        except (KeyError, ValueError):
            continue
        out.append(r)
    return out


def num(row, key, default=0.0):
    try:
        return float(row.get(key, default) or default)
    except ValueError:
        return default


def threads(row):
    """'Audio=1.4;PS2 (EE/IOP/VU)=90;...' -> {name: percent}"""
    result = {}
    for part in (row.get('threads') or '').split(';'):
        if '=' in part:
            name, value = part.rsplit('=', 1)
            try:
                result[name] = float(value)
            except ValueError:
                pass
    return result


def phases(rows):
    """First drawn second, first full-speed second (40+ fps shown), first movie picture."""
    first_out = next((r['_t'] for r in rows if num(r, 'out_fps') > 0), None)
    full = next((r['_t'] for r in rows if num(r, 'out_fps') > 40), None)
    movie = next((r['_t'] for r in rows if num(r, 'mpeg_pictures') > 0), None)
    return first_out, full, movie


def average(rows, keys):
    if not rows:
        return {k: 0.0 for k in keys}
    return {k: sum(num(r, k) for r in rows) / len(rows) for k in keys}


def movie_stats(rows):
    """Pictures per second over the movie (mpeg_pictures is a running total), and the worst 5 s."""
    pts = [(r['_t'], num(r, 'mpeg_pictures')) for r in rows if num(r, 'mpeg_pictures') > 0]
    if len(pts) < 8:
        return None
    pts = pts[3:]  # skip the start of the movie
    rate = (pts[-1][1] - pts[0][1]) / max(1e-6, pts[-1][0] - pts[0][0])
    worst = None
    j = 0
    for i in range(len(pts)):
        while j < len(pts) and pts[j][0] - pts[i][0] < 5.0:
            j += 1
        if j >= len(pts):
            break
        window = (pts[j][1] - pts[i][1]) / max(1e-6, pts[j][0] - pts[i][0])
        worst = window if worst is None else min(worst, window)
    vm = sum(num(r, 'vm_fps') for r in rows if num(r, 'mpeg_pictures') > 0) / max(1, len([r for r in rows if num(r, 'mpeg_pictures') > 0]))
    return {'rate': rate, 'worst5s': worst if worst is not None else rate, 'pictures': pts[-1][1], 'seconds': pts[-1][0] - pts[0][0], 'vm': vm}


def symbols_from_ram(path):
    """(address, name) pairs from an in-RAM symbol table of (name pointer, address, hash)
    entries, as Resident Evil 4 ships one. Empty when there is none."""
    if not os.path.exists(path) or os.path.getsize(path) < 0x400000:
        return []
    ram = open(path, 'rb').read()
    words = struct.unpack_from('<%dI' % (len(ram) // 4), ram)
    ident = re.compile(rb'^[A-Za-z_][A-Za-z0-9_]{2,}$')
    best = []
    # Scan 64 KB windows for runs of (name pointer, code address) pairs.
    for start in range(0x100000 // 4, min(len(words), 0x1000000 // 4) - 3, 3):
        n, a = words[start], words[start + 1]
        if not (0x100000 <= a < 0x1000000 and 0x100000 <= n < 0x2000000):
            continue
        name = ram[n:n + 64].split(b'\0')[0]
        if not ident.match(name):
            continue
        best.append((a, name.decode('latin1')))
    if len(best) < 200:
        return []
    return sorted(set(best))


def name_of(syms, keys, address):
    if not syms:
        return '%08X' % address
    i = bisect.bisect_right(keys, address) - 1
    if i >= 0 and address - syms[i][0] < 0x4000:
        return '%s+%X' % (syms[i][1], address - syms[i][0])
    return '%08X' % address


def hotspots(path, syms, count=15):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        return None, []
    lines = open(path).read().splitlines()
    header = lines[0] if lines else ''
    keys = [a for a, _ in syms]
    by_function = collections.Counter()
    for line in lines[1:]:
        p = line.split()
        if len(p) < 2:
            continue
        address, share = int(p[0], 16), float(p[1].rstrip('%'))
        label = name_of(syms, keys, address)
        by_function[label.split('+')[0] if syms else label] += share
    return header, by_function.most_common(count)


def iop_modules(path):
    """IOP module names (Sony's import tables, 0x41E00000), for reading iophot.txt."""
    if not os.path.exists(path):
        return []
    ram = open(path, 'rb').read()
    result = []
    for a in range(0, len(ram) - 24, 4):
        if struct.unpack_from('<I', ram, a)[0] == 0x41E00000 and struct.unpack_from('<I', ram, a + 4)[0] == 0:
            n = ram[a + 12:a + 20].split(b'\0')[0]
            if n and all(32 < c < 127 for c in n):
                result.append((a, n.decode()))
    return result


def ppm_to_rgb(path):
    data = open(path, 'rb').read()
    parts = data.split(b'\n', 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3][:w * h * 3]


def contact_sheet(frames, out, columns=4, scale=4):
    """Frames downscaled by 'scale', 'columns' per row, as a PNG."""
    images = []
    for f in frames:
        try:
            images.append(ppm_to_rgb(f))
        except Exception:
            pass
    if not images:
        return False
    tw = max(w for w, _, _ in images) // scale
    th = max(h for _, h, _ in images) // scale
    rows_n = (len(images) + columns - 1) // columns
    W, H = tw * columns, th * rows_n
    canvas = bytearray(W * H * 3)
    for k, (w, h, px) in enumerate(images):
        ox, oy = (k % columns) * tw, (k // columns) * th
        for y in range(min(th, h // scale)):
            src = (y * scale) * w * 3
            for x in range(min(tw, w // scale)):
                s = src + x * scale * 3
                d = ((oy + y) * W + ox + x) * 3
                canvas[d:d + 3] = px[s:s + 3]
    raw = b''.join(b'\0' + bytes(canvas[y * W * 3:(y + 1) * W * 3]) for y in range(H))

    def chunk(t, c):
        return struct.pack('>I', len(c)) + t + c + struct.pack('>I', zlib.crc32(t + c) & 0xffffffff)
    with open(out, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    return True


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if not args:
        raise SystemExit(__doc__)
    run = args[0]
    goal_movie = float(sys.argv[sys.argv.index('--goal-movie') + 1]) if '--goal-movie' in sys.argv else 30.0
    goal_menu = float(sys.argv[sys.argv.index('--goal-menu') + 1]) if '--goal-menu' in sys.argv else 60.0
    rows = read_perf(os.path.join(run, 'perf.csv'))
    md = ['# Game report: %s' % os.path.basename(os.path.abspath(run)), '']
    auto = os.path.join(run, 'autostart.txt')
    if os.path.exists(auto):
        md += ['Settings: `%s`' % ' '.join(l.strip() for l in open(auto) if l.strip() and not l.startswith('pad_script')), '']

    # Crash
    crash_lines = []
    for name in ('crash.txt', 'crashes_new.txt'):
        p = os.path.join(run, name)
        if os.path.exists(p) and os.path.getsize(p) > 0:
            crash_lines.append('%s: %s' % (name, open(p, errors='replace').read().strip()[:300]))
    if not rows:
        md += ['**No perf.csv: the emulator did not run or the files could not be fetched.**', '']
    md += ['## Verdict', '']
    verdict = []
    if crash_lines:
        verdict.append('**Crashed.** ' + ' | '.join(crash_lines))
    first_out, full, movie = phases(rows)
    last_t = rows[-1]['_t'] if rows else 0
    if rows and first_out is None:
        verdict.append('**Never drew a picture** (`out_fps` stayed 0).')
    ms = movie_stats(rows)
    if ms:
        ok = ms['worst5s'] >= goal_movie - 0.5
        verdict.append('Movie: %.1f pictures/s on average, worst 5 s %.1f, at %.1f emulated fps: %s (goal: locked %.0f)' % (
            ms['rate'], ms['worst5s'], ms['vm'], 'meets the goal' if ok else '**below the goal**', goal_movie))
    menu_rows = [r for r in rows if full is not None and r['_t'] >= full and num(r, 'mpeg_pictures') == 0 and num(r, 'out_fps') > 0]
    if menu_rows:
        vm = sorted(num(r, 'vm_fps') for r in menu_rows)[len(menu_rows) // 2]
        verdict.append('Menus/2D (no movie, drawing): median %.0f emulated fps: %s (goal %.0f)' % (vm, 'meets the goal' if vm >= goal_menu - 2 else '**below the goal**', goal_menu))
    if not verdict:
        verdict.append('Nothing measured.')
    md += ['- ' + v for v in verdict] + ['']

    # Phases
    md += ['## Load phases', '']
    md += ['| Phase | Second |', '|---|---|']
    md += ['| First picture on screen | %s |' % ('%.1f' % first_out if first_out is not None else '-'),
           '| First second over 40 fps shown | %s |' % ('%.1f' % full if full is not None else '-'),
           '| First movie picture | %s |' % ('%.1f' % movie if movie is not None else '-'),
           '| End of the run | %.1f |' % last_t, '']
    compile_s = sum(num(r, 'ps2_JIT') for r in rows)
    md += ['JIT compiling: %.1f s of the emulation thread over the run (a second run after a build compiles almost nothing).' % compile_s, '']

    # Per phase averages
    def describe(label, sel):
        if not sel:
            return None
        a = average(sel, ['vm_fps', 'out_fps', 'ps2_EE', 'ps2_IOP', 'ps2_other', 'ps2_JIT', 'ps2_GSwait', 'spu_wait_ms', 'mpeg_wait_ms', 'mpeg_decode_ms', 'disc_ms', 'limiter_ms', 'dispatch_ee', 'dispatch_iop'])
        t = collections.Counter()
        for r in sel:
            t.update(threads(r))
        tn = ', '.join('%s %.0f%%' % (k, v / len(sel)) for k, v in t.most_common(5))
        return '| %s | %d | %.1f | %.1f | %.0f/%.0f/%.0f/%.0f%% | %.0f/%.0f/%.0f | %.0f | %s | %s |' % (
            label, len(sel), a['vm_fps'], a['out_fps'], a['ps2_EE'] * 100, a['ps2_IOP'] * 100, a['ps2_other'] * 100, a['ps2_JIT'] * 100,
            a['spu_wait_ms'], a['mpeg_wait_ms'], a['disc_ms'], a['mpeg_decode_ms'], ('%.0fk' % (a['dispatch_ee'] / 1000)) if a['dispatch_ee'] else '-', tn)
    md += ['## Where the time goes, per phase', '',
           '| Phase | s | vm fps | shown fps | EE/IOP/other/JIT of the emu thread | waits ms/s: SPU/MPEG/disc | decoder ms/s | EE dispatches/s | threads (CPU %) |',
           '|---|---|---|---|---|---|---|---|---|']
    loading = [r for r in rows if first_out is not None and r['_t'] >= first_out and (full is None or r['_t'] < full)]
    moviesel = [r for r in rows if num(r, 'mpeg_pictures') > 0]
    for line in (describe('Boot (first picture to full speed)', loading), describe('Menus / 2D', menu_rows), describe('Movie', moviesel)):
        if line:
            md.append(line)
    md += ['', 'Reading it: the emulation thread near 90% or more is CPU bound (look at the hotspots); well under 90% with low fps means it waits (the wait columns). A movie at N emulated fps shows N/2 pictures per second.', '']

    # Hotspots
    syms = symbols_from_ram(os.path.join(run, 'eeram.bin'))
    header, ee = hotspots(os.path.join(run, 'eehot.txt'), syms)
    if ee:
        md += ['## EE hotspots (%s%s)' % (header, ', named from the symbol table in EE RAM' if syms else ''), '']
        md += ['| Share | Code |', '|---|---|'] + ['| %.1f%% | `%s` |' % (v, k) for k, v in ee] + ['']
    mods = iop_modules(os.path.join(run, 'iopram.bin'))
    header, iop = hotspots(os.path.join(run, 'iophot.txt'), [])
    if iop:
        keys = [a for a, _ in mods]
        md += ['## IOP hotspots (%s)' % header, '', '| Share | Address | Near (approximate: the next import table in IOP RAM) |', '|---|---|---|']
        for k, v in iop:
            a = int(k, 16)
            i = bisect.bisect_left(keys, a)
            md.append('| %.1f%% | %s | %s |' % (v, k, mods[i][1] if i < len(mods) else '-'))
        md.append('')
    sc = os.path.join(run, 'syscalls.txt')
    if os.path.exists(sc) and os.path.getsize(sc) > 0:
        calls = sorted(((int(l.split()[1]), int(l.split()[0])) for l in open(sc) if len(l.split()) == 2), reverse=True)[:8]
        md += ['## Kernel calls (count by number, since the hotspots started)', '', ', '.join('0x%02X: %d' % (n, c) for c, n in calls), '']

    # Frames
    frames = sorted(glob.glob(os.path.join(run, 'f_*.ppm')))
    if frames and contact_sheet(frames, os.path.join(run, 'sheet.png')):
        md += ['## Frames', '', 'Frame dumps every few seconds, in order: `sheet.png`. Check them: fps counters have been wrong while a picture was stuck.', '']

    with open(os.path.join(run, 'report.md'), 'w') as f:
        f.write('\n'.join(md) + '\n')
    print('\n'.join(md[:20]))
    print('... written to %s' % os.path.join(run, 'report.md'))


if __name__ == '__main__':
    main()
