#!/usr/bin/env python3
"""Puts the latest measured run of each game (docs/perf/compat_runs.csv,
written by tools/vitalab_compat.sh) into docs/COMPATIBILITY.md, between the
<!-- compat-runs-start --> and <!-- compat-runs-end --> markers.

    python3 tools/compat_report.py
"""
import csv
import os

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
RUNS = os.path.join(ROOT, "docs", "perf", "compat_runs.csv")
DOC = os.path.join(ROOT, "docs", "COMPATIBILITY.md")
START, END = "<!-- compat-runs-start -->", "<!-- compat-runs-end -->"


def main():
    latest = {}
    if os.path.exists(RUNS):
        with open(RUNS) as f:
            for row in csv.DictReader(f):
                latest[row["game"]] = row  # later rows win
    lines = [START, ""]
    if latest:
        lines.append("| Game | Serial | Result | First frame (s) | Emulated fps | Shown fps | EE % | VU1 % | GS wait % | JIT ms/block | Date |")
        lines.append("|---|---|---|---|---|---|---|---|---|---|---|")
        for game in sorted(latest):
            r = latest[game]
            name = os.path.splitext(game)[0]
            lines.append("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
                name, r["serial"], r["result"], r["first_draw_s"], r["vm_fps"], r["out_fps"], r["ee_pct"], r["vu1_pct"],
                r["gs_wait_pct"], r["jit_ms_block"], r["date"]))
    else:
        lines.append("No runs yet: see \"Running a compatibility test\".")
    lines += ["", END]
    with open(DOC) as f:
        text = f.read()
    if START in text and END in text:
        before = text[:text.index(START)]
        after = text[text.index(END) + len(END):]
        text = before + "\n".join(lines) + after
    else:
        text = text.rstrip("\n") + "\n\n## Measured runs\n\nLatest run of each game (tools/vitalab_compat.sh).\n\n" + "\n".join(lines) + "\n"
    with open(DOC, "w") as f:
        f.write(text)
    print("%d games in %s" % (len(latest), os.path.relpath(DOC, ROOT)))


if __name__ == "__main__":
    main()
