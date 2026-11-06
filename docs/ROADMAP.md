# VitaPS2 roadmap: everything needed for the best PS2 emulator on Vita

Status legend: `[x]` done · `[ ]` to do · **(HW)** needs testing on a real Vita.
Phases are ordered; items inside a phase are roughly ordered by payoff.

## Library-wide plan (current priority)

Goal: most of the PS2 library functional, starting with the **top 50 PS2
games that have no Vita version**. Work that helps one game only (hand
ports, per-game rewrites) comes last; everything here targets code that many
games share: Sony's SDK libraries, middleware (RenderWare, Criware, Havok),
common VU1 microprograms and common GS usage. What single-game work has
produced so far, and how it extends to other games: [LIBRARY_WIDE.md](LIBRARY_WIDE.md).

1. **Compatibility harness first.** Vita Lab runs each game unattended
   (autostart, a few minutes, button presses to get past menus) and collects
   screenshots, `perf.csv` and crash dumps. A compatibility table and a
   cross-game chart in the repo replace the single Silent Hill 2 line; the
   bottleneck shared by the most games is worked on next.
   - [x] Harness script `tools/vitalab_compat.sh` (boot, press past menus, collect, classify: crash / no boot / boots with fps and bottleneck)
   - [x] Top-50 list with the Play! core status of each game (docs/COMPATIBILITY.md)
   - [x] `docs/COMPATIBILITY.md` measured-runs table updated per run (`tools/compat_report.py`); [ ] cross-game fps chart
2. **Library HLE by signature.** Find library functions by their code
   bytes (not addresses) and run native ARM/NEON versions; libmpeg's
   `sceMpegGetPicture` (`src/common/MpegHle.cpp`) is the template.
   - [ ] Finish libmpeg on hardware (movies at full speed in every libmpeg game). libmpeg 3.0 under Sofdec added, 13 of 30 pictures/s in RE4 (docs/RE4.md)
   - [ ] Signature scanner shared by all HLE modules (masked code patterns, per-SDK-version variants)
   - [ ] memcpy/memset/strlen and math (sin/cos/sqrt/atan2) from the SDK's libc and libm
   - [ ] Criware (Sofdec/ADX) movies and audio
   - [ ] RenderWare and other middleware hot paths, picked by the harness's hotspot data
3. **Persistent JIT cache per disc serial.** The first play compiles and
   stores blocks (keyed by code hash), later plays load them: scene changes
   stop stalling after one pass, for every game. See docs/AOT_TRANSLATION.md.
   - [x] EE blocks stored with relocations in `ux0:data/VitaPS2/jitcache/<game>.jit` (patches 0064, 0065), loaded on later runs, JIT on a miss; tested end to end under qemu (ARM CI)
   - [x] Measured on the Vita: RE4 reaches the title in half the time on a second play. IOP and VU blocks cached (0066). Blocks read from the file on demand (0069)
4. **VU1 microprograms on the GPU.** Games reuse a small set of transform
   and lighting microprograms (SDK samples, middleware). Translate the common
   ones to GXM vertex programs, keyed by microcode hash, so the Vita's GPU
   does the work VU1 does on the CPU today (the usual 3D bottleneck).
   - [ ] Microprogram census across the test set (hash, size, frequency)
   - [ ] Translator for the common subset (FMAC, loads/stores, XGKICK output) with a CPU fallback
5. **Automatic per-game tuning.** On first boot, measure and try the speed
   hacks (EE rate, frame skip, VU1 thread, interlaced rendering), keep the
   smoothest per serial (extends `assets/game_profiles.ini` and Auto EE rate).
   - [x] `CAutoTuner`: VU1 thread and frame skip tried with A/B/A measurements, result in `settings/<game>.auto.ini` (EE rate stays with Auto EE rate)
   - [ ] Interlaced rendering and the software renderer as further candidates
6. **GS changes that help many games.** Mipmaps, dithering, the remaining
   blend modes; detection of full-screen effects (blur, bloom, depth of field)
   that can be cheapened or skipped.
7. **Whole-function translation on the Vita (tier 2).** Hot code regrouped
   into whole functions and recompiled in the background on an idle core,
   with whole-function analysis (register width, calling conventions, direct
   RAM access); stored in the persistent JIT cache. No PC needed. Design,
   cache findings and expected gains: docs/AOT_TRANSLATION.md.
8. **More IOP modules replaced natively.** The IOP took up to 20% of the PS2
   thread in Silent Hill 2's menus. Play! already replaces many SDK IOP
   modules (IRX) with native code; extend that list from the harness's
   per-game module logs, starting with sound drivers and CD streaming.

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

- [x] App launches; game list renders
- [x] Built-in GS self test shows the triangle (validates JIT + BIOS HLE + GS on device)
- [x] On-device GS benchmark numbers collected
- [x] Memory budget confirmed (heap + JIT pool + textures fit; no crash on boot)
- [x] First commercial game reaches its title screen
- [ ] Audio plays without crackling
- [x] Controls, rear touch L2/R2, front touch L3/R3 work
- [x] Exit to game list and boot another game without restarting the app
- [x] Crash reports: `crash.txt` on C++ terminate; Vita core dumps (`psp2dmp`) read on a PC by our own parser (stop reason, registers, module bases)

## Phase 2: Measure before optimizing **(HW)**

- [x] Per-second time breakdown in `perf.csv` (autostart mode): ~70 counters, EE/IOP/VU/VIF/SPU/IPU/JIT sections and sub-sections, JIT compile phases, GPU stages, idle triggers per register and DMA channel, VIF unpack formats
- [x] Sampling profiler on device: EE hotspot buckets (`eehot.txt`) and EE RAM dumps for disassembly on a PC
- [ ] Test set: the top 50 PS2 games without a Vita version (see "Library-wide plan"), run unattended by the compatibility harness
- [ ] Decide priorities from data: CPU-bound vs GS-bound share of the catalog

## Phase 3: Smoothness (felt performance)

- [ ] Frame pacing: steady 60/30/20Hz present cadence instead of jitter
- [x] Audio time-stretching when emulation runs below full speed (WSOLA, pitch preserved)
- [ ] Detect each game's real frame rate (buffer flips) and skip duplicate presents
- [ ] Adaptive internal resolution / interlaced rendering driven by frame time
- [ ] Automatic frame skip that never skips two frames in a row
- [ ] Fast-forward key and turbo mode
- [ ] Sharpening upscaler for the 960x544 screen (FSR-style / xBR shader on the GPU): image quality
- [ ] Frame generation ("Lossless Scaling" style) for games whose logic already runs at full
      speed but render at 30 fps or with frame skip: GPU-blended in-between frames first,
      then motion-compensated interpolation (block motion search on the GPU). Smoothness
      only: it can't speed up game logic, and adds about a frame of latency

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
- [x] AArch32 JIT: FP32 values in registers s16-s23 (saved in the prologue only by blocks that use them)
- [x] AArch32 JIT: chained frames between linked blocks (no prologue/epilogue per block jump)
- [x] AArch32 JIT: fused guest memory ops: every EE/IOP load and store (8 to 128 bits, LWC1/SWC1) is one JIT op with an inline page-table fast path and an out-of-line handler call, so guest registers stay in host registers across memory accesses instead of being spilled and reloaded around each one (~40% faster on a memory-heavy EE loop under qemu; checksum test against the x86 reference)
- [x] JIT register allocator: live intervals (load before first use, save after last write) so registers are shared between short-lived values; context state (PC, cycle quota, exception flags) allocated between guest memory ops; r9 as a 7th allocatable register
- [x] MIPS branches without control flow (select) and a block epilogue with a fast path (quota left, no exception: one compare then jump to the linked block)
- [ ] AArch32 JIT: page table pointer pinned in a host register
- [x] EE: track sign-extended 32-bit values in a block: branches, SLT/SLTI, MOVZ/MOVN tests, AND/OR/XOR/NOR, ORI and register moves on them use the lower halves only (no 64-bit compares, upper halves not kept in registers)
- [x] Random EE code fuzzer (tools/make_ee_fuzz_elf.py): checksums of all registers compared between x86, ARM and every JIT mode
- [ ] EE: carry sign-extension knowledge across blocks (guarded block entry)
- [ ] Jitter: keep register allocation across if/else diamonds (block epilogue still reloads state)
- [ ] AArch32 JIT: keep guest registers in host registers across linked blocks
- [ ] Optional accurate VU rounding (round toward zero) for games that need it (tri-Ace): ARMv7 NEON always rounds to nearest
- [ ] VU1 microprogram recompiler profiling and tuning (often the 3D bottleneck)
- [x] VU: sticky flags of FMAC instructions merged into one pipeline entry per block when the block doesn't read/set the status (was ~15 memory operations per FMAC instruction); VuTest case added
- [x] VU (AArch32): float clamp masks kept in q6/q7 (loaded once per function/chain instead of per clamp); lane-masked writes with VBIT/whole-register moves (was 2 core-register transfers per lane)
- [x] VU (AArch32): sign/zero flags of FMAC instructions whose MAC result is unused accumulated in a vector register (no NEON to ARM transfer per instruction, which stalls the Cortex-A9); one conversion per block
- [ ] Idle-loop and busy-wait detection, per game when the generic one misses
- [ ] Ahead-of-time block cache: prerecord and ship translated code per game
- [ ] Hot-function native replacements (hand-tuned C++/NEON) for top functions
- [ ] Middleware HLE: recognize and replace common library routines (memcpy/math, Criware, RenderWare)
- [x] IPU/MPEG movies: integer IDCT (IEEE 1180 compliant) and fixed-point color conversion
- [ ] IPU: NEON IDCT/CSC, profile VLC decoding on device
- [x] Threaded VU1 (MTVU): microprograms run on the third core; XGKICK packets are copied and sent to the GIF in order on the EE thread; sync at vblank, VU1 register/micro memory access, CMSAR1 (per-game toggle)
- [x] MTVU: no deadlock when a program waits for VIF1 data (bounded vblank / register waits)
- [x] SPU2 mixer: per-voice block loop (bit-identical output, ~1.45x faster; golden test)
- [x] SPU2 mixing on its own core: the IOP syncs before touching SPU state; IRQ flag polled atomically
- [x] SPU2: silent voices skip mixing (bit-identical, 22x less SPU work with no sound)
- [x] Emulation thread profiler (time per subsystem on the overlay)
- [ ] MTVU: sync EE direct VU1 data memory accesses (currently unsynchronized, like unsafe games on real hardware)
- [ ] Thread and core layout tuning (affinity for EE, GS, worker, audio)
- [ ] Explore unlocking part of the 4th core for audio/IOP

## Phase 6: Compatibility and accuracy

- [ ] Compatibility list (game, status, notes, settings) in the repo
- [ ] Per-game automatic fixes database (GameConfig.xml + VitaPS2 overrides)
- [ ] GS correctness gaps: mipmaps, dithering, anti-aliasing flag, region repeat edge cases
- [ ] Frame-dump replay test: record GS streams from games, compare against reference renderer
- [ ] Track and merge upstream Play! fixes regularly
- [ ] Memory card management (format, delete, per-game cards)
- [x] Save states: SELECT + R / SELECT + Triangle, one per game, refused while an IPU command runs (a state saved mid-command crashed on load)

## Phase 6.5: Auto-tuning across the catalog

- [ ] Engine/middleware detection per game (RenderWare, Criware, Unreal 2, ...)
- [x] Bundled per-game profiles keyed by disc serial (`assets/game_profiles.ini`)
- [x] Auto EE cycle rate (steps 100→90→75→60% while EE-bound, climbs back with headroom)
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
