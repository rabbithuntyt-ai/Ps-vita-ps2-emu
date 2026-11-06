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
| Title screen | 3.3 fps (Nov 3 run), 35 on the build this work started from | **53 emulated fps** (58.9 with `ee_cycle_rate=60 iop_rate=50`) | 60 emulated = 30 real |
| Orange intro / attract movies | black, 3 pictures/s | **picture correct, 12 pictures/s**, 27 emulated fps at best | 30 pictures/s |
| Gameplay | not reached yet | not reached yet | 30 |

Not verified: gameplay, New Game, audio sync in movies, and the movie picture
compared with a reference. Pictures looked right in frame dumps only.

The game's real flow: logos, title animation, title menu (about 20 s), then with no
input it plays attract movies. **The orange intro is an attract movie**, and Cross
does not skip it. A "Loading..." hang with no output was seen once after someone
pressed through the menus by hand; it has not been reproduced on purpose.

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

## How the time splits now (movie, per vblank at 25 emulated fps)

EE about 20 ms, IOP 9 ms, other 7 ms: 36 ms against 16.6 ms for full speed, so **the
movie needs more than 2x**. GPU thread 60%, not the limit any more. Decoder about 28 ms
per picture on core 2 (35 pictures/s at most, on a core it shares with SPU2 mixing).

EE samples in the movie (`ee_hot_after`):

| Share | Code | Idea |
|---|---|---|
| 11.8% | 0x2DADC0: Sofdec scans for start codes byte by byte | Native replacement by signature |
| 4.9% | 0x2AF420: byte-wise memcpy | Native replacement by signature |
| 15.8% | 0x29Cxxx, 0x298xxx: kernel stubs | Look for spin waits |
| 10% | 0x17Bxxx, 0x18Bxxx, 0x19Bxxx: game code | - |

## Ideas not done yet

1. HLE by signature for the start-code scanner and memcpy (ROADMAP item 2).
2. Native ADX audio decoding, if it shows up in the profile.
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
| `frame_dump_every=S` | Save the displayed picture to `ux0:data/VitaPS2/frames/` |
| `mpeg_dump_mb=N` | Save the movie stream the decoder gets to `mpegstream.bin` |
| `gs_diag_after=S` | Start `gs_diag` logging later (with `gs_diag=N`) |
| `ee_hot_after=S` | Sample EE hotspots from this second on |

`perf.csv` gained `jit_pool_kb`, `jit_flushes`, `mpeg_pictures`, `tex_decode_ms`,
`tex_gl_ms`. `tests/mpeg2_decoder_tests FILE --es` replays a stream dump on a PC.

## Traps when testing

- Judge movies by `mpeg_pictures` and frame dumps. The fps counter rose to 35 while the
  movie was actually stalled, and to 22 while it was replaying old pictures.
- Every build invalidates the JIT cache: the first run takes about 2x as long to reach
  a scene. Compare warm runs only.
- `vitalab ls` wraps sizes at 4 GB, so large images look truncated.
- `vitalab shot` and `stats` report SHELL at 0 fps in some sessions while the emulator
  runs. Use `frame_dump_every`.
- Button presses on the Vita change the timeline of a run.
- `ux0:data/VitaPS2/settings/Resident Evil 4 (USA).iso.ini` holds `ee_cycle_rate=130`,
  `safe_jit=1` and `interlaced=1` from an earlier experiment. They override the game
  profile and slow the game down.
- Uncommitted, on purpose: the "60 frame" change to `FindTarget` from HLE_MPEG.md item 3
  is still only in the working tree. All numbers here were measured with it in the build.
