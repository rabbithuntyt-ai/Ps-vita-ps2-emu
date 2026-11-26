# Work waiting for the Vita (written 26 Nov 2026, while it was offline)

Everything below needs the real hardware. In order: first make sure the Vita and the build are
sound, then measure what was built blind, then the new work. Background and numbers:
[RE4_30FPS_BRIEF.md](RE4_30FPS_BRIEF.md), [PERF_WORK_NOT_DONE.md](PERF_WORK_NOT_DONE.md).

**Rules that apply throughout**
- **General purpose first (decided 26 Nov 2026).** VitaPS2 is a general-purpose emulator, so
  each improvement must help most games. Game-specific work (RE4-only paths, per-game hacks) is
  limited from now on. If one seems genuinely necessary, say so and why, and get the user's OK
  before building it. RE4 stays the game we measure with; check changes on a second game too
  (Silent Hill 2 runs on the PC runner). A general switch that holds up becomes a global
  default, not a game profile key.
- Take the lock for manual commands: `source /c/Users/troy2/vita_lock.sh`, then `vita_lock NAME`
  (`gp.sh` takes it itself).
- Never run `vitalab clock`, and never go above 444 MHz.
- **`world_scratch` stays off.** It hung the Vita on 26 Nov, which is when it went offline.
- Build with `runs/re4/vb.sh NAME`. Push the eboot on every run (`EBOOT=...`), and check that
  perf.csv shows the build's own column. Another session's eboot was measured by mistake once.
- One build varies about ±5% run to run, so always interleave off/on runs: one warm-up, then
  pairs of 120 s runs with `timeout 480`. Read the results with `tools/perf_audit.py RUN/perf.csv`
  (steady game fps from 25 s, ms per game frame by section).

## 1. Back online

1. `bash runs/re4/vitacheck.sh` and `vitalab find` (in `/c/Users/troy2/Downloads`).
2. Build the current branch (patches through 0134): `runs/re4/vb.sh base`.
3. Sanity run: one `gp.sh` run with every new switch off. It must finish without a crash dump,
   at about 4.2 steady game fps (the last result, R55c: 4.17). Look at a few frame dumps
   (`DUMP=20`).

## 2. A/B the changes built while it was offline (all off by default, all exact on the PC)

| Switch | Patch | What it should move | Also check |
|---|---|---|---|
| `feed_neon=1` | 0130 | GS thread `feedWrite` (16 ms a game frame): PSMCT32 transfer blocks written by columns with NEON | Frame dumps right (only the Vita runs the NEON path) |
| `vif_ring_kb=256`, `512` against `4096` | 0131 | Emulation thread `VIFcopy` (20 ms) and the worker's `VIFunpack`: a ring that fits the L2 | `ringWaits` in perf.csv; VU1 wait not worse |
| `image_no_copy=1` | 0132 | DIRECT image data to the GS by reference: worker `VIFdirect` (19 ms), the relay | Frame dumps right (texture uploads) |
| `vif_merge=1` | 0133 | Emulation thread `VIFqueue` (13 ms) and `VIFwake`; worker `VIFcmd` (13 ms). Ring records -41% (RE4), -45% (SH2) | - |
| `spu_adma_full_fast=1` | 0134 | Emulation thread `SPUread` (~7 ms, all waiting). Paused SPU DMAs -92% (RE4), -95% (SH2) | `spu_wait_ms` and `spu_read_waits` in perf.csv; listen for audio glitches |

- Turn on as the global default whatever is exact and cuts its section time, even if game fps
  stays within the spread. All five are general mechanisms; use a game profile only for a
  measured reason. Commit with
  `runs/re4/commit.sh`, and put the numbers in the brief and in PERF_WORK_NOT_DONE.md.
- `vif_merge` and `spu_adma_full_fast` are general (both checked on Silent Hill 2). If they hold
  up, make them the global default, not RE4-only.
- After that: one combined run with everything kept, as the new baseline.

## 3. Silent Hill 2: a gameplay baseline (never measured on the Vita)

- On the PC runner, gameplay starts at about vblank 2000. The presses are crosses at vblanks 900,
  1200, 1400 and 1600 (New Game, Normal, Normal), and start at 1900 (skips the opening movie).
- On the Vita, `pad_script` times are seconds, and menu speed differs from the PC. So first run
  with `frame_dump_every=20` to find the times, then `save_state_at=` in the restroom.
- Then a `gp.sh`-style script for SH2: load the state, walk (left stick) for 120 s, and read it
  with `perf_audit.py`. Add the result to `docs/COMPATIBILITY.md`, whose SH2 row (2 Nov) has
  only menus and the movie.
- Repeat the A/Bs of section 2 for `vif_merge` and `spu_adma_full_fast` on SH2.
- **Check the old gameplay freeze first.** The user saw SH2 gameplay on the Vita freeze on a
  picture of a single floating wall (date not recorded). It is not documented anywhere else.
  - On the PC runner, with SH2's profile settings (`threaded_vu1=1`, threaded SPU, no
    `vu1_async`), James leaves the restroom without a freeze.
  - The route on the PC: walk to the far end of the restroom, turn left, walk forward. It is
    outside from about vblank 4300, and every shot through 5300 changes.
  - The PC does not run the ARM JIT, vitaGL or the Vita's memory limits, so this says nothing
    about the Vita.
  - Fixed since early November, and possible causes: the threaded VU1 hang (queued XGKICKs
    entering the GIF mid-packet, fixed 10 Nov), and vitaGL texture allocation failures from the
    old render target pool (`target_pool_lru`, 25 Nov).
  - If it still freezes: note where, then try `threaded_vu1=0` to see if it is a VU1 or GS
    path problem.
- Also seen on the PC: in one restroom run, a wall panel was drawn with garbled texture for a
  few shots. Compare with Vita frame dumps.

## 4. GS thread toward 33 ms a game frame (biggest section first)

R55 sections, ms per game frame: feedWrite 16-17, download 14, worldRet 13-14, glDrawWorld 10,
registers 8-9, worldVerts 8, flush 7-8, other 7, worldClip 7, skinLoop 7, texHash 6. Each change
gets a PC exactness check (screenshots identical), then a Vita A/B, then a commit.

- **Stale read-backs (correctness first):** vitaGL's glReadPixels waits for the GPU only when the
  target read is the one drawn last, so the Vita can read stale pixels. `readback_finish` fixes it
  but costs 13 to 55 ms. Try finishing only the target's own scene, or a fence on that target.
- `feedWrite`: after `feed_neon` is measured, look at PSMT4 and the other formats by `--feed-shapes`.
- `download` (read-backs, 14 ms): the GXM transfer engine with a fence, instead of glReadPixels
  (the VitaSX2 idea). It would also fix the stale read-backs.
- `worldRet` / `glDrawWorld`: fewer retained draws (merge more batches per draw). **RE4-only**
  (its native world path): ask before working on it.
- General GS work comes first, because every game goes through it: the transfer writes, the
  read-backs and uploads, the register and vertex path, texture hashing, and the cost per GL draw.

## 5. Emulation thread (the limit: ~229 ms a game frame, R55c)

R55c, ms per game frame: EE 94, VU1 wait 34, VIFcopy 20, relayDirect 16, VIFqueue 13, IOP 10,
SPUread 7, IOPkernel 7.
- **EE JIT work.** It needs the Vita: the ARM JIT cannot be tested off it, because qemu-arm fails
  in WSL. Candidates from the brief: guest registers kept across linked blocks, compare-to-branch
  flag folding, loop memory preflight, in-block fill and copy loops. Each step is below the noise,
  so judge them together.
- EE wait-loop fast-forward was measured and dropped for RE4 (0.8% of EE time). It may still be
  worth it for other games; measure first, with the same method (PERF_WORK_NOT_DONE.md, "EE wait
  loops in RE4").
- Native timer interrupt (libtimer TIMER1 handler, ~6% of EE samples, ~5 ms a game frame).

## 6. Housekeeping

- Results go into the brief's tables and PERF_WORK_NOT_DONE.md. Leave a hand-over note in the
  session notes folder when a session ends.
- Kernel plugins: only well-known third-party ones (for example kubridge), installed by the
  user, and only proposed with a measured reason.
