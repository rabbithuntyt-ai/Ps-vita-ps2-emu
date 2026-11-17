# VitaStation2: performance work not yet done

_Written by Fable 5.2 on 2026-11-15 from the repository at f7ed38d. The original PDF is next to this file ([VitaStation2_performance_work_not_yet_done.pdf](VitaStation2_performance_work_not_yet_done.pdf)); this is its text, with the layout kept._

**Read first: [PATH_TO_30FPS.md](PATH_TO_30FPS.md)** (Fable 5.2, 2026-11-16). It reviews the results below and gives the plan to a locked 30: the budget per thread, the gates to measure first, and the work packages in order.

## Progress (updated as items land)

The "PC result" column is from the PC runner (RE4 gameplay state `state_b2`, 900 vblanks,
`runs/re4/hostab.sh`); on the PC the GS thread (OSMesa) sets the real-time speed, so the
per-thread CPU time per game frame is the useful figure there. Vita results are in the sections
after the table: "First Vita results" (15-16 Nov) and "Second Vita day" (16 Nov). Current
state on the Vita: RE4 gameplay 1.5-2.1 game fps with RE4's profile alone (was 0.6-0.8 on
15 Nov); the goal is a locked 30.

| Item | State | Setting | PC result |
|---|---|---|---|
| P1 asynchronous VU1 | Done (patch 0097); on in RE4's profile since 16 Nov (Vita-tested), off for other games | `vu1_async=1` with `threaded_vu1=1` (PC: `--mtvu --vu1-async`) | 393 game frames in 900 vblanks (threaded VU1: 349, no thread: 393). EE thread 65 → 46 ms per game frame, VU1 thread 26 → 19 ms. Pictures correct. 0 VIF1 stalls, 0 PATH3 syncs in gameplay |
| R1 RE4 depth-blur skip | Done; on in RE4's profile since 16 Nov | `re4_skip_dof=176` (PC: `--re4-skip-dof 176`) | Real time for the 900 vblanks 41.6 s -> 24.7 s with `vu1_async` (same 393 game frames); 202 draws skipped per game frame. The background loses its blur; the green line along the top edge goes too |
| V1 RE4 world on the GPU | First half done: `gpu_world` draws right, also on the Vita since the one-batch-per-draw fix (16 Nov); on in RE4's profile (retained geometry still to do) | `re4_vu1_hle=1 gpu_world=1` | 0.10-0.15% of pixels differ from the CPU path. All of P1 + R1 + V1 together: 393 game frames in 23.6 s (threaded VU1 alone: 349 in 38.5 s) |
| P3 fourth core | Done (16 Nov): the app loads its own kernel module, `src/vita/core3/core3.c` (the fourth-core hook of CapUnlocker only: SceKernelThreadMgr offset 0x114C, patched only after checking for a function prologue), from `ux0:app/<title>/vitaps2_core3.skprx`; it lasts until reboot and is never in the boot config. `ux0:data/VitaPS2/no_core3.txt` turns it off. Audio output and the profiler run on core 3 | perf.csv `core3` column; status in `ux0:data/VitaPS2/core3.txt` and `core3_app.txt` | RE4 gameplay, 120 s: no core 3 150 game frames; mixer and movie copy on core 3 143/143 (slower: core 3 is shared with the system); mixer on core 2, output and profiler on core 3 153/152/153 (the default). A small gain, near noise. Still to try: `iop_thread=1 iop_core=3` in movies |
| G4 palette textures | Not built: measured first (16 Nov) and not worth it for RE4 | — | In warm RE4 gameplay on the Vita, texture decoding is 2 ms per second of the GPU thread and about 1 texture upload per second: the texture and content caches already hit. G4 would save about 0.2% of the bottleneck thread. It may still matter for games whose CLUTs change often (not measured) |
| Small items 1 and 2 (GS thread) | Done, always on | — | `AddVertex`: reciprocals cached per target size, Z format and texture size (multiplies only where they give the divide's exact result: power of two sizes; the `textured clut` test catches the last-bit difference otherwise). A register write with the value the register already has no longer forces a new state (TEX0/TEX2/TEXCLUT/TEXFLUSH and the transfer registers always do). Gameplay pictures identical |
| G1 own shaders (phases 1-3 of 3 started) | Phase 1 and 2 done, phase 3 partly: off by default, untested on the Vita | `gs_shaders=1` (PC: `--gs-shaders`), `gs_vertex_ring` (on) | 1: one GLSL program does the texture combiner, fog and alpha test (no glTexEnv, no fixed-function shader rebuild per draw); GS tests pass both ways. 2: batch vertices in four 4 MB GPU buffers, one copy (vitaGL copied the interleaved array once per attribute per draw); 0 fallbacks in RE4. 3: strip triangles that continue a strip add one vertex and three indices (3.45 of 10.8 million in RE4) and strip vertices are converted once. Found on the way: XYZ2/XYZ3/XYZF2/XYZF3 marked the GS state dirty, so every triangle decoded and compared the whole state (all renderers, all games); fixed. RE4 pictures identical. Not done: raw GS vertex values with the arithmetic in the vertex shader |
| V2 VU recompiler | Investigated; first step done (patch 0099), the rest needs the new recompiler | `vu_vector_opmul` (on) | RE4's vertex loop (0x12B8-0x13E8): 1,427 ARM instructions for 39 pairs, ~42 of them vector arithmetic; ~330 are context loads/stores and their address adds. Two causes found with a new allocation dump (patch 0098, `JITTER_REGALLOC_DUMP=1`): OPMULA/OPMSUB read VF lanes as scalars, which marks those VF registers aliased (memory only) for the whole block; now done with lane broadcasts and masked writes (bit-identical pictures). Then the real wall: the allocator gives a symbol one register for its whole first-to-last-use interval and has 8 NEON registers (q8-q15; q4/q5 hold scalar floats, q6/q7 the clamp masks). With the VF registers allocatable they take all 8 and temporaries go through the stack: the loop stays at 1,427-1,436 instructions with any ordering (`JITTER_MD_KEEP_FREE=N` keeps N for temporaries). Fixing this needs the new recompiler of V2, not a change to this allocator. Tried next (2026-11-16): live range splitting in the allocator (`JITTER_SPLIT=gap`: a temporary per cluster of accesses, loaded and stored around it) makes the loop longer at every gap (1,455-1,552), because with 8 registers the cluster temporaries go to the stack too. The new recompiler needs its own register assignment and more NEON registers (q4/q5 and q6/q7 are taken by scalar floats and clamp masks), and its ARM code can only run on the Vita or in CI: qemu-user does not run under WSL1 |
| V2 stage: loop copies (patch 0100) | Done, on (`vu_loop_variant`) | `vu_loop_variant=1` (PC: `--vu-no-loop-variant` to turn off) | A VU block that branches back to its own start (3 pairs or more) gets a second copy of its code for the loop, in which the entry FMAC stall checks are decided at compile time: on the back edge the pending writes are the block's own last ones, known constants. RE4's vertex loop: about 1,171 ARM instructions per iteration instead of 1,436 (-18%); RE4 pictures, game frames and primitive counts bit-identical with and without |
| RE4 without VU clamping | Profile default | `vu_no_clamp=1` in RE4's entry of `assets/game_profiles.ini` (PC: `--vu-no-clamp`) | The loop copy goes from 1,188 to 1,127 ARM instructions (-5%); RE4 gameplay pictures identical over 300 vblanks |
| V2 stage: 10 NEON registers without clamping (patch 0098) | Done; untested on ARM hardware (CI's ARM job runs the new `VuTest_noclamp`) | automatic with `vu_no_clamp=1` (`JITTER_NO_EXTRA_MD=1` turns it off) | With clamping off no VU block uses the clamp masks, so q6/q7 join q8-q15 as allocatable vector registers (callee saved; scalar floats stay limited to s16-s23). RE4's loop copy: 1,128 -> 1,080 ARM instructions (-4%). All of V2 so far on this loop: 1,436 -> 1,080 per iteration (-25%) |
| V2 stage: ARM vector code generation (patch 0098) | Done; untested on ARM hardware | always on | A 128-bit load into an allocated register goes straight into it (it went through q0 and a `vorr`): 1,080 -> 1,057. Sticky flag accumulation: the value with its bit 0 replaced by the zero test (`VSRI`, new in the assembler) instead of building both bits: 6 -> 3 NEON instructions per FMAC result, 1,057 -> 1,019. RE4's loop copy, all of V2 so far: 1,436 -> 1,019 ARM instructions per iteration (-29%). Not changed: MINI/MAX (the PS2 compares the bits as integers: +0 equals -0 and the exact bits are kept, so NEON's float min/max would differ) |
| CI | ARM build fixed (457b56a) | — | main_host.cpp used GL renderer classes outside `VITAPS2_HAS_GL`; the ARM Linux CI job failed at Build since 15 Nov (and the world-batch line since the gameplay merge) |
| T1 CI patches | Done: 0093 cut to the hunks no earlier patch has; `--ignore-whitespace` in `cmake/ApplyPatches.cmake` | — | All 97 patches apply to clean submodule copies (`runs/re4/cichk.sh`); CI logs not checked |

Not done yet, and why: C2 (COP2 flags) needs the EE profile in gameplay first, which the PC
runner cannot give (its EE samples land on the idle loops); small item 4 (texture storage
reuse) needs the Vita GPU's frame timing to know when a texture is free; P2 (GIF parsing on
the GS thread) moves work onto the GS thread, which is the Vita's wall in gameplay; G1/G3/G4,
P3, C1, T2/T3 and B6 are Vita-only. P1, R1 and V1 have now run on the Vita and are RE4's
profile defaults (`threaded_vu1=1 vu1_async=1 re4_skip_dof=176 re4_vu1_hle=1 gpu_world=1`);
other games keep them off until tested.

### First Vita results (2026-11-16)

RE4 gameplay from `state_b2` with the scripted walk (`runs/re4/gp.sh`, 150 s runs, warm JIT
cache: the first run after a new build is cold), `threaded_vu1=1` and RE4's profile
(`vu_no_clamp=1` now):

| Settings added | Game frames / ~125 s | Steady game fps | Pictures |
|---|---|---|---|
| none (build of 15 Nov) | 41 (cold start) | 0.6-0.8 | right |
| `vu1_async=1` | 115 | 1.0-1.4 | right |
| `re4_skip_dof=176` | 130 | 1.0-1.3 | right, depth blur gone |
| `re4_vu1_hle=1 gpu_world=1` | 207 | 1.8-2.0 | mostly right: in some frames a grey patch (missing leaves) or dark streaks |
| same, `gpu_world_slots=1` (16 Nov, now the Vita default) | 147 in 120 s | 1.6-2.0 | right: 17 of 17 frames clean |
| `gs_shaders=1` (vertex ring on) | 196 | 1.6-2.0 | as above |
| `gs_shaders=1 gs_vertex_ring=0` | 210 | 1.8-2.1 | as above |

- Async VU1 nearly doubles gameplay speed on the hardware; the GPU world adds 60-70% on
  top. Together: about 2.5x the build of 15 Nov (0.66 game fps then).
- The depth-blur skip helps little on its own on the Vita (the PC showed -40% time).
- `gs_shaders` and the vertex ring work but are no faster than vitaGL's fixed function
  path on the Vita: they stay off.
- Two Vita-only bugs found and fixed: vitaGL's GLSL translation crashed linking a
  fragment shader whose alpha test used a `bool` (SceGxm undefined instruction; found with
  the on-device shader self test, `shader_test.txt`), and vitaGL's packed attribute
  stream has 16-bit offsets (the vertex ring now draws from a base vertex).
- Solved (16 Nov): the GPU world's artifacts below came from several batches per draw. The
  vertex shader picks its batch's matrix with `uM[slot * 4]`, and with slots above 0 the
  Vita got it wrong (vitaGL's translation of the dynamic uniform index, most likely; Mesa is
  right). With `gpu_world_slots=1`, now the Vita default, 17 of 17 frames are clean at the
  same speed (1.6-2.0 game fps). The earlier notes:
- Was open: the GPU world's artifacts on the Vita. Some near-camera ground triangles are never
  rasterized (with `gpu_world_debug=2` the world draws red and those areas stay grey),
  sometimes with dark streaks; the CPU path draws them right (`re4_vu1_hle=1` alone is
  clean on the Vita, at 1.3-1.5 game fps with `vu1_async`). Ruled out on the Vita: async VU1
  (also without it), culling (`gpu_world_debug=4`: same; 64: the ground goes), the depth
  test (8: same), batches per draw (`gpu_world_slots=4`: worse), corners far off screen sent
  to the CPU (`gpu_world_guard=50/200`: same), one vertex array per attribute
  (`gpu_world_soa=1`: same), vertices clamped inside the far plane (same, plus new
  artifacts), more than 64k vertices per draw (704 at most). The PC (Mesa) draws the same
  batches right.

### Second Vita day (2026-11-16)

Same method (`runs/re4/gp.sh`, 120 s from `state_b2`, warm JIT cache). Each perf.csv row now
ends with `battery_pct,charging,battery_temp_c,arm_mhz,core3`.

| Change | Result on the Vita |
|---|---|
| GPU world, one batch per draw (Vita default) | Artifacts gone: 17 of 17 frames clean, 1.6-2.0 game fps |
| RE4 profile: `threaded_vu1=1 vu1_async=1 re4_skip_dof=176 re4_vu1_hle=1 gpu_world=1` | With no settings passed: 1.5-2.1 game fps, frames clean. A New Game with these settings plays the intro movies and reaches the first real-time scene, where threaded VU1 used to hang on DMA channel 1 (3-4 game fps in that cutscene; checked for its first 30 s) |
| P3 fourth core, own kernel module | 153 game frames per 120 s against 150 without core 3: small, near noise. The SPU2 mixer is slower on core 3 (143), so it stays on core 2 |
| IOP thread on core 3 (`iop_thread=1 iop_core=3`) | Movies: RE4's prologue movie, 120-240 s of a New Game, alternating runs: 14.71 and 14.90 pictures/s with the IOP inline, 15.34 and 15.34 on core 3 (+4%; game frames 1833/1829 against 1922/1926). Gameplay: 134 game frames per 120 s against 152-153 (-12%). Stays off: the movies gain less than gameplay loses, and both are far from their targets. A per-scene switch (IOP thread only while a movie plays) could take the movie gain alone |

| G4 palette textures | Not built: texture decoding is 2 ms/s of the GPU thread (see the G4 row) |

Where the GPU thread's time goes in RE4 gameplay (it is the bottleneck: busy about 95% of the
time, 948 ms per second in `gpu_pump_ms`; `gpu_parts`, warm run K8, ms per second):

| Part | ms/s | What it is |
|---|---|---|
| Target uploads, reading GS memory (`UPLOAD_SWIZZLE`) | 101 | render targets rebuilt from emulated GS memory |
| Draws (`DRAW`) | 97 | glDrawArrays/Elements |
| Read-backs (`DOWNLOAD_PIXELS` + `DOWNLOAD_SWIZZLE`) | 85 | glReadPixels and writing the pixels into GS memory |
| New render targets (`TARGET_CREATE`) | 68 | GL objects created for new targets |
| Target re-layouts on the GPU (`RELAYOUT`) | 33 | |
| Texture cache lookups (`TEX_LOOKUP`) | 18 | |
| Target uploads, GL side (`UPLOAD_GL`) | 10 | |
| Texture decoding (`TEX_DECODE`) | 2 | what G4 would remove |

Per game frame that is about 2 new render targets, 2 target uploads of about 600k pixels
from GS memory and 2 read-backs of about 540k pixels: RE4's post-processing round trips
between the GPU and emulated GS memory. Together about 250-300 ms of every second on the
bottleneck thread, about 4x the rest of the measured parts after drawing. That makes G5
(lazy image transfers), G6 (remaining read-backs) and G7 (skip or halve post-processing)
the next places for gameplay speed, ahead of G4. About 530 ms/s of `gpu_pump_ms` is not in
any part yet (GS register processing, vertex conversion and so on): a finer split there is
the other open question.

Testing safety, learned the hard way (details in RE4.md, "Traps when testing"):
- `vitalab clock` must not be used: `vitalab clock default` (333/111/111/111 MHz from the
  kernel, with the emulator open) took the Vita fully offline on 15 Nov. Vita Lab also
  ignores CPU values above 444, so B6's 500 MHz test was never possible through it; it needs
  PSVshell or LOLIcon by hand. B6 is on hold.
- Vita Lab's kernel plugin has a rescue listener: 3 UDP datagrams within 3 s on port 1342
  reboot the Vita (`tools/vita_rescue.py reboot`), for when the command server is stuck but
  Wi-Fi is up. Tested: back in about 18 s.

### Gates G0-G3 of PATH_TO_30FPS.md (2026-11-16, on the Vita)

Build `ad5322a` plus the ceiling's display skip; RE4 gameplay from `state_b2`, 120 s, warm.
New perf.csv columns: `gs_feed_ms`, `gs_feed_calls`, `gs_feed_kb`, `gs_regs_ms`,
`gs_reg_calls`, `gs_reg_writes`, `gs_mail_calls`, `world_ms` (patch 0101 and
`GSH_Hardware.cpp`); settings `gs_ceiling=1` and `vif1_log=S`; scripts `runs/re4/eefuncs.py`
and `runs/re4/vif1cmp.py`.

**Before the gates: one batch per draw costs little.** Warm, alternating pairs, 120 s:
`gpu_world_slots=16` 151 and 151 game frames (1.73 and 1.81 game fps after 30 s),
`gpu_world_slots=1` 143 and 145 (1.79 and 1.77). The steady rate is the same; the totals
differ by about 5%, all in the first 30 s. The 207 in the first table came from longer runs.

**G0, time per game frame (run G0, 1.69 game fps):**

| Thread | CPU share | Per game frame | Budget |
|---|---|---|---|
| GPU thread (core 1) | 79% (pump elapsed 919 ms/s) | about 470 ms CPU, 544 ms in the pump | 33 ms |
| Emulation thread (core 0) | 51% | about 300 ms | 33 ms |
| VU1 thread (core 2) | 42% | about 245 ms | 33 ms |

The GPU thread's pump, split by entry point (inclusive: the draws a call makes count in it),
per game frame: register write buffers 253 ms (1,760 calls, 64,400 register writes), the GPU
world path 95 ms (draws included), image transfers 92 ms (360 calls, 1.6 MB), the display
stage at the flip 83 ms, the rest about 20 ms. Mailbox calls: 3,330 per game frame. So
nearly all of the "unattributed" half sits in the register path (characters, effects, HUD,
and the post-processing that causes the round trips), the world path outside its draw call,
and the display.

**G1, the ceiling** (`gs_ceiling=1`: no draws, no GPU side of transfers or read-backs, no
display work; registers still processed): `vm_fps` 7.6 against 4.0 (1.9x), so about 3.2
game fps if the game keeps the same emulated frames per game frame (`game_fps` reads 0 here:
it counts drawn frames). The emulation thread is then the wall: 95.5% busy, VU1 83.5%, the
GPU thread 41.5%. A first ceiling run without the display skip stayed at the GPU thread
(the flip rebuilt the picture from GS memory: 742 ms/s). **Under 5**: by the plan's rule,
the EE work is as urgent as the renderer.

**G2, EE time by RE4 function** (`ee_hot_after=30`, 5,968 samples, 1,485 blocks covering
87.8%): game code 84.8% of EE samples, BIOS/kernel 3.0%. The top 5 game functions are 21.5%
of game code, top 10 36.1%, top 20 52.7%, top 50 75.8%. Top entries: `sceTimerInit` 6.1%
(probably unnamed code after that symbol, maybe a wait loop: check before counting it as
work), `partsWorldCalc__6cModel` 3.3%, `HermiteInterpolation` 3.1%, `InsertPacket` 2.9%,
`InsertPacketSCR` 2.8%, collision tests (`collision_sphere_hexahedron`, `hitCheck3`,
`AtSphereCapsuleCk`) 7.9% together, `Vu0Sinf`/`Vu0Cosf` 4.4%, `PSMTX*` 4.0%, `memset`
1.5%. Library math (`PSMTX*`, `Vu0*`, `memset`) is about 10% and would help every game (C3).
For overlays: covering 90% of game code needs well over 50 functions; this is spread, not
concentrated. The share inside the game's draw phase and in VU0 (COP2) code is not split
out yet.

**G3, VIF1 transfers frame to frame** (`vif1_log=60`, two consecutive game frames while
walking): 11,607 and 11,635 transfers, 3.3 MB each. Of frame 1's bytes: 93.6% same address
and same data as frame 0, 4.4% same data at another address, 2.0% new; no address carried
different data. No transfers from the scratchpad; 6,803 include their DMA tag. Geometry can
be keyed by address (W2), with a data check.

### W1, no round trips: first steps (2026-11-16/17)

How a game frame's render target traffic looks (PC runner, `gs_diag`, `runs/re4/diagrt.py`):
RE4's post-processing draws into 0x2A0000, 0x2D8000, 0x310000 and 0x348000 at 64, 128, 256
and 512 pixel widths by turns. Each width change made a new target (`N`), filled by a
read-back of the old one (`D`) and an upload from GS memory (`U`). Before W1, per game frame:
10 new targets, 8 uploads, 7 read-backs, 3 GPU relayouts.

| Step (setting, all on) | What it removes |
|---|---|
| Target pool (`target_pool`) | Released targets keep their GL objects (8, by size): 10 creations a frame became 3 |
| Partial relayout (`partial_relayout`) | A new target that starts inside an old one of the same pixel size at a whole page row gets the shared pages by GPU copy; the old one keeps its rows before it |
| Sprite creates its target (`sprite_new_target`) | An opaque first sprite's target is created before the upload decision |
| Strip cover (`strip_cover`) | RE4 clears the new 512x448 target at 0x310000 with eight full-screen sprites under 64-wide scissors: consecutive opaque strips replace never-uploaded rows, so no upload; anything else touching targets first uploads the columns not drawn yet |
| Narrower relayout uploads (part of `partial_relayout`) | A relayout covering whole page rows from row R on uploads only the rows above R (0x2D8000: 256x416 became 256x192) |

On the Vita (RE4 gameplay, two alternating rounds, GPU thread ms per second for read-backs +
uploads + target creation + relayouts): all of W1 off 299 and 302; W1 without the strip
cover 240 and 239; with it 191 and 192 (uploads 113 -> 65). The narrower relayout uploads
came after these runs. Game fps 1.68/1.71 off against 1.76-1.80 on: within run noise, as the
whole step is about 11% of the GPU thread. Pictures as before (PC: no pixel off by more than
16, mean difference 0.01; the GS tests caught a strip cover bug before it shipped).

Still per frame (PC): 6 uploads (largest 256x224 at 0x310000 and 0x348000, 256x192 at
0x2D8000), 12 read-backs (largest: all of 0x2D8000 256x448 when it is reused 64 wide; 128x224
of 0x348000; seven 128x32 pieces of 0x348000 when the last pass reads it as a texture).

P1 as built: `CVif1::ReceiveDMA` copies the DMA data into a queue and returns at once (small
chunks without a tag are merged, buffers are reused). The VU1 worker runs `ProcessPacket` on it
and runs each program inline. DIRECT data and MSKPATH3 go into the XGKICK queue, so PATH1 and
PATH2 stay in order, and the EE thread sends them to the GIF as before. VIF1 register reads and
writes, VU1 memory access, VIF1 read-backs, save states and vblank wait until the worker is
quiet (queue empty, or VIF1 stalled on an interrupt or a stopped program). A GIF DMA (PATH3)
waits for the queue first (`vu1_async_path3_sync`, on). Gameplay sends about 2.7 million
packets and 1.2 GB per 900 vblanks (3 MB per game frame) through the queue.

```text
VitaStation2: performance work not yet done
    Nov 15, 2026        ·   ​@Troy

  Three structural problems account for most of the Resident Evil 4 gameplay gap, and none
  of them is a planned item in the repository's docs: VU1 and the emulation thread take
  turns instead of running in parallel, each vertex is copied about six times on its way to the
  GPU, and the VU recompiler emulates the VU pipeline at run time.

  Where things stand
  These are the sessions' own measurements on the Vita, not mine.

     Scene                                   Now                         Goal   What limits it

     RE4 intro movie                         21–21.5 pictures/s (43–45   30     Emulation thread: 22.5 ms per
                                             emulated fps)               (60)   emulated frame against 16.7. EE
                                                                                code 11.0, IOP code 3.6, IOP
                                                                                kernel 2.3, the rest about 5

     RE4 gameplay                            0.7 game frames/s           30     Per game frame: GS thread
     (forest start)                                                             about 1.2 s, VU1 about 0.63 s,
                                                                                emulation thread about 0.1 s of
                                                                                its own work. The emulation
                                                                                thread waits 35% of the time on
                                                                                VU1 and 38% on the GS

     RE4 title menu                          55–59 emulated fps          60     Close

     Silent Hill 2 movie                     About 1 picture/s           30     The game's own VIF1 + VU1
                                                                                colour conversion. The threaded
                                                                                VU1 hang fix is merged but not
                                                                                retested

  A gameplay frame is about 9,600 triangles, 360 sprites, 700 draw batches, 7 read-backs
  and 13 uploads.

  The short version
  Three structural problems, none of them a planned item in the docs:

     1. VU1 and the emulation thread take turns instead of running in parallel (P1).
     2. Each vertex is processed and copied about six times between VU memory and the
        GPU, mostly by generic per-register code and vitaGL's fixed-function path (P2, G1,
        G2).
     3. The VU recompiler emulates the VU pipeline at run time instead of resolving it at
        compile time (V2).

  Two facts in the docs are out of date and unlock further work:

         Guest memory mapping and write protection are available on the Vita, through the
         kubridge kernel plugin (C1).
         The fourth CPU core can be used with a plugin (P3).

  One repository problem: CI has been failing since 6 November (T1), so the instruction-
  count chart stopped before fast RAM, cheap exits and tier 2 were added.

  Gains below are my estimates unless a measurement is named. Nothing here was run on a
  Vita.

  P. Pipeline between the cores
  Code references are to the tree with all 96 patches applied ( external/Play/... ); line
  numbers are approximate.

  P1. Asynchronous VU1 (new; the largest general gain for 3D)
         Today (verified in code): the worker thread only runs the microprogram. VIF1 stays on
         the emulation thread. Any command that needs VU1 idle sets VEW, and the EE then
         waits: CSubSystem::CountTicks calls m_vpu1->Sync()
         ( Source/ee/Ee_SubSystem.cpp:344 ), which spins in CVpu::Sync
         ( Source/ee/Vpu.cpp:205 ). XGKICK packets are copied into a std::vector under a
         mutex ( Vpu.cpp:322 ) and later parsed on the emulation thread ( Vpu.cpp:270 ).
         Measured by the last session: 349 ms/s of the emulation thread spent in VU1 sync,
         about 1,588 waits per second, VU1 thread only 42% busy.
         Change: the design PCSX2 uses for its threaded VU1.
                VIF1 command parsing stays on the emulation thread, but the work is queued into
                a lock-free single-producer ring: UNPACK payloads with the VIF state they need
                (CYCLE, MODE, MASK, ROW/COL, TOPS), MPG uploads, and MSCAL/MSCNT.
                The VU thread performs the unpack and runs the program.

                VIF1 never reports VEW, so the DMA completes at once, exactly as in non-
                threaded Play. vu1_wait_on_stall and the "emulated time runs ahead" problem
                disappear.
                At each MSCAL the emulation thread puts a "PATH1 placeholder" into the GS
                queue. When the GS thread reaches it, it takes that program's XGKICK packets
                directly from the VU thread, waiting if they are not ready. Order against PATH2
                and PATH3 is kept without the emulation thread touching the data.
         Sync points that remain: EE access to VU1 memory or registers, VIF1 FIFO read-back,
         save states, ring full.
         Risk: games that depend on real VIF1/VPU status timing. Keep the current mode as a
         per-game fallback. PCSX2 has shipped this design for years with a per-game off
         switch.
         Estimate: the emulation thread loses the VU1 waits and the VIF section (unpack plus
         inline VU1 was 50–85% of that thread in gameplay scenes without the thread). VU1
         can run continuously instead of 42% of the time.

  P2. A ring buffer to the GS thread, with GIF parsing on the GS side (new)
         Today (verified):
                Every hand-off is a std::function pushed on a std::deque under a mutex, with
                notify_all ( Source/MailBox.cpp:30 ).
                Image data is new[] plus memcpy per packet ( Source/gs/GSHandler.cpp:635 ).
                GIF tags are decoded register by register on the emulation thread
                ( Source/ee/GIF.cpp:106 ) into 16-byte RegisterWrite records, which the GS
                thread walks through a second switch ( src/gs/GSH_Primitives.cpp:5 ).
         Change: one single-producer ring of raw GIF data. The producer does a memcpy ; the
         GS thread parses. The consumer spins briefly before sleeping, because waking a
         sleeping thread on another core takes about 0.5 ms on the Vita (the sessions'
         measurement).
         Gain: takes "VIF direct" (1.7 ms per frame in the movie) and all per-register work off
         the emulation thread, and removes a lock and an allocation per message. It is the
         precondition for G2.

  P3. Use the fourth core (the roadmap says "explore"; it is available)
         The CapUnlocker kernel plugin lets game-mode apps run threads on CPU3, the
         system core, which is reportedly 10–15% busy.

         Detect it at start by trying affinity mask 0x80000 , and fall back when it is refused.
         Move the SPU2 mixer, audio output, disc read-ahead and the movie copy thread there.
         Then re-test iop_thread=1 with the IOP thread spinning on a core of its own. It lost
         before (20.5 against 20.7 pictures/s) because all three cores were full and each wake
         cost about 0.5 ms. Its share is 7.6 ms of the movie's 22.5 ms frame.

  P4. Event-driven time slices
          CPS2VM::EmuThread ( Source/PS2VM.cpp:1081 ) runs fixed slices: 4,800 cycles by
         default, 19,200 for RE4, and 38,400 breaks RE4.
         Run the EE to the next real event (timer compare, hblank or vblank, pending DMA or
         SIF) and let the IOP catch up lazily: on SIF or DMA access, at its own next event, and at
         slice end.
         This gives the benefit of long slices without the breakage, and replaces the per-game
         ee_slice guess. Estimate: a few percent of the emulation thread, more in menus.

  G. GS renderer
  The GS thread is the main wall in gameplay: about 1.2 s per game frame.

  G1. Own shaders and raw vertices instead of the fixed-function path (new)
         Today (verified):
                 AddVertex ( src/gs/hw/GSH_Hardware.cpp:2533 ) does, per vertex on the CPU, a
                double-precision divide for Z and up to six float divides.
                Strips are expanded into independent triangles, and each strip vertex is
                converted three times ( src/gs/GSH_Primitives.cpp:62 ).
                vitaGL's fixed-function draw then rebuilds a shader key from GL state and copies
                the client arrays again on every glDrawArrays .
         Memory traffic (my arithmetic): a 48-byte PS2 vertex becomes about 48 bytes of
         register writes, then 3 × 40 bytes, then 3 × 44 bytes, then vitaGL's copy: over 400
         bytes. The Vita copies about 310 MB/s (the sessions' copy_bench ). At 15,000 vertices
         × 30 fps that is roughly 180 MB/s, more than half of the bus.
         Change: a small family of shader programs. Custom shaders already work in this
         renderer (the alpha CLUT and world programs).
                Vertex attributes are the raw GS values: XY in 12.4 fixed point, Z, RGBA, STQ or UV,
                fog.

                Uniforms carry 1/target size, the XY offset, 1/texture size and the Z scale. The
                vertex shader does the arithmetic.
                Indexed strips, written once into GPU-mapped memory.
         This also removes the glTexEnv traffic in ApplyState ( GSH_Hardware.cpp:2227 ).

  G2. Fast paths for packed vertex formats (new; needs P2)
  Recognize the common PACKED register lists (ST + RGBAQ + XYZ2 or XYZF2, UV +
  RGBAQ + XYZ2, RGBAQ + XYZ2) and convert a whole NLOOP run in one tight NEON loop
  straight into the vertex ring, without WriteRegisterImpl and VertexKick per register.

  G3. Count the GXM scenes, then cut them (new)
         In vitaGL every framebuffer change ends the scene and begins a new one
         ( scene_reset in vitaGL's gxm.c ). On this tile-based GPU a scene has a fixed cost,
         and depth_force_load adds a depth load and store to each. glReadPixels also calls
          sceGxmFinish , so the CPU waits for the GPU.
         RE4's frame moves between about ten target layouts for 700 batches, and every
         feedback copy, alpha-CLUT pass, relayout and depth clear is one more scene.
         First: add scenes per frame to perf.csv . Then:
                Copies without a scene. Do copies, relayouts and ShrinkTarget with
                 sceGxmTransferCopy , the GPU's 2D blitter. vitaGL's glCopyTexSubImage2D is
                literally glReadPixels followed by glTexSubImage2D (vitaGL
                 textures.c:2314 ), and ShrinkTarget ( GSH_Hardware.cpp:782 ) still calls it.
                Self-sampling without a copy. Read the framebuffer inside the fragment program
                (GXM programmable blending). This covers RE4's 16 colour-grading strips, exact
                 (A−B)·C+D blending and DATE. Unverified: I could not confirm the shader syntax
                from public sources and vitaGL does not expose it, so start with a 20-line test on
                the device.
                Local-to-local transfers on the GPU. ProcessLocalToLocalTransfer
                ( GSH_Hardware.cpp:1231 ) downloads both targets, copies pixel by pixel on the
                CPU and uploads again.

  G4. Palette textures on the GPU (new)
  GXM has P8 and P4 texture formats with a separate palette, and vitaGL exposes them
  ( GL_COLOR_INDEX8_EXT , paletted compressed formats; vitaGL textures.c:842 ). PSMT8
  and PSMT4 textures then need no CLUT decode: upload the indices once and set a 256- or

  16-entry palette. Texture × CLUT combinations stop multiplying cache entries; the key
  includes clutHash today ( GSH_Hardware.cpp:1404 ).

  G5. Lazy image transfers (new)
  RE4 streams about 130 small textures per frame through the same GS memory slots. Each
  one costs a copy, a swizzled write into GS memory, a CLUT sync with a hash, and a page
  hash, all before the content cache hits. Record a whole-rectangle host-to-local transfer as
  "region R holds the payload with hash H" and write GS memory only if something else
  reads those bytes. A texture lookup that matches R exactly goes straight to the content
  cache.

  G6–G10. Smaller renderer items

     Item        What                                              Status

     G6          Remaining read-backs: Z sampled as PSMZ24         Noted by the sessions, not done
                 (keep depth in a texture-backed surface,
                 convert on the GPU); the CT16 view of a CT32
                 target (shader pass)

     G7          Per-game skip-draw ranges for blur, depth-of-     "Skip post-processing" noted as
                 field and haze passes; post-processing at half    an idea
                 rate. About 60% of RE4's GS thread is post-
                 processing round trips

     G8          Frame skip that also drops the skipped frame's    New
                 queued VU1 work (needs P1; per game)

     G9          Stopgap until G1: build vitaGL from source with   New
                 its speed options ( NO_DEBUG ,
                  DRAW_SPEEDHACK , INDICES_SPEEDHACK ,
                  TEXTURES_SPEEDHACK ,
                  SHARED_RENDERTARGETS ,
                  HAVE_VERTEX_LAYOUT_CACHE ). CI links the SDK
                 image's stock library

     G10         NEON block copies in UploadTarget and             New
                 DownloadTarget ( GSH_Hardware.cpp:874 ,
                 :960 ), which compute a table offset per pixel

  V. VU1

  V1. RE4's programs on the GPU, with retained geometry
          gpu_world is in progress. Last state: the modulo-4096 wrap and negative-w fix was
         found; the ground, leaves and house are still missing.
         Next step once it draws correctly (new): stop rebuilding vertex arrays every frame.
                Key a GPU vertex buffer by the unpack source (EE RAM address and length).
                Validate it cheaply: write-protected pages through C1, or a sampled hash.
                Per frame, send only the matrices and colour constants.
                With P1 the unpack of that static data can be skipped as well.
         The same scheme fits skinning, with bone matrices as uniforms.
         Why by source, not by content: hashing 70,000 vertices per frame would itself cost
         7–11 ms at the Vita's memory speed.

  V2. A new VU recompiler, as tier 2 was for the EE (the design is new)
         Today (verified):
                Pipeline state lives in memory and is advanced at run time: pipeFmacWrite stall
                checks at each block start, pipeTime , eight-slot rings for the MAC, sticky and clip
                flags, Q and P pipeline checks ( Source/ee/VuBasicBlock.cpp:17 ,
                 Source/ee/VUShared.cpp:255 , :1948 , :2031 ).
                Flags are assumed live at every block end.
                8 of the 16 NEON registers are allocatable.
                Result: 1,457 ARM instructions per 38-pair loop iteration.
         Design (microVU's approach):
                Compile a whole microprogram from its entry, with a control-flow graph.
                Key block variants by the pipeline state at entry, so stall, Q/P and flag timing are
                known at compile time and need no run-time checks. A self-loop converges after
                one iteration.
                Backward liveness of flag instances across blocks: compute MAC, status and clip
                flags only where an FMxx, FSxx or FCxx instruction can read them. Treat them as
                dead at the E bit, with a per-game override.
                VF registers in q0–q15 across loops, VI registers in core registers, direct
                 vld1 / vst1 into VU memory.

         Estimate: 4–8× fewer ARM instructions on FMAC-heavy loops. It is needed for the
         programs nobody will port by hand (RE4's 0x07C8 and 0x3008, and every other
         game).
         The two small changes the last session made in the existing JIT (one stall test per
         block, in-block flag reads) gave no measurable gain on the Vita, which fits the cost
         being structural.

  V3. XGKICK without allocation
  Replace the std::vector and mutex per kick ( Vpu.cpp:322 ) with a ring. The packet
  length is already computed there.

  C. EE and IOP

  C1. kubridge: memory mapping, protection and fault handlers (new)
   docs/RE4.md says mapping guest memory "is not available to Vita user code". The
  bythos14 fork of the kubridge kernel plugin exports kuKernelMemReserve ,
   kuKernelMemCommit (with a base block, which gives mirrors), kuKernelMemProtect and
   kuKernelRegisterExceptionHandler (data aborts with the full register context). I read
  the header. Make it optional: detect at start and fall back.

         Fault-based fast RAM. Replace the range test and branch with one mask instruction.
         The larger part: accesses through the kseg0, kseg1 and uncached (0x20000000)
         mirrors become as fast as plain RAM. Display-list code writes through uncached
         pointers constantly and takes the page-lookup path today. On a 32-bit host this needs
         a reserved window (512 MB with the top three address bits masked off) so that every
         non-RAM address faults instead of landing in the emulator's own memory. Whether
         the Vita grants that much address space has to be tested first.
         Real code-overwrite detection. CEeExecutor::SetMemoryProtected
         ( Source/ee/EeExecutor.cpp:484 ) has no Vita branch, so the Vita build never notices
         overwritten code. With protection, tier 2 could compile code that has not run yet.
         Dirty-page tracking for V1's geometry cache and for textures, instead of hashing.

  C2. COP2 (VU0 macro mode) flags in EE code (new; cheap)
         Today (verified): EE COP2 instructions call the shared VU code with no hints, for
         example VUShared::ADD(m_codeGen, …, 0, 0) ( Source/ee/COP_VU.cpp:504 ). So
         every one of them builds sign and zero flags (a NEON-to-ARM transfer, which stalls
         the Cortex-A9), updates the sticky flags and queues two flag-pipeline entries.
         Change: flag liveness for EE blocks. Compute flags only when a cfc2 of the status,
         MAC or clip register, or a VCALLMS, can read them before the next flag-writing
         instruction. PCSX2 has an analysis pass for exactly this.
         COP2 does not appear anywhere in the docs. Check first: the COP2 share of
         gameplay's hot blocks with tools/jit_arm_report.py .

  C3. libvu0 by signature (new)
  Sony's sceVu0* matrix and vector routines (MulMatrix, ApplyMatrix, RotMatrix,
  Normalize, inner and outer product) are in most 3D games and are pure functions: natural
  NEON replacements through the existing signature hook. The roadmap's "library HLE" list
  has memcpy and libm only.

  C4. Inline the hottest kernel calls; enter interrupt handlers directly
         IOP CpuSuspendIntr and CpuResumeIntr run about 4,400 times a second in RE4's
         movie. Each is an addiu zero, zero, N trap out to C++. Emit the flag update inline.
         For the SPU2 interrupt storm (5,600 a second, which the driver needs), enter the
         registered handler directly instead of running the BIOS stub at 0x200 (10.6% of IOP
         samples). This is the "cheaper path" the handoff asks for.

  C5. Tier 2
  The planned steps stand: regions through syscall , div , ldl / ldr and COP0, then the
  IOP front end. One addition: regions average 6 KB of code against a 32 KB instruction
  cache, so measure cache misses (T2) before adding features.

  C6. Build flags (new; nearly free)
  The build is plain CMAKE_BUILD_TYPE=Release ( .github/workflows/build.yml ). Add
  link-time optimization and profile-guided optimization for the C++ that dominates outside
  the JIT: VIF, GIF, the GS front end, SPU2, the HLE code and the decoder. The profile can
  come from the ARM build under qemu on a replay (T4).

  M. Movies
         M1. Hand pictures to the GPU as YUV (extends idea 5 in docs/RE4.md ). GXM
         samples planar YUV 4:2:0 directly with colour conversion, and vitaGL exposes it
         ( VGL_YUV420P_BT601 and others). The decoder's colour conversion, about 20% of its
         time, goes away. Where the game never reads the picture back, so does the 1.15 MB
         RGB copy.
         M2. The movie gap is 6 ms and today's plan covers it with no slack. P3 (IOP on CPU3),
         P2 (GIF parsing off the emulation thread, 1.7 ms) and C4 (interrupt path, about 1 ms)
         add slack that does not depend on tier 2.

  T. Tools and repository health
         T1. CI has been failing since 9cdb0c4 (6 November). I reproduced the patch failure
         on a clean checkout; I did not see the CI logs.
                 git apply rejects patches 0075, 0078, 0080, 0082 and 0086: their context has LF
                line endings and the upstream files have CRLF. They apply with --ignore-
                whitespace .
                Patch 0093 fails outright: its PS2VM , PS2OS and Ee_SubSystem hunks are already
                inside 0087, 0089 and 0091.
                Consequences: no CI-built VPK, no ARM tests under qemu, and the performance-
                gap chart stopped at b43b8a2 .
                Fix: --ignore-whitespace in cmake/ApplyPatches.cmake , and regenerate 0093.
         T2. Hardware counters. Tuning uses instruction counts under qemu and a 1 ms guest-
         PC sampler. With memory this slow, cache and TLB misses matter and are invisible. A
         small kernel plugin (or Vita Lab's) can enable user access to the Cortex-A9
         performance counters: cycles, instruction- and data-cache misses, branch
         mispredictions, per profiler section in perf.csv .
         T3. A native sampling profiler. Sample the host PC of the GS, VU1 and emulation
         threads from Vita Lab's kernel side and resolve against the map file. Roughly 300 ms/s
         of the GS thread in gameplay is covered by no timer today.
         T4. Replay benchmarks. Record one gameplay frame's GS stream and its VIF1 stream,
         and replay each in a loop on the Vita. This isolates the G and V work, is deterministic,
         and runs in seconds. The roadmap has frame-dump replay for correctness only.

  Small things seen while reading the code
  Each is independent and low risk.

     1. AddVertex : precompute reciprocals per state and drop the double Z divide.
     2. Any non-vertex register write sets m_stateDirty , even with the same value, which
        costs a DecodeState and a 52-field compare per primitive group
         ( GSH_Primitives.cpp:5 ). Compare the register value first.
     3. CVpu::Sync reads steady_clock::now() twice per wait. That clock costs several
         microseconds on the Vita and there are about 1,600 waits a second.
     4. Every texture upload calls glTexImage2D with new storage
         ( GSH_Hardware.cpp:1534 ), which allocates GPU memory in vitaGL each time. Recycle
         by size.
     5. notify_all on every mailbox send, even when the consumer is awake.

  Already known, and already tried
  Known to the sessions and still open (not new):

         Tier 2 coverage and IOP tier 2; a cheaper SPU2 interrupt path.
         Finishing gpu_world ; the skinning program; the Z, CT16 and PSMT8H read-backs.
         Skip post-processing; GS-level frame skip.
         Faster decoder parsing and colour conversion; pictures straight to GL.
         A signature scanner on for every game; ee_slice in the auto-tuner.
         Retesting Silent Hill 2 with threaded VU1; testing the disc read-ahead fix.

  Tried and measured; do not repeat as they were:

     Tried                                               Result

     IOP thread sharing the three cores                  Slower: 20.5 against 20.7 pictures/s

     SPU2 IRQ raised once per edge or per hit            Slower: the driver polls instead

     JIT regions (step 3a)                               No gain on RE4

     Cutting EE work in the movie (room flag lookup,     Frame time unchanged
     MW idle thread)

      ee_slice=38400                                     No display output in RE4

     Tried                                                           Result

     Decoder on core 1                                               Slower: 11.6 against 14.1 pictures/s

     VU stall test per block, in-block flag reads                    No gain on the Vita

     Condition variables on the Vita                                 Lose wakeups; use semaphores or
                                                                     polling

  Order I would take
     1. T1, T4, T3: CI, replay benchmarks, native profiler. About a day, and everything after it is
        measured.
     2. P1 and P2: the pipeline.
     3. G1 and G2, then G3 with the profiler's numbers.
     4. C2 and C3 (every game), and V1 retained geometry (RE4).
     5. V2, C1, P3.

  Where the list should put us
  The list should take RE4's movie to about its target and RE4 gameplay from 0.7 to roughly
  8–15 frames a second. It does not reach 30 in gameplay on its own. All figures in this
  section are my estimates.

     Scene                         Today             With the list    What limits it then
                                                     done

     RE4 title menu                55–59 emulated    60               Nothing
                                   fps

     RE4 intro                     21.5 pictures/s   27–30            The decoder, close to a full core
     movie

     RE4 gameplay                  0.7 game          8–15             The EE's own compiled game code
                                   frames/s

     Silent Hill 2                 About 1           Single digits    Its VU1 colour-conversion chain,
     movie                         picture/s                          which needs a native path of its own

         Movie: P2, C4, P3, C6 and P4 take roughly 4 ms off the 22.5 ms frame. The tier 2 work
         already planned has to supply the last 2 ms for a locked 30.
         Gameplay, GS thread: 1.2 s per game frame could fall to 30–60 ms if G1 to G7 land.
         Gameplay, VU1: 0.63 s could fall to 20–40 ms with V1, V2 and P1.
         Gameplay, emulation thread: this becomes the wall. About 0.1 s per game frame is
         compiled game code, and P1 and P2 remove waiting and data handling, not that code.
         C2, C3 and tier 2 might bring it to 60–80 ms. The IOP adds about 15 ms per game frame
         unless P3 moves it to its own core.
         Game speed: at 8–15 frames a second RE4 would also play in slow motion, if its logic is
         tied to frames as I believe (not checked).

  Reaching 30 in gameplay needs the EE's code itself about 3× faster. That is bet B1 below,
  helped by native replacements of the game's hottest gameplay functions.

  Bigger bets that have not been tried here
  None of these has been tested. All of them run on the Vita with no PC step for the player,
  and each has a cheap first experiment. (Building and testing the emulator itself still uses
  CI and Vita Lab, as now.)

  B1. An optimizing compile on the Vita, run when the game is not using the CPU
         Why: the on-device tier 2 gained 3–9% because it had to compile during play, in under
         a millisecond per frame. The limit was when the compiler runs, not where.
         How:
                Run the heavy compile between sessions (on exit, or from an "Optimize" entry in
                the game list, for example while charging) with every core and no time limit. Once
                P3 is in, also run it on the fourth core while playing.
                Work on whole functions that earlier sessions actually ran, using what was
                recorded: hot functions, the targets indirect jumps really took, which accesses
                always hit RAM.
                With minutes instead of milliseconds it can afford real passes: registers held
                across the whole function, 32-bit arithmetic wherever the upper half is provably
                dead, stack and constant-address accesses with no range check, direct calls
                between compiled functions, checks only at loop ends and calls.
                Store the result in the per-game cache the emulator already loads. The JIT stays
                as the fallback, so coverage never has to be complete.

         Estimate: 2–3× on the EE code it covers. This is less sure than borrowing a mature
         compiler would be, because the optimizer has to be written.
         First experiment: nobody has profiled the EE during gameplay yet, so do that first
         ( ee_hot_after from the gameplay state). Then hand-write what such a compiler
         should emit for the ten hottest functions, install it through the existing native-
         replacement hook and measure. That bounds the gain before anything is built.
         To investigate: whether LLVM's 32-bit ARM back end can be built for the Vita and
         used as that optimizer. It would be a large port and I have not checked feasibility.
         Risks: function boundaries, interrupt and thread-switch points, cycle accounting,
         code loaded at run time.

  B2. A compiler from VU1 microprograms to vertex shaders that checks itself on
  the Vita
         The gameplay session ported one RE4 program by hand. Make it automatic, on the
         device: analyse a microprogram's vertex loop when it is first uploaded and emit the
         vertex shader and the CPU-side glue. vitaGL already compiles shaders at run time.
         Self-check: for its first runs, execute each new program both ways, through the VU
         recompiler and through a CPU evaluation of the translated form, and compare the
         XGKICK output. Switch to the GPU path only when they agree, and store the verdict
         in the game's cache. Programs that fail stay on the recompiler.
         Why: RE4's forest needs about 258 million VU cycles a second, which no CPU
         recompiler on this hardware reaches.
         Where to start: RenderWare's stock vertex programs. They ship in a large part of the
         library (the GTA games, Burnout), although the biggest titles add custom ones.
         The roadmap has the census of microprograms; the automatic translator and its self-
         check are the new part.

  B3. In-between frames drawn from real geometry
         Once geometry is retained on the GPU (V1 or B2), the emulator knows each object's
         vertex buffer and matrix for the last two game frames. It can draw an in-between
         frame by interpolating the matrices and replaying the draw list. That is frame
         generation from geometry instead of from pixels, which is what the roadmap
         describes.

         Limit: it makes motion smooth; it does not make the game run faster. It pays for games
         that keep correct speed at a low logic rate (time-step logic, as in the GTA games, with
         the EE underclocked). Elsewhere it gives smooth slow motion. It adds a frame of
         latency.

  B4. A renderer that never reads back
         Today each special case (alpha used as a palette index, a buffer drawn 2, 4 or 8 pages
         wide, Z sampled as a texture) has its own code path or falls back to a read-back.
         Generalize it: keep a map from every 8 KB page of GS memory to the GPU surface that
         holds it. Any texture or target view, in any layout, becomes a shader that maps view
         coordinates to a surface and a coordinate. CPU memory is touched only when the
         game itself reads pixels back.
         With P2's ring the GS thread can also look at a whole frame before drawing it: drop
         passes that get overwritten, group draws by target to cut scenes, and cache that
         analysis by the frame's shape, since frames repeat.

  B5. Movies converted once on the Vita (optional, lower priority)
         During movies the software decoder already has a core nearly to itself, so this matters
         less than the other bets.
         If it is wanted: between sessions, convert the disc's movie streams on the Vita into a
         format it decodes in hardware. Motion JPEG through the system's JPEG encoder and
         decoder is the candidate. It needs a test first, and the files are large (my estimate:
         about 1 MB per second of video).

  B6. Three cheap things nobody has measured here
         500 MHz. The app keeps an overclock plugin's 500 MHz when one is present
         (PSVshell, LOLIcon, both installed on the Vita). That is 12.6% more clock on every
         thread for no code.
         PAL releases. They run 50 fields a second, so full speed needs 17% less work per
         second. The game then targets 25, not 30.
         Patches that switch off effects. PCSX2's .pnach format is simple to support: small
         text files placed next to the game, or a built-in table for popular games. Community
         patches that switch off blur, bloom or depth of field exist for some games; check per
         game. Post-processing is about 60% of RE4's GS thread.

  More levers for a locked 30 (researched)
  A locked 30 at correct game speed needs four more things beyond the list: drop RE4's
  depth-blur passes, draw only every other game tick, shrink the IOP's share, and bypass
  vitaGL. The first is proven elsewhere. The second is the one that changes the arithmetic,
  and it is unproven.

  R1. Skip RE4's depth-blur passes (verified precedent; about a day)
         PCSX2's old hardware renderer shipped a rule for exactly this game:
          GSC_ResidentEvil4 in plugins/GSdx/GSState.cpp , present in releases 1.2.1 and 1.4.0
         and gone by 1.6.0. I read it in the source.
                A textured draw into the buffer at 0x310000 (CT32) whose texture is the Z buffer
                at 0x1C0000 read as PSMZ24: skip the next 176 draws.
                A textured draw into 0x310000 from 0x2A0000 or 0x348000 (CT32, no mask): skip
                that draw.
         Those are the same buffers the sessions' diagnostics show. 212 of about 715 batches
         per frame go into 0x310000, and post-processing round trips are about 60% of the GS
         thread.
         Cost: the game loses its depth blur. PCSX2 users played it that way for years.
         GSdx counts addresses in 256-byte blocks, so its 0x3100 is 0x310000 here.
         For RE4, PCSX2's patch collection has only a widescreen patch, so this rule is the
         route, not a ready-made patch.

  R2. Draw every other game tick, and interpolate (unproven; RE4-specific)
         Community reports describe RE4 on the PS2 as slowing down under load, not
         skipping frames, so its logic is very likely tied to frames. Underclocking the EE or
         interpolating frames would then give smooth slow motion, not correct speed. These
         are fan sources, not measurements.
         The lever: patch the game's main loop so it runs its update phase every tick but its
         draw phase every second tick. The emulator fills the missing frame with B3's in-
         between frame from real geometry.
         Arithmetic (my estimate):
                With every tick drawn, compiled game code must fit in about 25–30 ms, which is
                3–4× faster than today's 100 ms.

                With every other tick drawn, two updates and one draw must fit in 66 ms: 1.5× to
                2.5× faster, depending on how the 100 ms splits between update and draw. That is
                within what B1 might reach.
                VU1 and GS work for the real frames halves as well.
         Why it might be feasible: RE4 ships its symbol table in memory (5,602 names,
          docs/RE4.md ), so the main loop's phases can be found by name.
         Risks: update and draw may not separate cleanly, and results appear a frame later.
         First experiment: on the PC runner, find the loop, skip the draw phase on odd ticks,
         and check that the game runs at normal speed with half the frames.

  R3. Shrink the IOP's share (about 15 ms per game frame at full speed)
         Replace disc-loaded copies of standard modules. Play! only replaces modules loaded
         under rom0: names ( Source/iop/IopBios.cpp:279 ), so a game's own copies of
         SIO2MAN and PADMAN run as emulated code. In RE4 they are 16% of IOP time, and
         libsd is another 7%. Add replacements keyed by the module's internal name, per
         game.
         Capcom's sound engine natively. Its software mixer is 37% of IOP time. The sessions
         judged it not worth doing for the movie; at 30 game frames a second it is about 5 ms
         per frame.
         With C4 as well, the IOP could fall to roughly a third. P3 (its own core) is the
         alternative.

  R4. Bypass vitaGL for the GS renderer
         vitaGL's source confirms the costs: every framebuffer change ends a GPU scene,
         glReadPixels waits for the GPU, glCopyTexSubImage2D is a read-back plus an
         upload, and it exposes no framebuffer read.
         A renderer written directly on GXM controls scenes, uses the GPU blitter and can use
         depth as a texture. Treat it as the end state of G1 to G5. Whether GXM lets a fragment
         program read the framebuffer is still unverified.

  What the research confirmed

     Claim                                          Evidence

     Asynchronous VU1 (P1) works broadly            PCSX2's MTVU.cpp queues unpacks and
                                                    program starts and posts a PATH1
                                                    placeholder, as described. Its game database
                                                    switches it off for 67 entries; RE4 is not one

     COP2 flag skipping (C2) and compile-time       PCSX2 has both ( COP2FlagHackPass ;
     VU flags (V2)                                  microVU's flag pass)

     Fault-based fast memory on the Vita (C1)       Flycast Vita and DSVita use kubridge for it.
                                                     kuKernelMemReserve reserves address
                                                    space without physical pages, so the window
                                                    size can be tested in minutes

     Fourth core (P3)                               CapUnlocker 1.4 lets game-mode apps use
                                                    CPU3; at least one port uses it and falls back
                                                    to three cores

  What it ruled out
         Underclocking as a fix for RE4. See R2.
         PCSX2's "EE cycle skip" (charging the EE emulated time for VU work). It becomes
         possible with P1, but for a game without speed compensation it only hides slowdown.

  Bottom line
  With R1 to R4 on top of the list and B1, a locked 30 on screen at correct game speed is
  arguable on paper for RE4: 15 drawn frames and 15 in-between frames a second, with logic
  at 30 Hz. R2 is the make-or-break step and it is unproven. One hour of profiling the EE in
  gameplay should come before any of it.

  What I read and what I did not
         Repository: rabbithuntyt-ai/Ps-vita-ps2-emu at f7ed38d (branch claude/vita-
         ps2-emulator ): every file in docs/ , the 218-commit history, the frontend and
         renderer source, and the Play! core rebuilt with all 96 patches applied.

         Chats, read directly: the RE4 session from 9 Nov 22:36 UTC to its end on 10 Nov, and
         the cloud session from 6 Oct 17:51 UTC to its end. I read the conversation text; tool
         output only in part.
         Chats, read through recaps only: the earlier hours of both sessions. Their context
         recaps summarize that work but do not quote every earlier user message.
         Not visible from here: the "RE4 30 FPS gameplay" session and the 2–5 November
         hardware session. I saw only their relayed messages and their docs.
         Outside sources: kubridge (bythos14 fork), whose header I read; vitaGL, whose
         source I read; CapUnlocker, from its listing only. The PCSX2 comparisons are from
         memory, not re-read.
```
