# Resident Evil 4 gameplay on the Vita (gameplay session)

Goal set by the owner: **locked 30 fps in gameplay, playable all the way through**. The movie,
loading and menu work is in [RE4.md](RE4.md) (another session); this file is the gameplay side.

## Working next to the movie session

- Separate git worktree `C:\Users\troy2\vps2-gp` (branch `claude/re4-gameplay`), its own
  submodule checkout and WSL build directories (`/root/build-vita-gp`, `/root/build-host-gp`).
  Play changes of this branch go in `patches/0095-Play-gameplay.patch` (`runs/gp/mkpatch.sh`
  diffs against the base copies in `runs/gp/base`).
- Separate install on the Vita: title `VTPS20001`, data folder `ux0:data/VitaPS2gp` (CMake
  options `VITAPS2_TITLEID`, `VITAPS2_DATA_DIR`, `VITAPS2_TITLE`; defaults unchanged). The game
  image stays in `ux0:data/VitaPS2/games`. Runs never touch the other session's eboot, states,
  perf.csv or JIT cache.
- One Vita: every run holds `/c/Users/troy2/vita_lock.sh` (`vita_lock NAME` / `vita_unlock`)
  from launch to collect. Both sessions' `run.sh` do.
- Scripts (`runs/gp`, not in git): `build.sh` / `hbuild.sh` (WSL), `run.sh NAME SECONDS
  [settings]` (Vita), `st.sh NAME SECONDS [settings]` (from the state in the Vita's
  `VitaPS2gp/states`), `png.sh NAME` (frame dumps to PNG and `sheet.png`), `h.sh` (PC runner).
- The disc image is on the PC too: `C:\Users\troy2\re4iso\re4.iso` (copied from the Vita,
  4.4 GB, about 40 minutes over Vita Lab).

## States

- `runs/gp/state_codec.state`: Hunnigan's radio call right after the first real-time scene.
- `runs/gp/state_b2.state`: **in gameplay**, Leon on the path at the start (HUD on screen).
  Made from the codec state by pressing Cross through the call (`save_state_at=215`).

## Where it stands (2026-11-09, before the fixes below)

| Scene | Emulated fps | Limit |
|---|---|---|
| First real-time scene (Leon at the bridge) | 1.7 | GPU thread: 1.1 s per second in texture binding |
| Hunnigan's call (codec) | ~10 | Emulation thread full: VU1 (VIF section ~50%) |
| Gameplay start | 1-3 | GPU thread (1.1 s/s) and, in other seconds, VU1 (VIF section ~85%) |

Picture problems: Leon's body is not drawn in gameplay (only a hair-shaped blob), patches on his
hair and jacket in the bridge scene, Hunnigan's portrait in the call is a white silhouette.

### A gameplay frame (gs_diag)

- ~1200 draws into the frame buffer 0xE0000 (512x448), 13 read-backs and 12 uploads per frame.
- ~130 small textures per frame streamed through 0x348000-0x3FE000: a 16x16 CLUT and a 4-bit
  128x128 (or smaller) texture each, uploaded right before the draws that use them.
- Post-processing: blur buffers at 0x310000 / 0x348000 / 0x2D8000 drawn 1, 2, 4 and 8 pages wide
  (CT32, and CT16 for 0x310000), the Z buffer (0x1C0000) sampled as PSMZ24, the frame and
  0x310000 sampled as PSMT8H (alpha as a palette index) for the colour grading, and the frame
  sampled while it is drawn (TE0000).

## Changes on this branch

| Change | Setting | Why |
|---|---|---|
| GL textures by content | `content_textures` (on) | Streamed textures come back to the same slots between other textures: keyed by page hash, format, CLUT and TEXA, they are decoded and uploaded once |
| Targets copied into a new width on the GPU | `gpu_relayout` (on) | The blur buffers changed width every frame: read-back plus upload each time |
| Copies of the target being drawn by a draw | `feedback_draw` (on) | `glCopyTexSubImage2D` (sampling the target being drawn) |
| VU1 worker without lost wakeups | (threaded VU1) | The worker waited on a condition variable; on the Vita those lose wakeups (found by the movie session for the IOP thread) |
| No clock read per VU1 program start | | `steady_clock::now()` costs several us on the Vita |

Diagnostics added: `perf.csv` column `gpu_parts` (GPU thread ms per part: read-back pixels,
read-back swizzle, upload swizzle, upload GL, feedback copy, texture lookup, decode, texture
GL, target creation, depth clears, draws, local copies, relayouts, then the count of textures
found by content); `vu1_stats=1` (Vita) / `--vu1-stats` (PC) writes `vu1prog.txt` (VU1
programs by start address: starts, cycles, unfinished, time); `pad_script` takes hold times
(`T:button:ms`), `l2`, `r2`, `lup`/`ldown`/`lleft`/`lright` (left stick) and `a+b`; the PC
runner takes `--load-state` (the Vita's states load there too).
