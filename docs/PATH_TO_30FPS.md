# Resident Evil 4: what it takes to reach a locked 30

_Written by Fable 5.2 on 2026-11-16 from the repository at `20875f2`, after reviewing [PERF_WORK_NOT_DONE.md](PERF_WORK_NOT_DONE.md). Nothing here was run on a Vita and nothing was built. Figures called "measured" are the sessions' own; everything called an estimate is mine._

**Gate results (16 Nov, on the Vita; details in [PERF_WORK_NOT_DONE.md](PERF_WORK_NOT_DONE.md), "Gates G0-G3"):** G0: per game frame the GPU thread uses about 470 ms of CPU, the emulation thread about 300 ms, VU1 about 245 ms. G1: the ceiling is about 3.2 game fps (1.9x today), with the emulation thread the wall, so the EE work is as urgent as the renderer. G2: EE game time is spread: top 20 functions 53% of game code, top 50 76%. G3: 93.6% of VIF1 bytes repeat at the same address with the same data from one game frame to the next, so W2 can key by address. Also measured: one world batch per draw costs about 5% in totals, not a quarter (steady rates equal).

## The answer

The steps in the review (second half of this file) do not reach 30. They remove the renderer's wall, which is necessary. By my estimate they leave the game somewhere near 10 frames a second, because the emulation thread and the VU1 thread are also several times over their budgets.

A locked 30 needs three things together. Each one removes work the Vita cannot afford, instead of making that work faster:

1. **Geometry stays on the GPU.** World and character vertex data is uploaded once and drawn from there. No thread touches a vertex on a normal frame.
2. **The game's hottest functions run as native code.** The EE recompiler cannot bring about 100 ms of game code per frame down to about 20 ms. Native replacements of the top functions, checked automatically against the recompiler, can.
3. **Draws go to the GPU through GXM directly** once geometry is retained, because a draw through vitaGL costs about 20 µs today (my arithmetic, in W6).

Two of the three are unproven here: whether RE4's EE time is concentrated in few enough functions (gate G2), and whether the GPU and the draw path keep up at full rate (gate G4). Both gates are cheap. Run them before the long work, and stop to re-plan if either fails. If the EE cannot reach 30 updates a second, there is no way to a locked 30 at correct game speed, and the fallback section says what is still reachable.

## The budget

At 30 frames a second each thread has 33 ms per game frame: 14.8 million CPU cycles on a 444 MHz core.

| Thread | Budget | Today on the Vita | Target |
|---|---|---|---|
| GPU thread (core 1) | 33 ms | About 530 ms: 948 ms a second in the pump at about 1.8 game fps (measured 16 Nov) | 25 ms |
| Emulation thread (core 0) | 33 ms | About 437 ms at 2.28 game fps (measured 17 Nov, E1 below): game code 85, waiting for VU1 97, queueing VIF1 data 66, relaying VU1 output 64, waiting for the GS thread 59. Not the limit: it waits 150 ms or more of each game frame | 28 ms |
| VU1 thread (core 2) | 33 ms | About 380 ms busy of 437 (measured 17 Nov): programs 299 (skinning 91% of the cycles), unpack 63, DIRECT 20; idle 57. With native skinning (W3, 23 Nov): skinning 70, idle 85 of ~420 | 25 ms |
| Core 3 | Shared with the system | Audio output and the profiler | Background checks |

What one game frame carries at the gameplay start (from the docs' counts):

- About 160,000 world vertices in 1,800 runs of the world program (790 runs and 70,000 vertices per vblank, 2.3 vblanks per game frame).
- About 3 MB of VIF1 data in about 6,900 DMA transfers (1.2 GB and 2.7 million transfers per 393 game frames).
- About 130 small textures streamed through the same GS memory slots.
- VU1 cycles: the world loop about 63%, skinning about 24%, everything else 13% (my arithmetic from the 10 Nov statistics).

The targets leave headroom on purpose. "Locked" means the slowest frame fits, not the average one.

**Done** means `game_fps` holds 30 over the scripted walk with the frame limiter on, each thread is under its target, and the audio does not underrun.

## Gates: measure before building

Each gate is hours of work. G0 to G3 come first; G4 and G5 follow the first work packages.

| Gate | What to run | What it decides |
|---|---|---|
| G0 | CPU milliseconds per game frame for the three threads, with RE4's profile. The `threads` column and `game_fps` give it. Also compare the GPU thread's CPU share with `gpu_pump_ms`, which is elapsed time | The real size of each gap. If the GPU thread's CPU share is well below 95%, part of its time is waiting inside vitaGL, not work |
| G1 | A ceiling run: a switch that makes the GPU thread drop every draw and transfer | The frame rate the EE, IOP and VU1 allow today. If it is under 5, the EE work below is as urgent as the renderer and should start at once |
| G2 | `ee_hot_after` in gameplay on the Vita, with the addresses grouped by RE4's symbol table | Three numbers: the share of EE time in the top 20 functions, the share inside the game's draw phase, and the share in VU0 (COP2) code. They decide E3, E5 and E4 |
| G3 | A log of the EE source address and length of each VIF1 transfer for two frames in a row | Whether geometry can be keyed by address (W2). If the addresses do not repeat, key by a hash of the packed data |
| G4 | After W2: replay one frame's retained draws with nothing else running, as fast as they go | Whether the GPU and the draw path manage 30. Under 40 replays a second means W6 is needed before anything else |
| G5 | R2's experiment on the PC runner: skip the game's draw phase on odd ticks | Whether the fallback exists |

**How to read G2.** Assume an overlay runs ten times faster than recompiled code (my assumption) and game code is 100 ms per game frame today (my estimate). Overlays covering 70% of EE time then leave about 37 ms, 85% leaves about 24, and 90% leaves about 19. The budget needs about 90%. If a workable number of functions cannot cover that, a true 30 depends on R2 as well.

## Work packages

Each package names the budget line it serves, the place in the code, and the test that says it is done. "All games" marks work that helps the whole library.

### GPU thread: from about 530 ms to 25 ms

**W1. No round trips in a steady frame (all games).**

- **Why:** target uploads, read-backs, new targets and relayouts are 297 of the thread's 948 ms a second (measured).
- **How:**
  - Pool the target objects. `AllocateTargetGl` (`src/gs/hw/GSH_Hardware.cpp:758`) builds a texture, a depth buffer and a framebuffer for every new target, about 19 ms each.
  - List the passes that still cause the two uploads and two read-backs per frame with `gs_diag`.
  - Remove each one: a skip rule in the profile if it is an effect, or a conversion on the GPU. `CanRelayout` (`GSH_Hardware.cpp:650`) refuses a 16-bit view of a 32-bit buffer today.
  - If the depth blur is wanted back later, vitaGL can expose a framebuffer's depth as a texture (`vglTexImageDepthBuffer`, read in vitaGL's source, untested here).
- **Done when:** `gpu_rt_up`, `gpu_rt_dl` and `f_targets_new` are 0 in steady gameplay.

**W2. World geometry stays on the GPU.** This is the largest single step for all three threads.

- **Why:** each world vertex is copied or converted at least five times per frame on three threads: the copy into the P1 queue (`CVpu::QueueVifData`), the unpack, the copy into `WORLD_BATCH::raw` (`src/gs/Re4Vu1Hle.cpp`), the conversion in `DrawWorldBatch` (`GSH_Hardware.cpp:2267`), and vitaGL's copy at the draw.
- **Step a, the shader:**
  - Give the GPU homogeneous positions so that it clips. The 4096-pixel wrap becomes a constant offset folded into the matrix, chosen per batch, and a sign flip makes w positive. The CPU's clip-matrix pass over every vertex goes away.
  - Write the shader in Cg, not GLSL. vitaGL treats `GL_CG_VERTEX_SHADER_EXT` and `GL_CG_FRAGMENT_SHADER_EXT` shaders as Cg and sends every other shader through its GLSL translator (`source/custom_shaders.c` in vitaGL). The `bool` crash was in that translator, and the slot-index fault probably is. Test the 16-slot shader again as Cg.
  - A vertex buffer then depends only on the vertex data, not on the camera.
- **Step b, the cache:**
  - Static buffer objects, one range per program run, with the triangle indices built once. The winding sign is a per-draw culling choice.
  - Key a run by where its data comes from: the EE addresses and lengths of the transfers that feed it (gate G3), with a few sampled words as a quick check.
- **Step c, lazy unpack in the P1 worker:**
  - The emulation thread queues a reference (address, length, sampled words) for a transfer it has seen before with the same words, and the bytes otherwise.
  - The worker decodes UNPACK commands but does not copy yet. When the next program start is a replaced program and every pending source is in the cache, it emits "draw cached range N with these constants". Otherwise it performs the pending unpacks and runs as today.
  - This assumes no later program reads the data a skipped unpack would have left in VU1 memory. Keep it behind a per-game setting and compare pictures with it off.
- **Step d, the background check:** a low-priority thread on core 3 re-hashes the source of every cached range in turn and drops an entry that changed. Write protection through kubridge (C1) is the stricter alternative.
- **Done when:**
  - VIF1 queue bytes per game frame fall from 3 MB to the constants.
  - The per-vertex loop in `DrawWorldBatch` is gone.
  - Pictures match the CPU path within the existing difference test.
  - Free memory (`heap_free_mb` and vitaGL's pools) stays above a floor you set, with least-recently-used eviction.

**W3. Characters the same way.** The skinning program (0x0498, 0x0000 and 0x0050 into 0x01D0) becomes a vertex shader in Cg with the bone matrices as a uniform array, and its vertex data is cached like the world's. Its lighting has not been analysed, so the size of this job is unknown. Done when the pictures match the recompiler's and the program no longer runs on VU1.

- **First step done (23 Nov): the skinning programs run as native code, translated ahead of time.** `tools/vu_aot.py` reads the PC runner's VU1 micro memory disassembly (`--vu1-stats` writes `vu1micro.txt`) and writes C++ for the pairs reachable from given entries: one block per instruction pair, the upper result committed after the lower, delay slots inlined, Play's semantics for CLIP, MAX/MINI, FTOI, DIV and the 32-bit VI registers (`src/common/VuAot.h`). It stops on anything it does not know. `src/common/Re4Skin.gen.cpp` is its output for 0x0000-0x07D0 (entries 0x0000, 0x0050, 0x0498, 0x07C8). `Re4Skin::Run` uses it only while micro memory holds exactly that code (`CVpu::g_nativeProgram`, patch 0105); setting `re4_skin` (on in RE4's profile).
- **Checked:** PC runner `--skin-verify` runs each program in the JIT and natively from the same state and compares VU memory, VF, VI, ACC, Q and clip flags. 300 vblanks of gameplay: 68,269 programs, **no difference**. (The first pass had 192: with a NaN, the x86 JIT's "greater" compare is CMPNLEPS, true for NaN, ARM's VCGT is false; `VuClipGreater` follows each.) Threaded and asynchronous on the PC, the six screenshots are identical to the JIT's.
- **On the Vita:** skinning costs the VU1 worker 70 ms per game frame (it was most of 299 ms) and the worker is idle 85 ms. The game did not get faster (206-218 game frames in 120 s, 219-227 before): the emulation thread then waited 141 ms per game frame on the GS thread, once per frame, in `CGSHandler::Finish`.
- **Then: four vblanks of lead (`gs_frames_ahead`, patch 0106).** The emulation thread could run only 2 vblanks ahead of the GS thread, less than one RE4 game frame (2.3 vblanks), while the GS thread sat idle 89 ms per game frame. Each vblank in flight has a register write buffer; they were 20 MB each (0x140000 entries) and the heap had 2.7 MB free, so they are 8 MB now (most writes in one vblank measured: 118,000 of 524,288; overflows are counted, `gs_wb_peak`, `gs_wb_overflows`). Same build, 120 s: lead 2 213 game frames, lead 4 230 and 234; with RE4's profile 235. Free heap 6-8 MB. The wait on the GS thread fell to 59 ms per game frame.
- **Where that leaves the threads (ms per game frame, ~420):** emulation thread: game code 89, queueing VIF1 data 74, relaying VU1 output 73, waiting for VU1 46, for the GS thread 59. VU1 worker: native skinning 70, unpack 77, other worker work 144, idle 85. So E1 (the relay onto the worker) fits now, and the queue copy is the other large slice.

**W4. Streamed textures by payload hash (all games; G5 in the list).** Record a whole-rectangle transfer as "this region holds the payload with hash H". Write emulated GS memory only when something reads those bytes another way. A texture lookup that matches the region goes straight to the content cache. Today each of about 130 textures per frame is written a nibble at a time (`Source/gs/GsTransferWrite.h`). Done when a steady frame writes no texture bytes into GS memory.

- **23 Nov: the writes themselves made cheaper instead (patch 0108).** PSMT4 blocks through NEON table lookups, PSMCT32 blocks 8 bytes at a time; exact (tests, a Vita self-test, identical PC screenshots). Image data 57 -> 42 ms per game frame on the GS thread; frame rate inside the noise (238-255 game frames per 120 s, 239-268 before). The next GS thread items by size: world 53-60, flush 40-45, read-backs 30-34 (RE4's depth drawn as 16-bit and read as alpha indices, then textures streamed into the same buffer: needs dirty areas by block; see PERF_WORK_NOT_DONE.md, "The GS thread after W3").
- **Then (23 Nov): read-backs and uploads a GS block at a time, and world draws merged** (consecutive retained batches with the same uniforms as one draw, from shared buffers made once a frame). Same build A/B: 263/265 against 255/258 game frames per 120 s; the emulation thread's wait on the GS thread 15-21 ms per game frame against 53-60. The GS thread has stopped being the main limit: the emulation thread (game code 88, VIF1 queueing ~70, relay ~76, VU1 wait ~40) and the VU1 worker (JIT programs ~157 ms besides skinning) are.

**W5. One queue record per batch, and no relay (all games).**

- **Why:** every hand-off to the GPU thread is a `std::function`, a mutex and a `notify_all` (`Source/MailBox.cpp`). The world path alone makes 1,800 to 3,600 per game frame, and each is first relayed through the emulation thread (E1 below).
- **How:** the VU1 worker collects its batches and submits them in blocks, straight to the GPU thread's queue. The GPU thread spins briefly before it sleeps.
- **Done when:** mailbox calls per game frame fall by a factor of ten or more, and the emulation thread's relay time is gone (E1).

**W6. The cost of a draw.**

- **Why:** a draw through vitaGL costs about 20 µs today by my arithmetic: 97 ms a second over roughly 4,500 draws, where the draw count is my estimate. At 30 fps, 1,800 runs drawn one by one would take 36 ms, more than the whole budget.
- **How, in order:**
  1. Merge consecutive runs whose constants are bit-identical into one draw.
  2. Measure again with W2's static buffers, which remove vitaGL's vertex copy.
  3. If a draw is still above 10 µs, add a fast path to a fork of vitaGL: bind a compiled program, the vertex streams and a uniform block, then `sceGxmDraw`, with none of its per-draw state checks. G9 in the list already suggests building vitaGL from source.
- **Done when:** gate G4 replays the frame at 40 a second or more.

**W7. The generic path for what is left (all games; G2 in the list).** After W2 and W3 only the HUD, effects and particles go through the register path. Recognise the common packed register lists and convert a whole run in one loop. Do this last, and only if G0 still shows it.

### VU1 thread: to 25 ms

- **V-a. No unpack for cached geometry.** This is W2 step c.
- **V-b. No skinning on VU1.** This is W3. At the measured cost per VU cycle, skinning alone needs more than three cores at full speed (my estimate, assuming it costs what the world loop did).
- **V-c. The remaining 13% of VU1 cycles.** At today's cost they still need two to three cores at full speed (same estimate). Either replace the next two programs (0x07C8 and 0x3008) the same way, or build the new recompiler (V2 in the list). The doc's own allocator experiments show the existing recompiler cannot be pushed further. V2 is the choice that helps all games.
- **V-d. Parse VU1's own GIF output on this thread.** See E1.

### Emulation thread: to 28 ms

**E1. Stop relaying VU1's output (all games).**

- **Why:** with asynchronous VIF1 the worker's output still returns to the emulation thread. `CVpu::FlushPendingXgKicks` (`Source/ee/Vpu.cpp`, patch 0097) parses every XGKICK and VIF1 DIRECT packet there and submits every world batch there. `CVpu::QueueVifData` also takes a mutex and copies the bytes for each of about 6,900 transfers per game frame.
- **How:**
  - The worker parses its own PATH1 and PATH2 packets and submits to the GS queue directly.
  - Order is already protected by an existing rule: a GIF transfer (PATH3) waits until the worker is quiet. Add one rule: the emulation thread flushes its GS write buffer before it queues VIF1 data.
  - SIGNAL and the case of another path in the middle of a packet keep today's logic, with the roles swapped: the worker waits and the emulation thread wakes it.
  - Measure first. The emulation profiler's sections show how large the relay is; do this only if it is a large slice.
- **Done when:** the emulation thread spends no time in `FlushPendingXgKicks`.
- **Measured first (17 Nov), and not done as written.** The VU1 worker never set its own profiler section, so its unpack and program time showed as the emulation thread's (`ps2_VIF`, `ps2_VIFunpack`, `ps2_VU1`): the "half of the thread relays VU1 and VIF1 output" figure came from that. Patch 0103 gives the worker its own section (perf.csv `vu1_sections`, EmuProfile order) and splits the relay (`ps2_VU1relay`) and the queue copy (`ps2_VIFqueue`) out. RE4 gameplay, 120 s, ms per game frame:

  | | Before 0104 | With 0104 |
  |---|---|---|
  | Game fps | 2.29 | 2.28 |
  | EE: queueing VIF1 data | 100 | 66 |
  | EE: relaying VU1 output | 53 | 64 |
  | EE: waiting for VU1 | 32 | 97 |
  | EE: waiting for the GS thread | 110 | 59 |
  | EE: game code | 81 | 85 |
  | VU1 worker: programs / unpack / idle | 229 / 51 / 140 | 299 / 63 / 57 |

  Patch 0104 signals the worker's semaphore only when it is about to sleep (it was a system call per queued VIF1 packet). The emulation thread saves 34 ms of queueing per game frame and spends it waiting: it is not the limit. The worker is: with `vu1_stats=1`, the skinning programs (0x0498, 0x0000, 0x0050) are 91% of VU1's cycles (the world program runs as HLE). Moving the relay onto that thread, as E1 says, would add 64 ms to a thread with about 57 ms idle. **Next is W3 (skinning off VU1)**; E1 comes back once the worker has room, or the relay goes to the GS thread with W5.
- **Tried after W3 (23 Nov), and not kept.** With native skinning the worker had room, so it decoded each XGKICK packet into GS register writes (`CGIF::DecodePath1Packet`, the same expressions as `ProcessPacked`/`ProcessRegList`; SIGNAL/FINISH/LABEL, transfer registers, HWREG and IMAGE left to the raw parse) and the emulation thread only appended them. Exact: on the PC 70,930 packets decoded, all 70,930 identical to the raw parse's writes, screenshots identical. But on the Vita (same build, 120 s): 228 game frames with it, 242 without. The emulation thread's relay did not shrink (62 -> 63 ms per game frame) and decoding cost the worker 59 ms (a vector of 16-byte writes per packet), so the emulation thread waited on VU1 89 ms instead of 35. The patch is kept out of the series.
- **What the relay is (patch 0107, sections `relayDirect` and `relayWorld`):** per game frame about 50 ms: VIF1 DIRECT data 21, PATH1 packets 19, RE4 world batches 10. No part is large enough to move on its own. Run to run, the same build gives 239-268 game frames in 120 s (about +-5%), so a change needs several runs.
- **The emulation thread now (ms per game frame, ~410):** game code 83, waiting for the GS thread 78, queueing VIF1 data 53, the relay 50, waiting for VU1 24. Queueing (a mutex and a copy per DMA transfer, `CVpu::QueueVifData`) is the largest overhead left there, but while the thread waits 78 ms on the GS thread, the GS thread is the limit again.

**E2. The IOP: shrink it where it is.** The IOP thread on core 3 lost 12% in gameplay (measured 16 Nov, commit `20875f2`), so that is not the way. Its cost at full speed is about 12–15 ms per game frame (my estimate from the movie figures). The remedy is R3 in the list: native replacements for the game's own copies of SIO2MAN and PADMAN (16% of IOP time) and libsd (7%), and Capcom's sound mixer (37%). Those shares were measured in the intro movie; take the IOP profile in gameplay before starting.

**E3. Native overlays for the game's hottest functions.** This is the step that can close the EE's gap, and it is new for this project.

- **What:** a table keyed by game serial, function address and a hash of the function's code. Each entry is a native C++ function that does what the game function does, on emulated memory.
- **Why it can work here:**
  - RE4 keeps its symbol table in memory (5,602 names, `docs/RE4.md`), so the profile reads as function names.
  - The project already replaces game code this way for movies: `src/common/MpegHle.cpp` patches a stub that makes an HLE syscall (`CPS2OS::SetHleHooks`, patch 0060).
  - Writing a native equivalent from a disassembly is work an AI session does quickly. What it cannot do unaided is prove the result right, which the harness below does.
- **Two ways in:**
  - For large functions called up to a few thousand times a frame, the existing syscall stub. Patch 0060 reserves 16 numbers and most are taken, so use one number and find the overlay by the caller's address.
  - For small hot leaf functions, a check where a block is compiled: a block that starts at an overlay's address compiles to one native call and a return. A syscall per call would cost more than these functions do.
- **The harness (PC runner), which makes this safe:**
  - At an overlay's entry, save the registers and write-protect EE memory.
  - Run the recompiled original, and record the pages it wrote and the registers it left.
  - Restore, run the overlay, and compare pages and registers. Report the first difference with the inputs that caused it.
  - Copying and comparing all 32 MB on sampled calls is the simpler form if page protection is awkward under WSL.
  - Run it over a recorded gameplay session for every overlay before it is switched on.
- **Order of work:** by G2's ranking, top first, until the emulation thread meets its budget. Sony library functions that show up (`sceVu0*` matrix code, memory copies) are keyed by code hash alone and serve every game: that is C3 in the list.
- **Done when:** the game code share of the emulation thread is under 20 ms per game frame, and the harness reports no differences.

**E4. The rest of the EE profile (all games).** C2 (COP2 flag work only where flags are read) if G2 shows time in VU0 code. Then tier 2's remaining gaps. The on-device optimizing compile (B1) only if overlays and these leave a gap.

**E5. R2, if the profile says the draw phase can be separated.** Running the game's draw phase on every second tick halves that part of the EE's work and all of the VU1 and GPU work for the skipped frames. Gate G5 tests it on the PC first.

## Order, and what can run side by side

1. **Gates G0 to G3.** One day.
2. **W1 and W5, and the merge step of W6.** Days. They are contained, and they give the first visible gain.
3. **W2**, then gate **G4.** One to two weeks.
4. **W3 and W4.**
5. **W6's fast path** if G4 asks for it, then **W7**.

The EE work does not touch the renderer. A second session in its own worktree, as the movie and gameplay sessions were run before, can start **E3's harness** right after G2 and work down the ranking while the renderer work proceeds. **E1** belongs to whoever owns patch 0097. **E2** and **V-c** can wait until G0 shows them at the top of their thread.

## If a true 30 is out of reach: 30 on screen

Retained geometry makes in-between frames cheap. A frame between two game frames is the same cached buffers drawn again with interpolated matrices. It costs GPU thread time only, and no EE or VU1 work.

- **If the EE manages 30 updates a second but not 30 draw phases** (R2 works): draw 15 game frames and 15 in-between frames. The screen shows 30 and the game runs at the right speed.
- **If the EE cannot manage 30 updates a second:** the game runs slow whatever the screen shows. Nothing in this plan fixes that except more of E3.

## What is proven and what is not

| Claim | State |
|---|---|
| The renderer's measured parts and their sizes | Measured by the sessions on the Vita |
| The emulation thread is 150–250 ms per game frame | My estimate. G0 measures it |
| RE4's EE time is concentrated enough for overlays | Unknown. G2 measures it |
| World vertex data arrives from the same addresses each frame | Unknown. G3 measures it. The game cannot copy 3 MB a frame at 30 fps on a real PS2, so referenced static data is likely |
| Skipped unpacks are never read by a later program | An assumption, behind a setting, checked by comparing pictures |
| Shaders in Cg avoid the translator faults | vitaGL's source shows Cg shaders skip the translator. That this cures the slot index is untested |
| The GPU and draw path manage the frame at 30 | Unknown. G4 measures it |
| The game's draw phase can be skipped on odd ticks | Unknown. G5 tests it |
| The skinning program fits a vertex shader | Likely, not analysed |

## Working rules for this stretch

- **Count game frames against the three thread budgets**, in one table at the top of [PERF_WORK_NOT_DONE.md](PERF_WORK_NOT_DONE.md), updated with every result.
- **Build the replay benchmark early (T4).** Runs of 120 s that spread from 1.5 to 2.1 fps cannot resolve a gain under 15%. A recorded frame replayed on the Vita can, in seconds.
- **Hardware counters need no kernel code.** The SDK has `psp2/perf.h`: `scePerfArmPmonSelectEvent`, `scePerfArmPmonStart` and `scePerfArmPmonGetCounterValue` per thread, with cycle, cache-miss and stall events. Whether retail firmware lets a homebrew app use it is untested; one call and its return value answers that.
- **Every shortcut that assumes something about the game sits behind a profile setting** and has a comparison run with it off.
- **Park these:** more G1 work through vitaGL (measured as no gain), G4 (0.2% for RE4), more core 3 tuning, the IOP thread in gameplay (measured −12%), more work on the existing VU register allocator, and B6's 500 MHz test.

---

## Review of the first two days of results (16 November)

This is the evidence behind the plan above. I read `docs/PERF_WORK_NOT_DONE.md` at `76024cb` and the code behind its numbers. The work took RE4 gameplay from about 0.7 to 1.5–2.1 game frames a second. The measurements also show that my estimates for several renderer items were too high, so the 8–15 frames a second I projected will not come from the remaining items as written.

### What landed, against what I expected

| Item | What I expected | Measured on the Vita |
|---|---|---|
| P1 asynchronous VU1 | The largest general gain | 0.6–0.8 → 1.0–1.4 game fps |
| V1 world on the GPU, first half | Large for RE4 | 1.0–1.3 → 1.6–2.0 game fps |
| R1 depth-blur skip | A large share of the post-processing cost | Small: 115 → 130 game frames per run. Two target uploads, two read-backs and two new targets per frame remain |
| G1 own shaders and vertex ring | A clear gain | None: vitaGL's fixed-function path was not the cost |
| G4 palette textures | Useful | Measured before building: 0.2% for RE4. Dropping it was right |
| P3 fourth core | Useful for the light threads | Within noise in gameplay, where one thread is the wall |
| V2 steps in the existing recompiler | Small; the new recompiler is what matters | 29% fewer instructions in the world loop, which RE4's profile no longer runs on VU1. No Vita figure |

### What the numbers say

1. **The GPU thread needs about 16 times less time per game frame.** It spends 948 ms of every second in the pump at about 1.8 game fps. That is about 530 ms per game frame, against 33 ms at 30 fps.
2. **The measured parts cannot get there.** They add up to 414 ms a second. With all of them at zero the thread would be 1.8 times faster. The other 534 ms a second is in no timer.
3. **`gpu_pump_ms` is elapsed time, not CPU time** (`CGSH_Hardware::Pump`, `src/gs/hw/GSH_Hardware.cpp:282`). The doc calls the thread "busy about 95% of the time"; the `threads` column says whether that is work or waiting.
4. **The round trips are one mechanism, not five parts.** Target uploads, read-backs, new targets and relayouts all come from the game reusing a buffer under another width or format.
5. **The emulation and VU1 threads have no figures in the doc.**

Where the unattributed half probably goes. This is from reading the code and the docs' counts; the sizes are my estimates and need timers to confirm.

| Suspect | Why | Rough size |
|---|---|---|
| The world path outside its draw call | The Vita default is one GPU draw per program run. Each vertex is copied into the batch, transformed by the clip matrix on the CPU to find about 150 triangles that need clipping, converted from 12 integers to 13 floats, then copied again by vitaGL. Each run also costs two heap blocks and one or two mailbox calls | 150–220 ms/s |
| Textures the game streams every frame | About 130 small textures per frame are still written into emulated GS memory a nibble at a time before the content cache hits | 45–90 ms/s |
| Everything else | Register processing for characters, effects and the HUD, and the mailbox | The rest |

Timers around the world vertex loop (`DrawWorldBatch`), the transfer write (`FeedImageDataImpl`), `WriteRegisterMassively` and the mailbox receive would settle it.

### Points to fix in PERF_WORK_NOT_DONE.md

- **`gpu_world_slots=1` is described as "the same speed".** The run totals next to it say 207 game frames before and 147–153 after, which suggests the one-batch-per-draw fix cost about a quarter of the speed. State both, or rerun the pair warm. Every draw also uploads all 16 slots (112 vectors) to use one.
- **"C2 needs the EE profile, which the PC runner cannot give."** The Vita's `ee_hot_after` gives it (gate G2).
- **"T2 and T3 are Vita-only" understates how close they are.**
  - T2 has an SDK interface (`psp2/perf.h`, above).
  - For T3, the app's kernel module can add CapUnlocker's third hook, the one it did not copy. That hook unlocks the thread suspend and context calls for apps (`sceKernelSuspendThreadForVM`, `sceKernelGetThreadContextForVM`).
  - The alternative is to sample inside the kernel module with `ksceKernelGetThreadCpuRegisters`. Both are untested.
- **Two V2 steps are marked "untested on ARM hardware" but are on by default,** and the T1 row says the CI logs were not checked. The Vita runs since then exercise them in RE4 only. Run VuTest on the Vita, or confirm the ARM job in CI is green.
- **The "IOP thread on core 3" row had no result at `76024cb`.** Commit `20875f2` filled it in.

### What I checked

- **Read in the repository:**
  - `docs/PERF_WORK_NOT_DONE.md` (to `20875f2`), `docs/RE4_GAMEPLAY.md` and the changes to `docs/RE4.md`.
  - `src/gs/hw/GSH_Hardware.cpp` (pump, targets, transfers, state, world path), `src/gs/Re4Vu1Hle.cpp`, `src/common/Re4Vu1Batch.h`, `src/common/MpegHle.cpp` (how it hooks game code), `src/vita/core3/core3.c`, `src/vita/Cores.h` and the main loop in `src/vita/main_vita.cpp`.
  - The Play tree with patches applied through 0100 except the CodeGen patch 0098: `Source/ee/Vpu.cpp`, `Source/ee/Vif1.cpp`, `Source/MailBox.cpp`, `Source/gs/GsTransferWrite.h`, and patch 0060.
- **Read outside it:**
  - vitaGL's source: `source/custom_shaders.c`, `source/framebuffers.c` and `source/vitaGL.h`.
  - The Vita SDK headers: `psp2/perf.h`, `psp2kern/kernel/threadmgr/debugger.h` and the function database.
  - CapUnlocker's `main.c`, which uses the same 0x114C offset as the new kernel module.
- **Not done:** nothing was run on a Vita and nothing was built. I could not see CI results from this session.
