# VitaPS2 roadmap: everything needed for the best PS2 emulator on Vita

Status legend: `[x]` done · `[ ]` to do · **(HW)** needs testing on a real Vita.
Phases are ordered; items inside a phase are roughly ordered by payoff.

## Phase 0: Foundation (done)

- [x] Research and pick the approach (Play! core + Vita frontend, see RESEARCH.md)
- [x] Play! core building for Vita with minimal patches
- [x] JIT executable memory pool (`sceKernelAllocMemBlockForVM`)
- [x] Portable software GS renderer
- [x] Vita frontend: game list, display, controls, audio
- [x] CI: host tests + VPK build with VitaSDK
- [x] First VPK produced
- [x] Profiling tools: log file, on-device GS benchmark, overlay, built-in self test

## Phase 1: Boots on hardware **(HW)**

- [ ] App launches; game list renders
- [ ] Built-in GS self test shows the triangle (validates JIT + BIOS HLE + GS on device)
- [ ] On-device GS benchmark numbers collected
- [ ] Memory budget confirmed (heap + JIT pool + textures fit; no crash on boot)
- [ ] First commercial game reaches its title screen
- [ ] Audio plays without crackling
- [ ] Controls, rear touch L2/R2, front touch L3/R3 work
- [ ] Exit to game list and boot another game without restarting the app
- [ ] Crash handler: write a crash report (registers, PC, last log lines) to the log

## Phase 2: Measure before optimizing **(HW)**

- [ ] Per-frame time breakdown: EE JIT, IOP, VU0/VU1, GS raster, presentation
- [ ] Sampling profiler on device (periodic PC sampling, hottest JIT blocks/functions)
- [ ] Test set of ~20 games across genres/engines with fps and status recorded
- [ ] Decide priorities from data: CPU-bound vs GS-bound share of the catalog

## Phase 3: Smoothness (felt performance)

- [ ] Frame pacing: steady 60/30/20Hz present cadence instead of jitter
- [x] Audio time-stretching when emulation runs below full speed (WSOLA, pitch preserved)
- [ ] Detect each game's real frame rate (buffer flips) and skip duplicate presents
- [ ] Adaptive internal resolution / interlaced rendering driven by frame time
- [ ] Automatic frame skip that never skips two frames in a row
- [ ] Fast-forward key and turbo mode

## Phase 4: GS renderer speed

- [x] Span rasterizer, fixed-point stepping, specialized span loops
- [x] Decoded texture cache with page-stamp invalidation
- [x] Multi-threaded rasterization (bit-identical)
- [x] Interlaced rendering / frame skip hacks
- [ ] NEON span kernels (4 pixels per iteration) for the hottest cases
- [ ] Faster perspective correction (NEON reciprocal), accuracy-checked
- [ ] Mipmapping (LOD) support with a fast path (currently LOD 0 only)
- [ ] Dirty-region rendering: reuse pixels when a region's draws are unchanged
- [ ] Effect detection: cheap or skippable full-screen blur/bloom/DOF passes
- [ ] GPU-assisted fill for large textured/blended primitives (hybrid GS)
- [x] GPU (vitaGL) renderer with render targets and on-demand GS memory sync
- [ ] GPU renderer: exact blending for `(A-B)*C+A` forms (e.g. `Cs*(1+As)`) via a custom GXM shader with framebuffer fetch; FIX blend + alpha test together
- [ ] GPU renderer: affine (not perspective) Gouraud color and fog interpolation on STQ primitives (differs when Q varies strongly within a triangle)
- [x] GPU renderer: fog (second texture stage interpolating towards FOGCOL)
- [x] GPU renderer: region clamp and (power of two) region repeat
- [ ] GPU renderer: alpha test fail modes, 16-bit dithering, depth buffer readback
- [ ] GPU renderer: upscaling (render targets at 2x where VRAM allows)
- [ ] Upscaled UI/2D option where it is cheap

## Phase 5: CPU emulation speed

- [x] AArch32 JIT: 128-bit values (VU registers, MMI) allocated in NEON registers q8-q15 instead of going through memory for every operation (84% of VU operands in registers on Play!'s VU tests)
- [x] AArch32 JIT: exact reciprocal / reciprocal square root (was a ~16-bit NEON estimate)
- [x] ARM cross build running Play!'s CodeGen and VU test suites under qemu in CI
- [ ] AArch32 JIT: FP32 values in registers (needs s0-s31 = q0-q7 handling)
- [ ] Optional accurate VU rounding (round toward zero) for games that need it (tri-Ace): ARMv7 NEON always rounds to nearest
- [ ] VU1 microprogram recompiler profiling and tuning (often the 3D bottleneck)
- [ ] Idle-loop and busy-wait detection, per game when the generic one misses
- [ ] Ahead-of-time block cache: prerecord and ship translated code per game
- [ ] Hot-function native replacements (hand-tuned C++/NEON) for top functions
- [ ] Middleware HLE: recognize and replace common library routines (memcpy/math, Criware, RenderWare)
- [x] IPU/MPEG movies: integer IDCT (IEEE 1180 compliant) and fixed-point color conversion
- [ ] IPU: NEON IDCT/CSC, profile VLC decoding on device
- [ ] Thread and core layout tuning (affinity for EE, GS, worker, audio)
- [ ] Explore unlocking part of the 4th core for audio/IOP

## Phase 6: Compatibility and accuracy

- [ ] Compatibility list (game, status, notes, settings) in the repo
- [ ] Per-game automatic fixes database (GameConfig.xml + VitaPS2 overrides)
- [ ] GS correctness gaps: mipmaps, dithering, anti-aliasing flag, region repeat edge cases
- [ ] Frame-dump replay test: record GS streams from games, compare against reference renderer
- [ ] Track and merge upstream Play! fixes regularly
- [ ] Memory card management (format, delete, per-game cards)
- [ ] Save states (Play! supports them; needs UI and storage handling)

## Phase 6.5: Auto-tuning across the catalog

- [ ] Engine/middleware detection per game (RenderWare, Criware, Unreal 2, ...)
- [ ] Automatic profile on first boot: measure, try speed hacks, keep the smoothest
- [ ] Shared community profiles file (import/export)

## Phase 7: User experience

- [ ] LiveArea polish, icons, cover art in the game list
- [ ] Settings screen: global defaults + per-game overrides
- [ ] On-screen controller remapping; analog deadzone and sensitivity
- [ ] Display options: filtering (nearest/bilinear/sharp bilinear), aspect, position
- [ ] Widescreen patches (per game) for the 16:9 screen
- [ ] Suspend/resume handling (audio, timers, VM state)
- [ ] Battery and clock profiles (444 MHz for heavy games, lower for light ones)
- [ ] PS TV: DualShock 3/4 support and TV output resolution

## Phase 8: Project health

- [x] Automated tests: GS unit, differential, parallel, JIT pool, end-to-end ELF
- [ ] Hardware test checklist run before each release
- [ ] Versioned releases with changelogs; VPK attached to GitHub releases
- [ ] Contributor guide and issue templates (game, settings, log, overlay numbers)
- [ ] License notices for Play! and bundled libraries in the app

## Long shots (research)

- [ ] Frame interpolation on the Vita GPU (30 to 60 fps perceived)
- [ ] Static recompilation of showcase titles (PS2Recomp-style)
- [ ] Hybrid: native per-game ports for the most popular titles
