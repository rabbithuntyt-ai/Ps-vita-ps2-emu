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

**Trap: the Vita can fall asleep anyway** (during the 40 minute ISO copy it went to the lock
screen). Then `vitalab launch` "succeeds" but nothing starts, `vitalab shot` returns the last
picture before sleep (same clock in the status bar) and `stats` shows CPU 333 MHz / SHELL. A run
that leaves `autostart.txt` unread and an empty `perf.csv` is this. Fix: `vitalab press ps`,
`vitalab swipe 900 80 400 300 400` (unlock), `vitalab cmd "nosleep on"`.

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

### VU1 is the CPU wall (PC measurements, `--vu1-stats`)

Gameplay start, per vblank: about 4.3 M VU1 cycles (88% of a real VU1's 4.9 M). Two programs
are 87% of it:
- continued at 0x1408 (`B 0x1040`, ends with XGKICK and E at 0x13F0): the world's vertex
  loop. Per vertex (0x12B8-0x13E8, ~39 cycles): position and texcoords from integers (ITOF),
  two 4x4 transforms (MULA/MADDA), CLIP against w, DIV for Q, colour scale and clamp, a
  cross product for back faces, FTOI4 out, ADC from the clip flags. Clipping (FCOR on the
  flags) branches to 0x14B0 / 0x1E88 / 0x14D8 / 0x1F78. About 790 runs per vblank, ~92
  vertices each: ~70k vertices per vblank (the leaf piles at the start).
- continued at 0x0498 (`B 0x01D0`): skinning (weighted bone matrices, lighting): Leon,
  enemies.
On the PC's x86 JIT this is ~7 ms per vblank. A generic VU JIT on a 444 MHz ARM core cannot do
4.3 M VU cycles in 16.7 ms; the options are threaded VU1 (one core), a faster VU JIT, and
native versions of these two loops.

**On the Vita (2026-11-10):** the vertex program takes ~296 us per run (6.3 us on the PC):
36-55 ARM cycles per VU cycle, ~350 ms of VU1 work per vblank in this scene. Real game frame
rate (`game_fps`, below) is about **1-3 per second** in gameplay, inline or threaded.
`tools/vu_arm_dump` (VU1 block of a micro memory dump, compiled by the Vita's code
generator) shows why: the 38-pair vertex loop is 1457 ARM instructions per iteration, of which
~36 are vector arithmetic. The rest: every VF register loaded from and stored to the context
around each operation (`add ip, fp, #off` + `vld1`/`vst1`: the 128-bit VF symbols are
"aliased" in the Jitter's allocator because lane accesses and the indexed flag pipelines alias
them, and only 8 NEON registers are allocatable), MAC flag pipeline bookkeeping (arrays indexed
by pipeline position), Q pipeline selects, ~110 instructions of FMAC stall checks re-run at
every iteration of a self-looping block, and float clamping (`vu_no_clamp=1` removes it: only
4% of the code).

**The budget:** RE4 needs ~258 M VU1 cycles per second here. One dedicated 444 MHz core would
need under 2 ARM cycles per VU cycle; even a near-ideal recompiler (4-6) would not reach full
speed in this scene. A locked 30 needs RE4's vertex microprograms run as GPU vertex shaders
(the GPU transforms, clips and culls; the CPU only moves the unpacked vertex data), with the
generic VU JIT made several times faster for the rest. The microprograms are uploaded per
object type (MPG), so they must be recognised by content, not address.

**Threaded VU1 and emulated time:** with the thread, the EE ran on in emulated time while VU1
worked, so the game saw its frames take far longer: 77 game frames in 900 vblanks (393
inline) on the PC; on the Vita "30 emulated fps" were ~5 game frames per second.
`vu1_wait_on_stall` (on): when VIF1 waits for the program (VEW), the EE waits too, in real
time: 263 game frames in 900 vblanks on the PC. Count game frames, not vblanks: `game_fps`
in perf.csv (flips showing a new picture), "game frames" in the PC runner's `--hw` output.

PC with the software renderer and the GPU renderer (Mesa): the gameplay picture is right
(Leon, gun, HUD). So Leon's missing body on the Vita is a Vita GPU issue (depth lost at GPU
scene breaks is the main suspect: `depth_force_load`).

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
| **Threaded VU1: PATH1 packets wait while another GIF path is mid-packet** | (threaded VU1) | The hang at the first real-time scene. Queued XGKICK packets were sent on every EE slice, also in the middle of a VIF1 DIRECT (PATH2) or GIF DMA (PATH3) packet split over transfers: the GIF took them as the rest of that packet and stayed on PATH1, the next DIRECT never went through. Reproduced on the PC (`--mtvu`, gameplay state: 12 of 1200 frames shown); fixed: 1063 of 1200, picture correct, Leon walks |
| No clock read per VU1 program start | | `steady_clock::now()` costs several us on the Vita |

Diagnostics added: `perf.csv` column `gpu_parts` (GPU thread ms per part: read-back pixels,
read-back swizzle, upload swizzle, upload GL, feedback copy, texture lookup, decode, texture
GL, target creation, depth clears, draws, local copies, relayouts, then the count of textures
found by content); `vu1_stats=1` (Vita) / `--vu1-stats` (PC) writes `vu1prog.txt` (VU1
programs by start address: starts, cycles, unfinished, time); `pad_script` takes hold times
(`T:button:ms`), `l2`, `r2`, `lup`/`ldown`/`lleft`/`lright` (left stick) and `a+b`; the PC
runner takes `--load-state` (the Vita's states load there too).

## State at the end of the 2026-11-10 session

- `re4_vu1_hle=1`: the world program (0x1408 and the 0x00A0/0x00C0 stubs) runs without VU1,
  drawn on the GS thread (CPU transform, exact against the JIT). Picture right on the PC. On the
  Vita the GS thread is then the limit (~930 ms/s; world batches ~196 of it).
- `gpu_world` (OFF by default, unfinished): world batches through a GLSL vertex shader. The
  transform matches the CPU path (debug prints), w > 0 for every drawn vertex, no GL error,
  but the draws produce no fragments on Mesa, even with the debug red override at the start
  of the fragment shader, no culling and no depth test (`--gpu-world-debug 14`). Next to check:
  the viewport/framebuffer bound at FlushWorld time (FlushWorld runs inside FlushBatch, which
  can be called after another target was bound), uniform array upload (uM location 0), and
  attribute 0 aliasing. Triangles touching the clip volume already go through the CPU clipper.
- Still to do: skinning program (0x0498/0x0000/0x0050 -> 0x01D0) HLE, the depth-of-field/haze
  read-backs (Z as PSMZ24 -> CT16 view of 0x310000 -> PSMT8H), the GS primitive path's cost.
- Real game frame rate on the Vita in gameplay: ~1-3 per second.
