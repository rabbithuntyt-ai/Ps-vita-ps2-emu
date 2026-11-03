# Real-world performance on a PS Vita

Silent Hill 2 (USA) on a PS Vita through Vita Lab, measured by
`tools/vita_realworld.sh` from the emulator's perf.csv. Medians per real
second. Full speed: 60 emulated fps; the game shows 30 fps on the title
screen and plays its movies at 30 pictures per second. The synthetic
instruction gap (`gap.svg`) says how much ARM code runs per PS2
instruction; these numbers say what a player sees.

| Date | Build | Title screen fps | Movie: emulated fps | Movie: shown fps | Movie: pictures/s | JIT ms/block | Movie: EE % | VIF % | SPU wait % | GS wait % | VIF1 kQW/s |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2026-11-02 | VIF unpack fast loop (0056) | 10.7 | 14.8 | 14.5 | 0.8 | 4.44 | 31.5 | 11.6 | 0.1 | 6.9 | 91.5 |
| 2026-11-02 | SPU mixer on core 2 |  masked unpack lanes | 10.8 | 11.2 | 10.7 | 0.8 | 4.41 | 26.2 | 12.6 | 0.1 | 9.8 |
| 2026-11-02 | SPU worker priority | 8.2 | 7.0 | 6.3 | 0.7 | 4.38 | 39.4 | 6.6 | 0.1 | 12.8 | 66.0 |
| 2026-11-02 | sprite edge clamp (all textured) |  screen kept on | 8.9 | 8.7 | 7.9 | 0.8 | 4.41 | 32.3 | 24.9 | 21.5 | 0.0 |
| 2026-11-02 | STQ sprite edge clamp |  screen kept on | 2.0 | 7.9 | 7.0 | 0.8 | 4.43 | 36.5 | 22.6 | 19.6 | 0.0 |
| 2026-11-02 | STQ sprite edge clamp |  screen kept on | 8.4 | 8.4 | 8.0 | 0.8 | 4.42 | 30.8 | 23.6 | 21.7 | 0.0 |
| 2026-11-02 | sprite edge clamp + VIF stats | 8.7 | 8.9 | 8.9 | 0.8 | 4.40 | 28.4 | 25.1 | 24.8 | 0.0 | 84.4 |
| 2026-11-02 | sprite edge clamp + VIF stats | 8.9 | - | - | - | 3.22 | - | - | - | - | - |
| 2026-11-02 | VIF waits for VU1 (0052) | 8.8 | 1.9 | 1.8 | 0.8 | 4.41 | 51.7 | 0.9 | 1.0 | 0.0 | 86.8 |
| 2026-11-02 | 17a6ace + VIF1 per-channel stats | 8.8 | 9.9 | 9.9 | 0.8 | 4.43 | 23.8 | 30.2 | 27.1 | 0.0 | - |
| 2026-11-02 | 26a0bfb + local fixes + IPU poll fix | 8.7 | 9.9 | 9.8 | 0.8 | 4.41 | - | - | - | - | - |
| 2026-11-02 | 26a0bfb + local fixes | 8.6 | 12.3 | 7.8 | 0.8 | 4.41 | - | - | - | - | - |
| 2026-11-02 | e47b4fa + local fixes (before cloud merge) | 9.9 | 56.5 | 0.0 | 0.3 | 4.27 | - | - | - | - | - |
