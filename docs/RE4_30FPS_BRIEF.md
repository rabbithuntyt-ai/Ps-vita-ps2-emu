# Resident Evil 4 on a PS Vita PS2 emulator: what has been done, and what is left

A brief for anyone asked how to get from about 3 to 30 game frames per second. Every number
below was measured on a real Vita unless it says PC. Written 24 Nov 2026 from the repository's
own notes (`docs/PERF_WORK_NOT_DONE.md`, `docs/PATH_TO_30FPS.md`).

## The setup

- **Hardware:** PlayStation Vita. ARM Cortex-A9, 4 cores at 444 MHz (3 for applications; the
  fourth is shared with the system and unlocked by the app's own kernel hook), NEON, 512 MB RAM
  plus 128 MB of video memory, PowerVR SGX543MP4+ GPU.
- **Emulator:** a fork of Play! (an open source PS2 emulator), with about 110 patches on top. The EE
  (the PS2's main MIPS R5900 CPU) and the VUs run through Play's JIT, which has an AArch32 back
  end (block linking, an indirect jump table, a tier 2 optimizing pass, fused guest memory
  ops). Graphics go through vitaGL (an OpenGL layer over the Vita's GXM API).
- **Threads:**
  - **Emulation thread** (core 0): the EE, the IOP (I/O processor) and the SPU2 timing, plus
    handing VIF1 data and VU1 output to the other threads.
  - **VU1 worker** (core 2): VIF1 unpacking and VU1 programs (asynchronous since patch 0097).
  - **GS thread** (core 1, shared with the UI): the GS (PS2 graphics chip) emulated on the CPU,
    turning GS register writes into GL draws, with render targets kept on the GPU.
  - **Core 3:** audio output and the profiler.
- **Measuring:** 120 s scripted walk from a gameplay save state (`runs/re4/gp.sh`), real game
  frames counted (`game_fps`), sampled profiler sections per thread (1 ms samples). The same
  build varies about ±5% run to run, so changes need alternating runs.
- **Constraints:** no overclocking (the remote tooling took the Vita offline once, and does not
  go above 444 MHz), no third-party kernel binaries.

## Where RE4 stands (24 Nov 2026)

- **About 3.2 game fps** in gameplay: 290-320 game frames per 120 s, against 0.66 on 15 Nov.
  The game targets 30. The emulated machine runs at about 9 of 60 vblanks a second (15% speed).
- **Per game frame (~300 ms),** per thread:

| Thread | Where the time goes (ms per game frame) |
|---|---|
| Emulation thread (the limit) | RE4's own code (EE JIT) 83; waiting for the GS thread 40-70; queueing VIF1 data 34 (copy 18, rest 16); relaying VU1 output to the GS 40 (DIRECT 15, PATH1 17, world 8); waiting for VU1 15-20; DMA register writes 10; IOP 17; SPU and other 10 |
| GS thread | Busy ~280: image data into GS memory 40, other 9, world batches 39, GL flush 38, read-backs 26, render targets 25, target uploads 20, vertex conversion 21, textures 18, vertices 16, registers 13, primitives 9, state 8; idle ~20 |
| VU1 worker | Busy ~170, idle 40%: native skinning 52, world handler 34, unpack 29, JIT VU1 programs 21, VIF1 commands 19, DIRECT 17, XGKICK copies 7 |

- **Workload per game frame:** about 3 MB of VIF1 data in ~9,400 transfers; ~640 XGKICK packets
  (1.2 MB); ~1,700 VIF1 DIRECT transfers (1.6 MB); ~1,500 RE4 world batches (~160,000
  vertices); ~84,500 GS register writes; ~370 GL draws of ordinary batches and ~210 retained
  world draws; ~320,000 render target pixels read back and ~210,000 uploaded; ~3,000 EE I/O
  register accesses; about 2,250 timer interrupts per emulated second.
- **Cost of one GL draw through vitaGL:** ~63 µs (fixed-function path), ~27 µs (own GLSL program),
  ~44 µs (retained world draw with uniforms).
- **EE JIT code density:** 23 ARM instructions per EE instruction in RE4's hot code (77% the
  instructions' own work, 10-12% guest register loads and stores, 8% branches, 5% cycle quota and
  exit bookkeeping). RE4's game code has a flat profile: the hottest function is 3.2% of EE time,
  the top 20 about half.

## Work done and kept

Ordered roughly by effect. "Exact" means pictures compared identical (PC screenshots or Vita
frame dumps) or results compared bit for bit.

### Threads and data flow

1. **Asynchronous VU1 (patch 0097).** VIF1 DMA data is queued and the worker runs the unpacks
   and VU1 programs; PATH1/PATH2 order is kept. Nearly doubled gameplay speed (0.6-0.8 to
   1.0-1.4 game fps).
2. **Wake the worker only when it sleeps (0104).** It was a system call per queued packet.
3. **VIF1 queue as a lock-free ring (0109).** One copy into a 4 MB ring and an atomic publish,
   with an in-order overflow queue. Queueing 68 to 34-41 ms per game frame.
4. **Four vblanks of GS lead (0106).** The emulation thread ran only 2 vblanks ahead of the GS
   thread (less than one RE4 game frame). +8%.
5. **Relay: GIF PACKED loop with its state in registers, GS register loop that kicks vertices
   directly, one GS call per world batch (0120).** Relay ~50 to ~37 ms. +5%.
6. **Bulk VIF unpack (0121).** V4-16/V4-32 unpacks read runs straight from the source with NEON
   widening. Worker unpack 60 to 25 ms per game frame.

### RE4-specific replacements (exact or near exact)

7. **RE4's world VU1 program done in C++ (`re4_vu1_hle`) and drawn by a vertex shader
   (`gpu_world`).** +60-70% on top of asynchronous VU1. 0.10-0.15% of pixels differ from the
   CPU path.
8. **World geometry retained on the GPU (W2).** Batches keyed by hash and kept in static buffers;
   consecutive batches with the same uniforms merged into one draw (182,884 to 29,684 draw calls
   in a PC run). +3%.
9. **RE4's skinning programs translated ahead of time to C++ (W3, `tools/vu_aot.py`, 0105).**
   68,269 programs compared bit exact against the JIT. Skinning off the VU1 JIT.
10. **Depth-of-field skip (`re4_skip_dof=176`, as PCSX2's RE4 rule).** Loses the background
    blur.

### GS renderer (GS thread)

11. **Image transfers into GS memory a GS block at a time (0102), PSMT4 blocks by NEON table
    lookups and PSMCT32 blocks 8 bytes at a time (0108).** Image data 95 to 40 ms per game
    frame.
12. **Render target read-backs and uploads a GS block at a time (`TargetPixels`).** 17.6 to 10
    and 20 to 11 ms.
13. **Render targets pooled; old targets copied into a new layout on the GPU instead of through
    GS memory (W1 relayouts, partial and front relayouts).**
14. **GS state work:** vertex writes no longer mark the state dirty; rewrites of a register with
    its current value no longer force a new state; reciprocals cached.
15. **The GS thread's register path:** per-vertex registers stored and kicked by a direct loop
    (0120). GS thread "other" 60 to 50 ms.

### VU recompiler (helps every game)

16. **VU1 JIT improvements (V2 stages, 0098-0100):** outer products without aliasing VF registers;
    a loop copy with compile-time stall decisions; 10 NEON registers without clamping; direct
    128-bit loads into allocated registers; cheaper sticky flag code. RE4's old vertex loop
    1,436 to 1,019 ARM instructions per iteration (-29%), before it was replaced natively.

### EE JIT and the rest

17. **Fused 128-bit guest memory op for LQC2/SQC2 (0122)**, as LQ/SQ already had. No measurable
    change.
18. **Native MPEG decoding for movies (`mpeg_hle`)**, ~15 of 30 pictures a second in RE4's
    movies (movies only).
19. **Fourth core:** audio output and the profiler on core 3. Small gain, near noise.
20. **Profiling built along the way:** sampled sections for all threads, GS sections, GPU parts,
    read-backs by cause, EE I/O per register, EE hot spots by 32-byte bucket, a JIT code
    density report (`tools/jit_arm_report.py`).

## Tried, measured, and not kept

| What | Result | Why not kept |
|---|---|---|
| Decoding XGKICK packets on the VU1 worker (E1) | Exact, but 228 against 242 game frames | The relay did not shrink and the worker became the wait |
| Turning off the per-vblank wait for VU1 | Exact; no gain | The time moved to waiting for the GS thread |
| Lazy image transfers (W4: GS memory written only when read another way) | Exact; only 8% of transfer work saved | RE4 reuses texture slots for other shapes, so the overlap must be written anyway |
| GPU pass for RE4's depth-as-alpha-texture trick | Exact | Saved no read-back: textures streamed into the same buffer force it |
| Dirty areas by 16-row bands | Exact | Same pixels read back moments later, when the buffer changes layout |
| Uploading stale rows so old targets can be copied on the GPU | Read-backs -15% on the PC | On the Vita uploads cost 2.5x and RE4 crashed (writes into textures in use) |
| Bands plus GPU copies that leave stale rows to GS memory | Uploads -41%, read-backs only -6% | Pictures wrong in two rows of the blur; at best ~4 ms |
| NEON copy for the VIF1 ring | memcpy 800-1,000 MB/s, the NEON loop 400 | Slower |
| Own GLSL program for batches (`gs_shaders`) | GL time per batch 63 to 27 µs; wait on the GS thread 70 to 40 ms | No frame rate gain (the emulation thread is the limit). Crashed with 16 MB of vertex ring (GPU memory exhausted in a texture upload); fine with 4 MB. Off by default |
| Native C++ for EE library functions (E3: matrix, sin/cos, memset, float conversion) | Exact (17,780 calls checked by a harness) | No gain: for short functions the call in and out costs what the JIT code did |
| IOP on its own thread on core 3 | Movies +4%, gameplay -12% | Core 3 is shared with the system |
| SPU2 mixer on core 3 | Slower | Same |
| Live range splitting in the VU register allocator | Loop longer | 8 NEON registers are too few |
| Palette texture decoding on the GPU (G4) | Not built | Texture decoding is 2 ms a second; caches already hit |
| 500 MHz overclock | Not possible | The tooling caps at 444 MHz and once took the Vita offline |

## Not done yet, and why

| Idea | Estimated gain | Why not done |
|---|---|---|
| Characters drawn straight from native skinning output on the GPU (as the world is) | 15-25% overall | Several days of RE4-only work; skinning's lighting not analysed. Skips the relay, the XGKICK copies and the GS register path for characters |
| EE JIT rework: VU0 flag work only where read; LDL/LDR pairs as one unaligned load; guest registers kept across linked blocks | Each 1-5% of EE time, together perhaps 10-20% of game code | Days to weeks; each step is below run-to-run noise, so it can only be judged in total. VU0 sticky flags accumulate every result, which limits flag elimination |
| Native C++ for RE4's long game functions (`partsWorldCalc` 3.3%, `HermiteInterpolation` 3.1%, collision tests) | A few ms each | The harness exists; the profile is flat, so many functions are needed for a large gain |
| Skip VIF data for geometry already on the GPU (W2 step c) | Most of the 3 MB per game frame of queueing and unpack | Assumes no later VU program reads the skipped data; needs a per-game switch and picture checks |
| Draw through GXM directly instead of vitaGL (W6) | Up to ~30 ms of GL time | Draw cost is not the current wall; only matters once the others are gone |
| Drop RE4's post-processing (blur buffers, colour passes) as a quality option | ~70 ms of GS thread work (read-backs, uploads, targets) | Changes the picture; useless while the emulation thread is the limit |
| Timer interrupt done natively (libtimer's TIMER1 handler, 2,250 a second) | ~5 ms per game frame | Small; needs care with the game's alarms |
| Native replacements for IOP modules (SIO2MAN, PADMAN, libsd, Capcom's mixer) | Part of 17 ms | Small in gameplay |
| A new VU recompiler (V2 complete) | Large for VU-heavy games | RE4's heavy VU programs are already native |
| Run the game's draw phase every second tick (R2) | Halves draw work | A fallback (not a true 30); gate G5 never run |

## Why 30 is far away

- **All three threads need cutting together.** Even with no waiting at all, the emulation
  thread needs ~230 ms per game frame (about 4.3 fps). At 30 fps each thread has 33 ms.
- **The emulation thread is about 7x over budget on its own work.** RE4's game code alone is ~83
  ms per game frame, about 2.5x the whole budget, and its profile is flat.
- **The GS thread is about 8x over.** The GS is emulated on the CPU: every register write,
  vertex, primitive and render target round trip is CPU work on a 444 MHz core.
- **The VU1 worker is about 5x over**, even with skinning and the world done natively.
- **Every incremental step left measures in single percent**, below the ±5% noise of a run.

## Questions worth asking

1. Is there a way to run RE4's game logic (the EE) at full speed on a 444 MHz Cortex-A9 at all,
   given ~83 ms per game frame of JIT code today and a flat profile?
2. Short of a full rewrite, what architecture would bypass the PS2's VIF/VU1/GS data path for
   one game (a game-specific renderer fed from the game's own data structures)?
3. Which parts of a PS2 GS emulation can move onto the Vita's GPU (SGX543MP4+) when render
   targets are reused in different layouts every frame?
4. Is a frame-skipping or half-rate scheme (R2) acceptable, and how would it keep game timing
   right?
