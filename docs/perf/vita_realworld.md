# Real-world performance on a PS Vita

Silent Hill 2 (USA) on a PS Vita through Vita Lab, measured by
`tools/vita_realworld.sh` from the emulator's perf.csv. Medians per real
second. Full speed: 60 emulated fps; the game shows 30 fps on the title
screen and plays its movies at 30 pictures per second. The synthetic
instruction gap (`gap.svg`) says how much ARM code runs per PS2
instruction; these numbers say what a player sees.

| Date | Build | Title screen fps | Movie: emulated fps | Movie: shown fps | Movie: pictures/s | JIT ms/block |
|---|---|---|---|---|---|---|
| 2026-11-02 | 26a0bfb + local fixes + IPU poll fix | 8.7 | 9.9 | 9.8 | 0.8 | 4.41 |
| 2026-11-02 | 26a0bfb + local fixes | 8.6 | 12.3 | 7.8 | 0.8 | 4.41 |
| 2026-11-02 | e47b4fa + local fixes (before cloud merge) | 9.9 | 56.5 | 0.0 | 0.3 | 4.27 |
