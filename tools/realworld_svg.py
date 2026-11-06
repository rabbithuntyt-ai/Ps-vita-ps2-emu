#!/usr/bin/env python3
# Draws docs/perf/vita_realworld.svg from docs/perf/vita_realworld.csv: frame
# rates measured on a real PS Vita (tools/vita_realworld.sh), one point per
# build, against the 30 fps the game shows at full speed. Same look as
# tools/perf_gap.py's chart (light and dark from the viewer's color scheme).
#
# Usage: realworld_svg.py [docs/perf/vita_realworld.csv] [docs/perf/vita_realworld.svg] [game name]

import csv
import sys

SERIES = [
    ('title_out_fps', 'Title screen', 1),
    ('movie_out_fps', 'Opening movie', 2),
]
TARGET = 30.0
GAME = sys.argv[3] if len(sys.argv) > 3 else 'Silent Hill 2'


def number(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def render(rows, path):
    W, H = 760, 400
    left, right, top, bottom = 56, 170, 116, 40
    pw, ph = W - left - right, H - top - bottom
    values = [number(r.get(k)) for r in rows for k, _, _ in SERIES]
    values = [v for v in values if v is not None]
    ymax = max([TARGET] + values)
    step = next(s for s in (5, 10, 20) if ymax / s <= 7)
    ymax = step * (int(ymax / step) + 1)
    n = len(rows)

    def x(i):
        return left + (pw * i / (n - 1) if n > 1 else pw / 2)

    def y(v):
        return top + ph - ph * v / ymax

    latest = rows[-1]
    best_title = max(v for v in (number(r.get('title_out_fps')) for r in rows) if v is not None)
    out = []
    out.append('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" role="img" '
               'aria-labelledby="t d" font-family="-apple-system,BlinkMacSystemFont,Segoe UI,Helvetica,Arial,sans-serif">' % (W, H, W, H))
    out.append('<title id="t">VitaPS2 on PS Vita hardware: ' + GAME + ' frame rates</title>')
    out.append('<desc id="d">Frame rates measured on a PS Vita for each build. Latest (%s): %s. Full speed shows 30 fps.</desc>' %
               (latest['label'], ', '.join('%s %s fps' % (t, latest.get(k, '-')) for k, t, _ in SERIES)))
    out.append('''<style>
svg{--surface:#fcfcfb;--ink1:#0b0b0b;--ink2:#52514e;--muted:#898781;--grid:#e1e0d9;--axis:#c3c2b7;
--s1:#2a78d6;--s2:#eb6834;--s3:#1baf7a}
@media (prefers-color-scheme: dark){svg{--surface:#1a1a19;--ink1:#f0efec;--ink2:#c3c2b7;--muted:#898781;
--grid:#2c2c2a;--axis:#383835;--s1:#3987e5;--s2:#d95926;--s3:#199e70}}
.bg{fill:var(--surface)} .h{fill:var(--ink1)} .t2{fill:var(--ink2)} .m{fill:var(--muted)}
.grid{stroke:var(--grid);stroke-width:1} .axis{stroke:var(--axis);stroke-width:1}
.ref{stroke:var(--ink2);stroke-width:1.5;stroke-dasharray:4 4}
.line{fill:none;stroke-width:2;stroke-linejoin:round;stroke-linecap:round}
.dot{stroke:var(--surface);stroke-width:2}
</style>''')
    out.append('<rect class="bg" width="%d" height="%d" rx="8"/>' % (W, H))
    out.append('<text class="t2" x="%d" y="30" font-size="13">%s on a real PS Vita (higher is better, 30 fps = full speed)</text>' % (left, GAME))
    out.append('<text class="h" x="%d" y="66" font-size="30" font-weight="600">%.1f fps</text>' % (left, best_title))
    out.append('<text class="t2" x="%d" y="66" font-size="13">best title screen so far · %d measured builds</text>' % (left + 128, n))
    v = 0.0
    while v <= ymax + 1e-9:
        cls = 'axis' if v == 0 else 'grid'
        out.append('<line class="%s" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/>' % (cls, left, left + pw, y(v), y(v)))
        out.append('<text class="m" x="%d" y="%.1f" font-size="11" text-anchor="end">%g</text>' % (left - 8, y(v) + 4, v))
        v += step
    out.append('<line class="ref" x1="%d" x2="%d" y1="%.1f" y2="%.1f"/>' % (left, left + pw, y(TARGET), y(TARGET)))
    out.append('<text class="t2" x="%d" y="%.1f" font-size="11">full speed</text>' % (left + pw + 8, y(TARGET) + 4))
    out.append('<text class="m" x="%d" y="%d" font-size="11">%s</text>' % (left, H - 20, rows[0]['date']))
    if n > 1:
        out.append('<text class="m" x="%d" y="%d" font-size="11" text-anchor="end">%s</text>' % (left + pw, H - 20, rows[-1]['date']))
    ends = []
    for k, title, slot in SERIES:
        pts = [(x(i), y(number(r.get(k)))) for i, r in enumerate(rows) if number(r.get(k)) is not None]
        if not pts:
            continue
        color = 'var(--s%d)' % slot
        if len(pts) > 1:
            out.append('<polyline class="line" style="stroke:%s" points="%s"/>' % (color, ' '.join('%.1f,%.1f' % p for p in pts)))
        out.append('<circle class="dot" style="fill:%s" cx="%.1f" cy="%.1f" r="4.5"/>' % (color, pts[-1][0], pts[-1][1]))
        last = [number(r.get(k)) for r in rows if number(r.get(k)) is not None][-1]
        ends.append([pts[-1][1], slot, last])
    # End labels, spread so they never overlap
    ends.sort()
    placed = []
    for yy, _, _ in ends:
        placed.append(max(yy, placed[-1] + 18) if placed else yy)
    overflow = placed[-1] - (top + ph) if placed else 0
    if overflow > 0:
        placed = [p - overflow for p in placed]
    for (yy, slot, val), ly in zip(ends, placed):
        color = 'var(--s%d)' % slot
        lx = left + pw + 14
        out.append('<path d="M%.1f %.1f L%.1f %.1f" style="stroke:%s;stroke-width:1;fill:none"/>' % (left + pw + 5, yy, lx - 3, ly, color))
        out.append('<text class="h" x="%d" y="%.1f" font-size="12" font-weight="600">%.1f fps</text>' % (lx, ly + 4, val))
    for i, (k, title, slot) in enumerate(SERIES):
        color = 'var(--s%d)' % slot
        lx = left + i * 200
        out.append('<line x1="%d" x2="%d" y1="90" y2="90" style="stroke:%s;stroke-width:3;stroke-linecap:round"/>' % (lx, lx + 14, color))
        out.append('<text class="t2" x="%d" y="94" font-size="11">%s</text>' % (lx + 20, title))
    out.append('</svg>')
    with open(path, 'w') as f:
        f.write('\n'.join(out) + '\n')


def main():
    source = sys.argv[1] if len(sys.argv) > 1 else 'docs/perf/vita_realworld.csv'
    target = sys.argv[2] if len(sys.argv) > 2 else 'docs/perf/vita_realworld.svg'
    with open(source) as f:
        rows = list(csv.DictReader(f))
    if rows:
        render(rows, target)


if __name__ == '__main__':
    main()
