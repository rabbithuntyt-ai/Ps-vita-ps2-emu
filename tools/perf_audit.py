#!/usr/bin/env python3
# tools/perf_audit.py PERF.csv...: game frame rates measured with the rows' own timestamps (t), and the
# emulation thread's / VU1 worker's sampled sections in ms per game frame over the steady window
# (from STEADY_T seconds of the run's clock to the end). Each row's ps2_* value is the share of the
# 1 ms samples in that row's interval; sections are exclusive (one current section per thread).
import csv, sys
STEADY_T = 25.0
GS_NAMES = 'idle other registers vertex primitive addvertex state texture target flush upload download transfer world display worldRet worldSkin feedWrite glDraw glDrawWorld glDrawRet worldSetup worldVerts worldEnd worldClip'.split()
EE_NAMES = "other EE VU0 VU1 IOP SPU VIF GIF IPU SIF JIT GSwait idle SPUsync SPUread VIFdirect VIFunpack EEio EEkernel EEticks IOPio IOPkernel IOPticks IOPcounters IOPbios IOPdma IOPwait IOPdrain IOPlag IOPpark VU1relay VIFqueue VU1native VU1kick relayDirect relayWorld VU1hle VIFcmd VU1jit VIFlock VIFwake VIFcopy VIFunlock VU1sync VU1quiet EEioTimer EEioIntc EEioDma EEioGs EEioWait".split()
for f in sys.argv[1:]:
    rows = list(csv.DictReader(open(f)))
    t = [float(r['t']) for r in rows]
    g = [float(r['game_fps']) for r in rows]
    total = sum(g)
    first = next(i for i, v in enumerate(g) if v > 0)
    span_all = t[-1] - t[first - 1] if first > 0 else t[-1]
    steady = [i for i in range(1, len(rows)) if t[i] >= STEADY_T]
    dt = {i: t[i] - t[i - 1] for i in steady}
    steady_frames = sum(g[i] for i in steady)
    steady_time = sum(dt.values())
    naive = sum(g[i] for i in steady) / len(steady)
    print('%s: %d rows, row interval %.3f s; %d game frames in all; from the first frame: %.2f fps over %.1f s; steady (t >= %.0f s): %.2f fps over %.1f s (per-row average, the old figure: %.2f)' % (
        f.split('/')[-2], len(rows), (t[-1] - t[0]) / (len(rows) - 1), total, total / span_all, span_all, STEADY_T, steady_frames / steady_time, steady_time, naive))
    # ms per game frame, steady window: share x row interval, summed, over the frames
    out = []
    for n in EE_NAMES:
        key = 'ps2_' + n
        if key not in rows[0]:
            continue
        ms = sum(float(rows[i][key]) * dt[i] * 1000 for i in steady) / steady_frames
        if ms >= 2:
            out.append((ms, n))
    out.sort(reverse=True)
    print('  emulation thread, ms per game frame: total %.0f (sum of sections) | %s' % (sum(m for m, _ in out), ', '.join('%s %.0f' % (n, m) for m, n in out)))
    vu = [0.0] * 64
    for i in steady:
        for k, v in enumerate(rows[i]['vu1_sections'].split(';')):
            vu[k] += float(v) * dt[i] * 1000
    parts = sorted(((v / steady_frames, EE_NAMES[k]) for k, v in enumerate(vu) if k < len(EE_NAMES) and v / steady_frames >= 2), reverse=True)
    print('  VU1 worker, ms per game frame: total %.0f | %s' % (sum(m for m, _ in parts), ', '.join('%s %.0f' % (n, m) for m, n in parts)))
    # GS thread: raw sample counts per row (gs_sections), turned into shares of the row's samples
    gs = [0.0] * 32
    samples = 0
    for i in steady:
        v = [float(x) for x in rows[i]['gs_sections'].split(';')]
        total = sum(v) or 1
        samples += total
        for k, c in enumerate(v):
            gs[k] += c / total * dt[i] * 1000
    gparts = sorted(((v / steady_frames, GS_NAMES[k]) for k, v in enumerate(gs) if k < len(GS_NAMES) and v / steady_frames >= 1), reverse=True)
    print('  GS thread, ms per game frame: total %.0f | %s; samples per second %.0f' % (sum(m for m, _ in gparts), ', '.join('%s %.0f' % (n, m) for m, n in gparts), samples / steady_time))
    # gs_wait_split runs: each section's samples taken while the GS thread was ready (runnable, not
    # on a core) or blocked (waiting in the kernel); the rest were running on a core
    if 'gs_blocked_sections' in rows[0] and any(float(x) for i in steady for x in rows[i]['gs_blocked_sections'].split(';') + rows[i]['gs_ready_sections'].split(';')):
        ready = [0.0] * 32
        blocked = [0.0] * 32
        for i in steady:
            total = sum(float(x) for x in rows[i]['gs_sections'].split(';')) or 1
            for k, c in enumerate(rows[i]['gs_ready_sections'].split(';')):
                ready[k] += float(c) / total * dt[i] * 1000
            for k, c in enumerate(rows[i]['gs_blocked_sections'].split(';')):
                blocked[k] += float(c) / total * dt[i] * 1000
        lines = []
        for k, name in enumerate(GS_NAMES):
            t = gs[k] / steady_frames
            if t < 1:
                continue
            r, b = ready[k] / steady_frames, blocked[k] / steady_frames
            lines.append((t, '%s %.0f = run %.1f + ready %.1f + blocked %.1f' % (name, t, t - r - b, r, b)))
        lines.sort(reverse=True)
        print('  GS thread by scheduler state, ms per game frame: ' + '; '.join(l for _, l in lines))
