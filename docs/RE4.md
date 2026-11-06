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

Where the 0.9 fps goes: GPU thread 1600 ms/s in `gpu_tex`, the emulation thread 83% waiting
for the GS, 640 draws, 28 read-backs, 12 uploads and 19 texture decodes per frame. Targets
at the same address with different widths and formats (0x2D8000 as 1 and 4 pages wide;
0x310000 as 2, 4 and 8, formats 0 and 2) replace each other, reading back each time: the
first thing to fix.

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
| About 7% | 0x264000-0x2643FF | Not looked at (0x2643C0 is called in a loop per 10 items) |
| About 5% | 0x298200-0x2984FF: a list of callbacks walked | Not looked at |
| About 5% | 0x19B000: looks like ADX audio decoding | Native ADX by signature |

## Ideas not done yet

1. 0x264xxx and 0x298xxx above; native ADX; the IOP's 17-25% (it does not shrink with
   `iop_rate`).
2. Tier 2 recompilation (AOT_TRANSLATION.md). After the items above it is probably what
   stands between about 20 and 30 pictures/s.
3. Convert pictures straight into a GL texture, skipping EE RAM and the GIF: each picture
   is copied to EE RAM, then to the GS in 16 pixel columns.
4. Larger slices only while EE and IOP are both idle. 38400 broke RE4 but gave 58.5 fps.
5. Reach gameplay, then make a save state for repeatable measurements.

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
