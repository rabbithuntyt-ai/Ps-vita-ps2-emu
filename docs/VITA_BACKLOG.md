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

## 1. Back online: done 27 Nov 2026

- After a restart or a new eboot, do one warm-up run before measuring: the JIT cache starts
  empty, and a cold run spends its 120 s compiling (black screen, 0-3 game frames).

## 2. A/B of the changes built while it was offline: done 27 Nov 2026

Results in [PERF_WORK_NOT_DONE.md](PERF_WORK_NOT_DONE.md), "Vita A/B of the nine changes".

- On by default for every game since then (patch 0135 and `GSH_Hardware.h`): `vif_merge`,
  `spu_adma_full_fast`, `feed_neon`, `clut_load_keep_batch`.
- Still off, to be judged on Silent Hill 2 (no effect in RE4): `pump_keep_batch`,
  `texture_lazy_texels`.
- Dropped: `image_no_copy` (no gain); `vif_ring_kb` below 4096 (slower; 256 KB stalled once).

## 3. Silent Hill 2

**First Vita numbers, 27 Nov 2026** (PERF_WORK_NOT_DONE.md, "Silent Hill 2 gameplay on the
Vita"): 1.54 game fps in the restroom. The VU1 worker is the limit (VU1 JIT 391 ms a game
frame). Flat grey untextured polygons appear as the camera moves (likely the "floating wall"),
with no freeze in 6 minutes. Still to do:
- Find the grey polygons: frame dumps with `gs_debug`, then the same scene on the PC runner.
- The user saw, on the Vita: in the opening movie a black frame between every movie picture;
  in gameplay, every third frame or so the picture flickers grey or vanishes, as if loading in
  again. For another time (noted 27 Nov 2026).
- A/B `pump_keep_batch` and `texture_lazy_texels` on SH2 with a saved state (`save_state_at`)
  once a pad script reaches the restroom; texCreate was 62 ms a game frame.
- Then the VU1 JIT (general): SH2's programs cost 391 ms a game frame on the worker.

Earlier plan:

- On the PC runner, gameplay starts at about vblank 2000. The presses are crosses at vblanks 900,
  1200, 1400 and 1600 (New Game, Normal, Normal), and start at 1900 (skips the opening movie).
- On the Vita, `pad_script` times are seconds, and menu speed differs from the PC. So first run
  with `frame_dump_every=20` to find the times, then `save_state_at=` in the restroom.
- Then a `gp.sh`-style script for SH2: load the state, walk (left stick) for 120 s, and read it
  with `perf_audit.py`. Add the result to `docs/COMPATIBILITY.md`, whose SH2 row (2 Nov) has
  only menus and the movie.
- A/B `pump_keep_batch` and `texture_lazy_texels` on SH2 (on the PC: draws -91% with the CLUT
  switch, texture cache entries 44,175 to 224).
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
