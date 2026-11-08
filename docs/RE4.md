# Resident Evil 4 (SLUS-21134) on the Vita: status and notes for other sessions

Goal set by the owner: **a smooth 30 fps in menus, cutscenes and gameplay**, RE4
first. Silent Hill 2's movies are tested with these fixes only once RE4's movie
reaches a real 30 pictures per second.

RE4 is the test case for an emulator meant for every PS2 game: what is specific to it
and how each finding could reach other games is in [LIBRARY_WIDE.md](LIBRARY_WIDE.md).

## Where it stands

Measured on a real Vita, 2026-11-05 (warm JIT cache). "Pictures/s" counts new
movie pictures handed to the game, not flips. A real 30 needs 60 emulated fps.

| Scene | Before this work | Now | Goal |
|---|---|---|---|
| Title screen | 3.3 fps (Nov 3 run), 35 on the build this work started from | **55 emulated fps** (58.9 with `ee_cycle_rate=60 iop_rate=50`) | 60 emulated = 30 real |
| Orange intro (New Game) | black, 3 pictures/s | **picture correct, 14.2 pictures/s**, 30.6 emulated fps. Plays through, "Six years later", then Leon's car cutscene | 30 pictures/s |
| Leon's car cutscene | never reached | picture correct, 13.9 pictures/s | 30 pictures/s |
| Attract trailer (idle at the title) | black, 3 pictures/s | picture correct, 13 pictures/s (peaks of 17), 27 emulated fps. Plays through and returns to the title | 30 pictures/s |
| Real-time 3D (Leon on the path, from a save state) | grey screen or hang | **drawn, with depth. 0.9 fps.** Some textures on Leon are wrong (patches on hair and jacket) | 30 |

Load times, 2026-11-07 (warm JIT cache, wall clock on the Vita):

| Load | Before | Now | What changed |
|---|---|---|---|
| Boot to the title menu | 29.7 s | **8.8 s** | CRI ADX poll loop counted as idle |
| New Game confirmed to the first intro picture | 17 s | **10 s** | SN Systems DLL linker's symbol scans native |
| First button press at the title to the first intro picture | 43 s | 36 s | Same two changes. The rest is menu screens and the memory card check |

### Why the loads were slow

- **Boot (7 to 27 s of black screen).** `DVDLoadPartition` waits for its files with
  `while(ADXF_GetPtStat(i) != 3) ADXM_ExecMain();`. The file comes from the IOP, so it
  needs emulated time, but the EE spent all its cycles running CRI's servers in the
  loop: 5 emulated fps, 75% of the EE in `LSC_ExecServer`, `ADXF_GetStat` and friends.
  `ee_cycle_rate=50` alone brought the title to 19.9 s, which proved it was a wait.
  Now `ADXM_ExecMain` (0x17CB98) starts with an HLE call. From its third call in a
  frame, the rest of the EE's time slice is idle (`IDLE_HLE_POLL`, patch 0076). It never
  fires at the title or in movies. Setting: `adx_idle=0` turns it off.
- **New Game (8 s at 50 emulated fps, nothing on screen).** 58% of the EE was in three
  static functions of SN Systems' DLL linker (0x2A4CB0, 0x2A4DC8, 0x2A4EA0, between
  `_kExitTLBHandler` and `snInitDllSystem`). They resolve each symbol by walking every
  loaded module's table with `strcmp`. Their scans are native now (`PatchSnDll`). The
  relocation routine (0x2A4B70) is still the game's own code, called from a short MIPS
  loop. The intro and Leon's car cutscene still play, 270 s of movie with the game's
  code running behind them (frame dumps, run `dl_long`). The 3D after the cutscene was
  not reached in that run: if the gameplay session sees a crash or missing behaviour
  right after New Game, retest with the linker patch removed first.
- RE4 ships its symbol table in EE RAM: `runs/re4/syms.txt` (5602 names, made by
  `runs/re4/an27.py` from a RAM dump) names most hot addresses.

Not verified: moving Leon (no input tried yet), audio sync in movies, and the movie picture
compared with a reference. Pictures looked right in frame dumps only, apart from one
frame (t=317 of a run) that was mostly black with a blocky patch: maybe a scene cut,
maybe a decoding problem.

The game's real flow: logos, title animation, title menu (about 20 s), then with no
input it plays attract movies. Pressing START and Cross at the title starts a new game:
memory card, "Loading...", the orange intro, "Six years later", Leon's car cutscene.
Cross does not skip these. The two movies differ a lot in speed, so **always say which
one a number comes from**.

## What was wrong, and which games it affects

| Finding | Fix | Reach |
|---|---|---|
| Textures kept right after a frame buffer were sampled from the buffer's render target when its height was rounded up: black picture and a blocking copy per draw | `TARGET::drawnRows`; such textures decode from GS memory. Test: `texture after drawn rows` | Every game with this memory layout |
| Sampling a 448 row buffer with TH=512 re-uploaded undrawn rows into the target every frame (GPU sync) | Rows past `drawnRows` are not filled in | Every game that samples its frame buffer |
| Every written texture re-decoded all its rows, per 8x8 tile | `CTextureCache::Reset` invalidates only the page rows written; `DecodeAll` works row by row | All games |
| `vitaps2_jit_alloc` called `abort()` when the 16 MB code pool filled up. RE4 reaches 11.8 MB before the title | Patch 0069: EE code is dropped at a safe point when under 1.5 MB is free. Tested with `jit_limit_kb=6000` (9 flushes, no crash) | All large games |
| The JIT cache read its whole file into the heap (17 MB for RE4), and a block could only come back once | Patch 0069: index in memory, blocks read from the file on demand. Movie reached at 112 s instead of 137 s | All games |
| The main loop hands over every 4800 EE cycles, about 3000 host cycles of overhead per slice | Patch 0070, `ee_slice`. **19200: title screen 35 to 53 fps.** 38400 breaks RE4 (no display output) | All games, per game setting. Not tested on other games |
| Native movie decoding only knew libmpeg 2.2 | libmpeg 3.0 in both output modes, NEON RGB32 conversion | Games on libmpeg 3.0, including Sofdec ones. Only RE4 tested |

## Real-time 3D: three things stood in the way

1. **Threaded VU1 hangs.** At the first real-time scene the game waits forever for DMA
   channel 1 (0x251928: `bc0f` loop on `D_PCR`), running at 59 emulated fps with no output
   and sound playing. Same as open bug 1 in HLE_MPEG.md. `threaded_vu1=0` in the profile
   for now. The root cause is still open, and it costs a core's worth of speed.
2. **`MAX_TEXTURES` was 24, and GL textures were dropped after 64 binds.** A scene uses
   65 or more textures per frame and does hundreds of binds: everything was decoded again on
   every frame. Now 512 textures or 48 MB.
3. **Depth on the Vita does not survive a new GPU scene.** vitaGL stores depth but does not
   load it, so after a target switch, a read-back or a copy, depth starts at 1.0 and every
   GEQUAL test fails: 8700 triangles, a grey screen. Depth is now cleared to 0 when a target
   is drawn with depth after such a break. Things drawn earlier in the frame no longer hide
   later ones, so **every break also risks wrong overlaps. Fewer breaks fixes both this and
   the speed.** Found with `gs_debug=1` (no depth test), which showed the world at once.

### Handoff to the gameplay session

Gameplay has its own session from 2026-11-07; this one keeps movies, loading and menus.
Where it stands:

- About 1 fps, GPU-bound: `gpu_tex` 1190 ms/s, the emulation thread mostly waiting for
  the GS. Per frame: 625 draws, 13 read-backs (`D`), 12 uploads (`U`), 4 texture decodes.
- Done: textures of alpha values (PSMT8H/4HL/4HH) do not read the frame back when the
  draws since kept alpha (`TARGET::alphaDirty`). RE4 grades its colours by drawing the
  frame over itself in 16 strips through a palette indexed by the frame's alpha: 28 -> 13
  read-backs per frame, `gpu_tex` 1650 -> 1190 ms/s, same picture.
- Next, from `gs_diag` (`runs/re4/dg2.py` tallies a frame's `D`/`U`/`M`):
  - 0x310000 is drawn as 2, 4 and 8 pages wide, formats 0 and 2; 0x2D8000 as 1 and 4.
    Each layout replaces the other with a read-back and an upload. Let them coexist.
  - The Z buffer (0x1C0000) is sampled as a texture (`M1C0000/8/31`, PSMZ24) for an
    effect: depth is never read back from the GPU, so this reads stale memory.
  - Wrong textures on Leon (patches on hair and jacket).
- Start from a state: `save_state_at=570` in a New Game run (scripted with `pad_script`)
  saves one just before the first real-time scene; `runs/re4/state_before_gameplay.state`
  is a copy. `threaded_vu1=0` is needed (see above).

A state from just before this scene saves 10 minutes per test: `save_state_at=570` during
a New Game run, then `load_state=1`.

## Native movies under Sofdec: what to know

Details of libmpeg 3.0 (RE4 addresses, found by code signature):

- `sceMpegGetPicture` has two entry points: 0x2E73C8 (`sys+0xC4 = 1`, RGB32, the one
  RE4 uses) and 0x2E7410 (`sys+0xC4 = 0`, RAW8). Buffer `sys+0xEC`, macroblocks
  `sys+0xF8`. The mp fields are as in 2.2.
- Callback table: 12 byte entries (function, data, **gp**) from `sys+0x0C`. No-data is
  type 1: `sys+0x18/0x1C/0x20`. `gp` has to be set for the call.
- Abort flag: `sys+0x878`.
- **Sofdec's no-data callback (0x2CE3F0) takes an empty ring for the end of the movie:**
  it queues an end code, and the next call aborts. So do not call it while the decoder
  has data: wait for the picture.
- **Sofdec's DMA chain loops over its whole 512 KB ring** (255 REF + 1 REFE). The IPU
  only pulls what it needs, so hardware never overtakes the writer. A greedy input sink
  does: 98% of what the decoder got was stale, and it replayed about 48 old pictures
  while the fps counter looked good. Input must be on demand. Check with the stream dump
  (below): no 2 KB unit should equal the unit 524288 bytes earlier.
- Sofdec stops DMA4 between pictures, so data for decoding ahead must arrive during the
  previous `GetPicture` call (the input window, 24 KB).
- A picture can be larger than the window: a starved decoder always gets more, or it
  deadlocks with "no picture (5001 waits)".
- **It is Sony's ezmpeg `ViBuf`** (HLE_MPEG.md), at player +4376: ring, tags, units,
  first queued unit, units queued. Input never goes past the units queued, so the decoder
  can read ahead safely. A fixed window without this read old data at the end of a movie:
  50 decoder errors and timeouts.
- The no-data callback also refreshes the DMA feed, so it has to keep running. It can
  only end the movie while a flag set by its caller is on (player +4264 +20). At the start
  of a stream the decoder runs dry while the disc catches up: ending the movie then left
  the game on a black screen after "Six years later". The flag now stays off for 120
  vblanks of no data.

## How the time splits now (orange intro, 30.6 emulated fps)

The PS2 thread is on the CPU 94% of the time, so what is left is real work: **about 33 ms
per vblank against 16.6 ms for full speed, 2x**. By the profiler: EE 73%, IOP 17%, other
7%. GPU thread 69%, decoder about 45 ms per picture of wall time on core 2 and the EE does
not wait for it.

**The largest gain was not a CPU cost.** The PS2 thread slept a third of the time, with
spare CPU on every core: 335 ms per second waiting for SPU2 mixing on register reads,
because the mixer shared core 2 with the decoder at equal priority (threads start at 191,
the lowest). Mixer at priority 160: 10.2 to 14.2 pictures/s. Found by timing every wait
(`spu_wait_ms`, `mailbox_wait_ms`, `limiter_ms`, `disc_ms`, `vu_sync_ms`, `mpeg_wait_ms` in
`perf.csv`). The section profiler showed 1.3% for it, so do not trust that one for waits.

EE samples (`ee_hot_after`), done and left:

| Share | Code | Status |
|---|---|---|
| About 25% | 0x17BEA8/0x17BF30: critical sections, about 5 kernel calls per pair, 2700 pairs/s | Done: a counter in guest code (`thread_lock_hle`) |
| 12.8% | 0x2AF400: memcpy's byte loop (Sofdec copying video out of sectors) | Done: native |
| 11.8% | 0x2DADC0: Sofdec scans for start codes byte by byte | Done: native (now 2%) |
| About 9% | 0x264038: room item flag slot (`GetEtcFlgPtr`, `SceAtItemFindFlgCk`...), counts entries in 10 room tables through `checkRoomRange` | Tried native: **no gain** (see below), reverted |
| About 6% | 0x298200-0x2984FF: `PS2OT` exec, reset, create (ordering tables) | Not looked at |
| About 5% | `IdCalcVertex` (0x21D790) | Not looked at |
| About 2% | `ADX_DecodeSte4AsSte`: CRI ADX audio decoding | Not looked at |

### Cutting EE work does not raise the movie rate (2026-11-07)

Measured warm on the Vita, intro window 60-110 s. Pictures/s is always half the emulated
fps (Sofdec shows one picture per 2 vblanks), so the goal is 60 emulated fps.

| Change | Pictures/s | Emulated fps | What moved |
|---|---|---|---|
| Baseline | 14.1 | 29.3 | EE 0.71, IOP 0.15 of the PS2 thread |
| Room flag lookup native (9% of EE samples) | 14.1 | 29.3 | Its samples went to CRI's servers (`ADX_DecodeSte4AsSte`, `SJRBF_*`, `SJX_Destroy`): the EE still runs its whole cycle budget per frame |
| CRI MW idle thread (`adxm_mwidle_proc`, 0x17C318) limited to 2400 cycles per slice, the rest idle | 14.1 | 29.4 | EE 0.71 to 0.63, **IOP 0.15 to 0.24**: the same 32 ms per emulated frame |
| Same with `iop_rate=50` | 13.8 | 30.7 | IOP still 0.2 |
| `ee_cycle_rate=60` | 13.7 | 31.0 | The game's frame work does not fit: fewer pictures per vblank |
| Decoder on core 1 (`mpeg_core=1`) | 11.6 | 24.1 | Decoder 88 ms per picture, EE waits 230 ms/s for pictures |

What this means:
- The PS2 thread costs about 32 ms per emulated frame whatever is removed from the EE.
  Time saved on the EE shows up in the IOP. That transfer is **not explained yet**, and
  it is the next thing to understand: profile the IOP (which modules, which loops) with
  and without the MW idle rule.
- **The native decoder is the next limit after that.** It takes about 55 ms of wall time
  per picture on core 2, which it shares with the SPU2 mixer (17%). That is 78% of the
  core at 14 pictures/s, so it caps out near 18. 30 pictures/s needs under 33 ms per
  picture: faster IDCT/motion compensation in NEON, or the two fields of a picture on two
  cores.
- Neither code change was kept: no measured gain, and the room lookup is game code that
  gameplay depends on.

## Ideas not done yet

1. Find where the IOP's time goes in movies (above): it grows when the EE gets cheaper,
   and does not shrink with `iop_rate`.
2. A faster native MPEG decoder (above). Helps every movie game.
3. 0x298xxx (`PS2OT`), `IdCalcVertex`, native ADX.
4. Tier 2 recompilation (AOT_TRANSLATION.md). After the items above it is probably what
   stands between about 20 and 30 pictures/s.
5. Convert pictures straight into a GL texture, skipping EE RAM and the GIF: each picture
   is copied to EE RAM, then to the GS in 16 pixel columns.
6. Larger slices only while EE and IOP are both idle. 38400 broke RE4 but gave 58.5 fps.
7. Reach gameplay, then make a save state for repeatable measurements.

## Tools added

Autostart keys, all per run:

| Key | Does |
|---|---|
| `ee_slice=N` | EE cycles per time slice (also a per-game setting) |
| `iop_rate=N` | IOP clock in percent (also a per-game setting) |
| `jit_limit_kb=N` | Treat the JIT pool as this small, to test the flush |
| `pad_script=T:btn,T:btn` | Hold a button for the game 400 ms at second T (start, cross, up...). Works when Vita Lab's presses do not reach it |
| `save_state_at=S` | Save a state at second S; `load_state=1` starts from it |
| `gs_debug=N` | 1 no depth test, 2 no alpha test: shows what a test is hiding |
| `dump_targets_at=S` | Every render target to `frames/` as images |
| `frame_dump_every=S` | Save the displayed picture to `ux0:data/VitaPS2/frames/` |
| `mpeg_dump_mb=N` | Save the movie stream the decoder gets to `mpegstream.bin` |
| `gs_diag_after=S` | Start `gs_diag` logging later (with `gs_diag=N`) |
| `ee_hot_after=S` | Sample EE hotspots from this second on; also writes `syscalls.txt` (kernel calls by number) |
| `mpeg_core=N` | Core of the movie decoder (default 2) |
| `thread_lock_hle=1` | RE4 USA: critical sections without kernel calls (also a per-game setting) |

`perf.csv` gained `jit_pool_kb`, `jit_flushes`, `mpeg_pictures`, `tex_decode_ms`,
`tex_gl_ms`, `mpeg_wait_ms`, `mpeg_decode_ms`, `mpeg_starved`, `disc_ms`, `disc_reads`,
`vu_sync_ms`, `spu_wait_ms`, `mailbox_wait_ms`, `limiter_ms`. `tests/mpeg2_decoder_tests FILE --es` replays a stream dump on a PC.

## Traps when testing

- Judge movies by `mpeg_pictures` and frame dumps. The fps counter rose to 35 while the
  movie was actually stalled, and to 22 while it was replaying old pictures.
- Every build invalidates the JIT cache: the first run takes about 2x as long to reach
  a scene. Compare warm runs only.
- `vitalab ls` wraps sizes at 4 GB, so large images look truncated.
- `vitalab shot` and `stats` report SHELL at 0 fps in some sessions while the emulator
  runs, and button presses then do not reach the game. Use `frame_dump_every`.
- Builds in WSL must record patches applied by hand in the submodule's
  `vitaps2-applied-patches`, or CMake's reconfigure fails. A failed build then tests the
  previous program without any warning: check that it says `built:`.
- Button presses on the Vita change the timeline of a run.
- `ux0:data/VitaPS2/settings/Resident Evil 4 (USA).iso.ini` holds `ee_cycle_rate=130`,
  `safe_jit=1` and `interlaced=1` from an earlier experiment. They override the game
  profile and slow the game down.
- `mpeg_dump_mb` slows a run (it writes to the card on the emulation thread). Do not
  measure speed with it on.
- Uncommitted, on purpose: the "60 frame" change to `FindTarget` from HLE_MPEG.md item 3
  is still only in the working tree. All numbers here were measured with it in the build.
