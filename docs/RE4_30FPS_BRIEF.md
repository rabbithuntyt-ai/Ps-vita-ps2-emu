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

- **About 3.4 game fps in steady gameplay since GPU skinning** (3.36-3.45; 2.81-3.25 before it,
  measured from the perf.csv timestamps from 25 s on), **2.6-2.9 over a whole run** (290-323 game frames in about
  111 s of rows, including about 14 s of state loading with no game frames). The game targets 30:
  about 10x. The emulated machine runs at about 9 of 60 vblanks a second.
- **Audit note (24 Nov):** earlier figures (3.2-3.6 game fps, and ms per game frame about 8% low)
  averaged per-row counts as if each perf.csv row were one second; rows are 1.10 s apart. All
  numbers below use the rows' own timestamps (`tools/perf_audit.py`).
- **Profiler sections are exclusive:** each thread has one current section, sampled every
  millisecond, and a thread's sections sum to its wall time per game frame (about 330 ms at 3.0
  fps). Waits are their own sections, so active time is the sum minus the wait sections.
- **Per game frame (~330 ms of wall time),** per thread (runs R8a, R8c, R10a, R10b):

| Thread | Where the time goes (ms per game frame, exclusive sections) |
|---|---|
| Emulation thread (the limit) | Total 300-345. Active ~230: RE4's own code (EE JIT) 88-93; queueing VIF1 data 31-40 (copy 17-21); relaying VU1/VIF1 output to the GS 37-47 (DIRECT 16-17, PATH1 14-20, world 7-10); VIF1 commands 8-11; DMA register writes 9-12; IOP 20-21; SPU 5-6; other 5-10. Waits: GS thread 73-78, VU1 16-25 |
| GS thread | Busy ~280: image data into GS memory 40, other 9, world batches 39, GL flush 38, read-backs 26, render targets 25, target uploads 20, vertex conversion 21, textures 18, vertices 16, registers 13, primitives 9, state 8; idle ~20 (per-row figures, about 8% low) |
| VU1 worker | Total 307-351. Busy ~190: native skinning 45-63, world handler 29-42, unpack 23-36, JIT VU1 programs 18-26, VIF1 commands 16-23, DIRECT 17-19, XGKICK copies 6-9. Idle 133-152 |

- **Pipeline view:** steady throughput is set by the slowest stage plus the synchronization that
  does not overlap. Today the emulation thread's active work (~230 ms) is the longest; the GS
  thread (~280 ms busy, but it overlaps with the emulation thread through 4 vblanks of lead) is
  next; the VU1 worker (~190 ms) third.

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

## New measurement: the GS data is almost all repeated (24 Nov)

PC runner, RE4 walk, 420 vblanks, every GIF packet the relay hands to the GS hashed (patch 0123,
`--reuse-scan`); "repeated" means the same bytes were relayed in the previous 6 vblanks
(about 2-3 game frames):

| Relayed GIF data | Size | Repeated |
|---|---|---|
| XGKICK packets (VU1 output: characters, effects) | 176 MB, 97,750 packets | 0.2% of bytes |
| VIF1 DIRECT, PACKED register data | 152 MB | 97% |
| VIF1 DIRECT, REGLIST register data | 38 MB | 99% |
| VIF1 DIRECT, IMAGE (texture uploads) | 56 MB | 99% |

- Per game frame that is about 1.2 MB of register data and 0.37 MB of image data sent by the
  game's EE code through VIF1 DIRECT, nearly all identical to what it sent a few vblanks before.
  This register data is most of the ~84,500 GS register writes per game frame.
- Its cost today, along the whole path: the emulation thread copies it into the VIF1 queue
  (part of 31-40 ms) and parses it into register writes (DIRECT relay, 16-17 ms); the VU1 worker
  copies it out of the queue (DIRECT, 17-19 ms); the GS thread decodes, assembles primitives,
  converts vertices and batches them (registers, vertices, primitives, vertex conversion, part of
  flush: roughly 60-90 ms) and writes the images into GS memory (40 ms).
- **With the GS register state at the packet's start in the key** (patch 0124): 236,640 of the
  260,995 DIRECT packets (225 of 246 MB) still repeat.
- **But the register path is not DIRECT's:** register writes by GIF path over the same run are
  PATH1 11.5 million (89%), PATH2 (DIRECT) 1.2 million (9.4%), other 0.2 million. DIRECT packets
  are mostly texture uploads (a few register setups, then image data: their first tag reads as
  PACKED). The register path is VU1 output, which does not repeat.
- **And all VU1 output is skinning:** XGKICK bytes by the program that made them: 0x0000 87.0 MB
  (46,858 packets) and 0x0050 88.4 MB (47,443), nothing else above 1 MB.
- **So a GS command cache (the roadmap's Priority 1) is not viable for RE4:** the commands that
  cost GS thread time are skinned vertices, new every frame; the repeated ones are texture
  uploads, which cannot be skipped because RE4 reuses each texture slot for other shapes (W4,
  tried 17 Nov: 8% saved).

## The roadmap's items, measured (24 Nov)

Status of each item of the external review ("PS Vita PS2 Emulator Performance Roadmap",
24 Nov) after measuring:

| Roadmap item | Status | Evidence |
|---|---|---|
| Priority 0: frame counter, timer window, profiler buckets | Done | Rates corrected (rows are 1.10 s apart); sections exclusive and summing to wall time (`tools/perf_audit.py`) |
| Priority 1: GS command caching | Not viable for RE4 | 89% of register writes are skinned vertices, 0.2% repeated; the repeated DIRECT data is texture uploads |
| Priority 1: GPU-resident GS memory | Tried earlier in three forms, not kept | Dirty bands, stale-row uploads, stale-row GPU copies: read-backs move rather than vanish (RE4 reuses its blur buffer memory in another layout each frame), one form crashed, one drew wrong rows |
| Priority 2: VU1 to GPU | **Built for RE4's skinning (`gpu_skin`, on in RE4's profile): +15-19%** | See "GPU skinning" below: 3.36-3.45 game fps against 2.81-2.90 |
| Priority 3: EE trace compilation | Partly measured | 23 ARM instructions per EE instruction, 77% the instructions' own work; LQC2/SQC2 fused with no measurable change; cross-block register retention not built |
| Priority 4: dependency scheduling | Not promising now | The VU1 worker is idle 133-152 ms per game frame and the emulation thread is the limit; moving work between threads was tried twice (E1, IOP on core 3) and lost |
| Priority 5: reduced precision, title-specific | Used where exact enough | RE4's world (0.10-0.15% pixels differ) and skinning (bit exact) are already title-specific |

**The skinning program, as read from VU1 microcode (entries 0x0000/0x0050, loop 0x02C8-0x0478):**
setup loads a projection matrix and a clip matrix (VU1 memory 0-7), a light direction and colour,
an ambient colour and a material colour. Each vertex is 3 qwords: integer position (with a scale
from the batch header) and a pointer to its bone list, integer normal and a strip restart flag,
integer texture coordinates. The bone list gives 1-N bone indices and weights; their matrices are
blended, then the position and normal are transformed; one directional light plus ambient, times
the material colour, clamped to 255; the position is projected (1/w, 12.4 fixed point) and the
texture coordinates scaled by Q. Output is ST, RGBA, XYZ2 per vertex under one GIF tag; a
triangle is skipped (ADC) when any of its three vertices is outside the clip box or where the input
restarts the strip.

**Checked:** the same math written as plain float code (`src/common/Re4SkinModel.cpp`, PC runner
`--skin-model`) against the bit-exact native program over the RE4 walk (420 vblanks): 94,301
batches, 3,800,968 vertices. All 2,588,369 drawn vertices are identical in screen position, depth,
colour and texture coordinates; the skip bit matches for all but 731 vertices (0.02%, the first
vertices of a batch, which use clip flags left by the previous run). Per game frame: about 616
batches and 25,000 vertices, a third of them skipped. The semantics are understood; a shader
version would differ only by the GPU's own float rounding.

**Risks of a GPU version:** the PS2 skips triangles that leave the clip box, the GPU would clip them
(different only at the guard band, as the GPU world already accepts); bone matrices indexed per
vertex in the shader (vitaGL's GLSL translation got a dynamic uniform index wrong for the world
shader on the Vita, so this likely needs a Cg shader); float rounding in the lighting.

## GPU skinning (24 Nov, `gpu_skin`, patch 0125)

- **How:** the VU1 worker no longer runs the skinning program's output stage. For each batch it
  computes every vertex with the program's own arithmetic (bone blend, projection, lighting,
  texture numerators, the skip bit: the same code as the bit-exact model) and hands the batch
  to the GS thread in the RE4 world path's form, already projected (`WORLD_BATCH::clipSpace`).
  The GS thread draws it with the world shader (one switch: the position is taken as given), and
  consecutive skinning batches share one draw. No XGKICK packet, no relay parse, no GS register,
  vertex or primitive work for characters. The VU registers the program would leave are set from
  its setup (the vertex loop's leftovers matter to nothing after it: PC pictures identical
  without them).
- **PC (RE4 walk, 420 vblanks):** all 94,301 skinning batches on the new path; GS register writes
  12.9 to 1.56 million; relayed XGKICK packets 97,750 to 3,449; about 10,000 extra draws for
  94,000 batches. Pictures: 6-9% of pixels differ, nearly all by 8 levels or less (99-214 pixels a
  shot by more, at most 69): GPU rasterization of the same triangles; no visible difference.
- **Vita (same build, alternating, 120 s):** 3.34 and 3.28 game fps with it, 2.90 and 2.81
  without (332 and 327 game frames against 289 and 279). The emulation thread's wait on the GS
  thread 86 to 33-38 ms per game frame; the GS thread's vertex, primitive and vertex conversion
  work for characters ~47 to ~2 ms; the VU1 worker does the vertex work instead (skinning 58 to
  88-92 ms) and is still idle 77-79 ms. No crash dumps; Vita frame dumps look right.
- **First version, kept off:** batches with one bone matrix only, each drawn with its own matrix
  uniform: +2%, because every batch became its own GL draw on the Vita (one matrix slot per
  draw there).
- **Then:** the blended bone matrix made once per run of vertices sharing a bone list (same
  arithmetic; the worker's skinning 88-92 to 50-67 ms per game frame), and the per-batch clock
  reads of the world and register timers only with `fine_timers`. With RE4's profile (`gpu_skin`
  on): **3.36, 3.45, 3.40, 3.44 game fps** in four runs (R13c/d, R15a/b).
- **Where a game frame goes now (R15, ~290 ms):** emulation thread: game code 91-92, waiting for
  the GS thread 33-44, waiting for VU1 22-25, VIF1 queueing 35-39, DIRECT relay 17, world and skin
  batch submits 11-13, DMA register writes 11-12, IOP 17-21, SPU 6, other 5: **about 240 ms of
  its own work, the limit now.** GS thread: image data and other 54, flush 30, world batches 30
  (plus retained 13, skinning 16), targets 26, read-backs 26, uploads 20, textures 19, idle 18.
  VU1 worker: idle 94-109.

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
16. **Image transfers stored without comparing the old bytes (0127, `feed_compare=0`, on in
    RE4's profile).**
    - A new GS section (`feedWrite`) showed that writing image data into GS memory was 34-35 ms
      of the GS thread's "other" per game frame, about 1.6 MB.
    - On the PC, 99.6% of those units already went through the block path.
    - The cost was reading the old bytes to report "changed". That report only lets a transfer
      that changes nothing skip refreshing textures and targets, and in RE4 that is 16 of 52,779
      transfers.
    - Vita self-test: PSMCT32 blocks take 12.5 ns per byte with the compare and 4.8 without;
      PSMT4 takes 14.6 and 7.9; memcpy takes 1.8.
    - Exact: on the PC, the same RE4 walk gives identical screenshots and identical read-back and
      upload counts. The host test checks memory against the comparing path.
    - Vita, interleaved, 120 s each:
      - store only: **3.70, 3.88, 3.80** steady game fps (mean 3.79)
      - compare: **3.59, 3.52, 3.58** (mean 3.56)
      - +6.5%; the two sets don't overlap.
    - `feedWrite` went from 34 to 22-23 ms, and the frame from 281 to 264 ms. The default stays 1
      for games that resend the same images (menus).
17. **World triangles the CPU clipper would return nothing for skip it (`clip_reject`, on;
    exact).**
    - About 3,300 triangles per game frame touch the clip volume and went through the CPU
      clipper, at 4.2 µs each. 86% of them came out empty.
    - The early reject follows the clipper's own planes and distances.
    - PC: 0 mismatches in 415,222 early rejects; identical screenshots.
    - Vita: `worldClip` 15-16 to 7-8 ms per game frame. Frame rate is unchanged within the
      spread (3.74 mean both ways), so no speedup is claimed.
    - Profiling behind it: [PERF_WORK_NOT_DONE.md](PERF_WORK_NOT_DONE.md), "Flush and world
      draw submission, split".
    - What the GS thread spends per draw: vitaGL takes about 25 µs per draw call even for VBO
      draws, so about 790 draws per game frame cost about 26 ms there.
18. **Fewer draws: world batches with the open slot's constants share its draw
    (`world_share_slots`), and full-screen sprites redrawn per scissor column are one draw
    (`strip_merge`). Both on, both exact on the PC.**
    - World slot draws −56%; fixed-function draws −21% (PC counts).
    - Vita: `world_share_slots` alone measured 3.91/4.00/3.81 against 3.55/3.66/3.73 (+7.1%);
      both together 3.78/3.87 against 3.57/3.64/3.55 (+6.6%).
    - Details: [PERF_WORK_NOT_DONE.md](PERF_WORK_NOT_DONE.md), "Fewer draws".
19. **Render target pool fixed (`target_pool_lru`, on).**
    - The pool held 18.6 MB of menu-sized targets forever, while the target RE4 shrinks and
      grows back every frame was created anew each time: about 2 new render targets per game
      frame, with vitaGL's free memory sinking all run.
    - Now: oldest-first eviction, an age limit, shrinks go to the pool, and reuse only from a
      previous frame.
    - Vita: 4.09/3.94/4.07 against 3.72/3.84/3.67 steady game fps (+7.7%). Free GL memory
      stays about 48 MB instead of sinking to about 30 MB.
    - This is the likely cause of the vitaGL texture-allocation crashes; none in 12 runs since.
20. **Render target uploads drawn from a scratch texture (`gpu_upload`, on).**
    - vitaGL copied the whole target texture (1 MB) on every glTexSubImage2D into a target
      drawn recently.
    - Vita: 4.17/4.14/4.10 against 3.86/3.91/3.94 steady game fps (+6.0%).
21. **GS thread cuts of 26 Nov (each with its own switch, all exact on the PC).** Measured on the
    Vita as section time; the frame rate did not move beyond the spread (the emulation thread is
    the limit), so no speedup is claimed.
    - `alpha_clut_reuse`: RE4 grades its colours in 16 strips, each binding the same alpha-index
      texture of the frame; the GPU pass that converts it now runs once per frame and the other
      binds reuse its result (1,057 of 1,208 passes on the PC). `alphaClut` 5 to under 1 ms.
    - `batch_pool`: world and skinning batches go back to a pool after their draw instead of
      being freed on the GS thread (99% reused). Freeing them took 7-8 ms; now 2.
    - `skin_prefetch`: the projected skinning loop prefetches its source (written by the VU1
      worker on another core) and destination 8 vertices ahead. `skinLoop` 10-12 to 7 ms.
    - `mailbox_batch` (patch 0129): the GS thread takes all queued calls under one lock, and
      senders wake it only when it waits. `other` about 1.5 ms lower.
    - `clut_hash_on_load`: the 1 KB CLUT is hashed only when TEX0 loads it (and on reset and
      state loads), not after every TEX0 write.
    - `end_on_worker`: a world batch's last two vertices are projected where the batch is built.
    - World vertex and index arrays are no longer zeroed before being written.
    - `solid_readback` / `solid_upload` / `strip_merge_marks`: see "Tried, measured, and not
      kept": exact by construction, but they do not apply on the Vita.

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
| Texture page hashes taken right after each transfer (`page_hash_on_write`) | Exact; texHash 6-7 to 17 ms on the Vita | Twice the pages hashed, and the data was not in cache as hoped; a per-page hash cache alone was reused 2% of the time. Removed |
| Read-backs of solid-filled targets written from the fill colour (`solid_readback`, `solid_upload`) | Exact on the PC: 0x310000's 512x448 read-back each frame skipped (read-backs 23 to 13 ms on the Vita in the first, wrong version) | On the Vita the target is not one colour when read after the GPU finishes, so the colour is never learned and nothing is skipped. Kept on: it only acts on a whole read-back found uniform |
| World vertices written into vitaGL stream buffer memory (`world_scratch`) | Exact on the PC (not used there) | Crashed the Vita in vitaGL's draws (pointed buffer), then hung it (stream buffer). Off |

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

Ideas taken from VitaSX2 (a separate Vita PS2 emulator built on PCSX2, GPL-3.0: read for ideas only,
nothing copied; details and sources in [PERF_WORK_NOT_DONE.md](PERF_WORK_NOT_DONE.md), "Ideas from
VitaSX2"). None tested here yet.

| Idea | Thread / section it targets | Why it looks promising |
|---|---|---|
| EE wait-loop fast-forward in closed form: loops that poll GS CSR VSINT, a DMAC channel's CHCR.STR, a RAM flag, or count down are jumped straight to the next scheduler event (cycles computed, not spun) | Emulation thread, EE game code (93 ms) and EE I/O | Exact if done at event seams; we have idle-loop and thread-lock HLE, but not per-poll-shape skipping. Check first how much RE4 time sits in such loops (eehot.txt) |
| EE JIT: guest registers kept in host registers across linked blocks, with a "compatible entry" per link signature, dirty write-back at block end (VitaSX2 pins 5 GPR low words; a Q-register cache of 8 GPRs) | EE game code | Our hot code spends 10-12% of instructions on guest register loads/stores; adds the mechanism to the existing EE JIT rework item |
| EE JIT: compare consumed only by its branch keeps the result in flags; 64-bit high words rematerialized from a sign/zero-extension fact instead of stored | EE game code | Cuts instructions per EE instruction (ours: 23) without fastmem |
| EE JIT: loop-level memory preflight - an affine address range proven RAM (and free of compiled code) once at loop entry, then loads/stores in the body skip the per-access VTLB and code-overwrite checks | EE game code | The Vita has no page faults, so per-access checks are the price of every load; VitaSX2 reports its first such loop >=2x |
| EE JIT: exact loop-shape recognizers for zero-fill, word-fill, word-copy and scan loops compiled as bulk operations inside the block (not function overlays) | EE game code | Our overlays of whole library functions lost to call overhead; in-block folding has none |
| EE JIT: code-overwrite detection by a byte per 4 KB page plus a bit per 64-byte chunk | EE game code (stores) | Cheaper than checking whole pages for every store |
| EE MMI (128-bit) ops as one NEON instruction per lane-wise op | EE game code | Check what CodeGen emits for RE4's MMI use |
| VIF1: adjacent V4-32 unpack records merged in place before the worker can see them | Emulation thread (VIFqueue 12, VIFcopy 20), worker unpack | **Built in our form (`vif_merge`, patch 0133, off until a Vita A/B):** our records are per DMA transfer, and every chain tag (TTE) was a 16-byte record of its own before its data. The tag is now held unpublished and its data joins it: records 2.36 to 1.39 million in the PC walk (-41%), screenshots identical |
| VIF1: the EE does not wait for the VU1 worker except when it reads VU state; VIF1's finish is scheduled from a cycle estimate | Emulation thread, VU1 wait (32 ms) | Our per-vblank wait off moved the time to the GS wait; VitaSX2 removes waits at their source instead. Needs care with RE4's sync points |
| VU JIT: all 16 NEON Q registers (D16-D31 reloaded only after helper calls), MAC/STATUS flag liveness so unread flags are not computed, flags held in registers until the block end, per-lane normalization tracking | VU1 worker (VU1 JIT 14 ms; other games much more) | Our allocator has 8 NEON registers and that was the wall (PERF_WORK_NOT_DONE.md, V2) |
| GS: render targets kept on the GPU between uses with versioned "needs store" flags; read-backs through the GPU copy engine with a completion fence (not glReadPixels) | GS thread, read-backs (13-23 ms) | Also fixes the stale vitaGL read-back bug (26 Nov); needs our own GXM calls next to vitaGL or the W6 GXM path |
| GS: ~40 shader variants compiled offline, chosen per draw by exactness predicates; blending through the shader patcher's fixed-function blend where it is exact | GS thread, draw cost | Part of W6; shows how to keep GXM draws cheap |
| GS mailbox: entries carry pointers to the packet data (no payload copies), adjacent PATH1 reservations merged, counters published once per vsync | Emulation thread relay, GS "other" | Matches the DIRECT no-copy work started on 26 Nov |
| GPU-VU (VU1 programs on the GPU): lesson only | - | Their own measurements: GXM jobs (about 120 a second) dominate, small batches are slower than the CPU; only one SDK sample gained (2 to 8 fps). Not worth it for RE4 beyond the vertex shaders we already use |

## Why 30 is far away

- **All three threads need cutting together.** Even with no waiting at all, the emulation
  thread needs ~230 ms per game frame (about 4.3 fps). At 30 fps each thread has 33 ms.
- **The emulation thread is about 7x over budget on its own work.** RE4's game code alone is
  ~90 ms per game frame, about 2.7x the whole budget, and its profile is flat.
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
