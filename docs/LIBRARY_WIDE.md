# From one game to every game

VitaStation2 is an emulator for the whole PS2 library. Resident Evil 4 is the current
test case ([RE4.md](RE4.md)): each thing done for it is listed here with how far it
reaches, and what it would take to reach further. Update this file when a game-specific
fix is added or generalised.

Only RE4 has been measured with this work. "All games" below means the code is not
specific to RE4, not that other games were tested. The next step for all of it is a run
of `tools/vitalab_compat.sh` across the test set.

## Already general: helps any game, on by default

| Change | What it does | Expected reach |
|---|---|---|
| `TARGET::drawnRows` in the GPU renderer | Textures kept after a frame buffer are not mistaken for it. Undrawn rows are not re-uploaded when a buffer is sampled | Any game with this memory layout, or that samples its frame buffer with a power-of-2 TH larger than the buffer (common for full-screen effects) |
| `CTextureCache::Reset`, `DecodeAll` | Only written page rows are decoded again; rows decoded whole; CT32 pages in memory order | Any game that updates large textures. RE4: 277 to 126 ms/s |
| JIT pool flush (patch 0069) | A full code pool drops EE code instead of crashing | Any large game. Tested by forcing it on RE4 |
| On-demand JIT cache (patch 0069) | Cache file indexed, blocks read when needed, so the file (17 MB for RE4) no longer sits in the heap at boot | Every game |
| **SPU2 mixer priority above other core 2 work** | Register reads no longer wait for a movie picture to finish. RE4's intro: 10.2 to 14.2 pictures/s | Every game with `mpeg_hle`. Silent Hill 2 has the same layout, so test it there first when the time comes |
| Wait timers in `perf.csv` | Every place the emulation thread sleeps is timed. Found the largest gain so far | Every game: **look at them first**. A thread below 90% CPU with no fast game means a wait, not a CPU limit |
| `pad_script` | Scripted buttons for the game in autostart runs | Lets the compatibility harness get past menus without Vita Lab |
| Diagnostics (`frame_dump_every`, `ee_hot_after`, `syscalls.txt`, `mpeg_dump_mb`...) | See [RE4.md](RE4.md) | Finding the next bottleneck in any game |

## General code, off until a game enables it

These need a per-game choice today. The job is to make that choice automatic.

| Setting | RE4 result | The risk | How to make it work for every game |
|---|---|---|---|
| `ee_slice` (default 4800) | 19200: title 35 to 53 fps. 38400: **no display output** | Too long breaks games, and how long depends on the game | Add it to `CAutoTuner`: step up on first boot, step back when output stops or fps falls, and store the result per game. A stopped display is easy to detect (`vm_fps` high, `out_fps` 0). Longer slices only while EE and IOP are both idle may be safe everywhere |
| `iop_rate` | 50: small gain on the title, none in movies | Slow sound and disc drivers | Probably not worth automating: the IOP's cost barely moved in RE4. Try on other games first |
| `ee_cycle_rate` | 60: title 53 to 57 fps | Game logic slows down in busy scenes | Auto EE rate exists. Check that it climbs back fast enough in gameplay |
| `mpeg_hle` | 3 to 13 pictures/s, a whole trailer plays through | A stream the decoder cannot handle, or a library variant not recognised | Turn on by default once a few more games pass, with fallback to the IPU when the signature does not match (already the case) |

## RE4 only today, and how each could generalise

| RE4-specific work | Why it is specific | The general form |
|---|---|---|
| **Critical sections** at 0x17BEA8 and 0x17BF30 replaced by a counter (`thread_lock_hle`, kernel hold-back in patch 0073). A quarter of the EE's time in movies | Exact code match on the US release. The port's own "disable interrupts" | 1. The kernel side (`g_dispatchLockAddress`) is already generic: a game only supplies a counter address. 2. Other Capcom ports of that era may share this code: search for its signature in other discs. 3. **Look at `syscalls.txt` in every game.** Over a thousand kernel calls per second of the same few numbers means a pattern worth replacing. 4. Cheaper kernel calls help all games: `GetThreadId` and `ReferThreadStatus` need no exception path and no reschedule |
| **Start code scan** at 0x2DAD80, native (12% of EE time) | Exact match, including the jal in the middle | Part of Sofdec, not RE4. Sofdec is in many PS2 games, so other games likely have this routine. Mask the jal target and every game with this Sofdec build gets it. Needs other discs to find variants |
| **Sofdec's input rules** (on-demand input, wait for the picture) | Only applied to libmpeg 3.0 | Applies to anything that loops its DMA chain over a ring. libmpeg 2.2 games with `viBuf` end the chain, so they do not need it. Check any game that replays pictures: same 2 KB units 1 ring apart in `mpeg_dump_mb` |
| **memcpy's byte loop**, native | Exact match, but it has no absolute addresses | Sony's C library, so any game built with this SDK should match. 13% of EE time in RE4's intro. Other SDK versions need their own signature. It could be enabled for every game, not only with `mpeg_hle` |
| **Stay within `ViBuf`'s queued units** (input never reads old data) | The address comes from this Sofdec's player, found after an exact match of its callback | The structure is Sony's ezmpeg sample: Silent Hill 2 has it at 0x1211E40. Other games need a way to find it: it is the data pointer the no-data callback gets, or close to it |
| **End of movie held back while the decoder has no data** (flag at player +4264 +20) | This Sofdec build | Other Sofdec builds need the same flag found, or a short movie ends early and the game can stay on a black screen. **If a Sofdec game goes black after a movie, look here** |
| **libmpeg 3.0 signatures** | Taken from RE4's build | Should match other games on the same SDK release (`PsIIlibipu 3000` in EE RAM), including non-Sofdec ones. RAW8 mode (0x2E7410 here) is written but has **never been run** |
| `[SLUS-21134]` in `assets/game_profiles.ini` | One serial | The PAL and Japanese releases need their own entries and their own lock signatures. Their `ee_slice` should carry over |

## The method, for any game

1. `tools/vitalab_compat.sh`, or a run with `frame_dump_every=15`. Look at the frames: fps
   counters have been wrong twice in RE4.
2. Is the PS2 thread or the GPU thread at the limit? (`threads` in `perf.csv`)
3. GPU thread: `gs_diag=N gs_diag_after=S` shows per frame what gets uploaded (`U`),
   read back (`D`), sampled (`T`) and decoded (`M`). A repeated `U` or `D` is a stall on
   every frame.
4. PS2 thread under 90%: find the wait in the wait timers (`spu_wait_ms`...). At 90% or
   more: `ee_hot_after=S` gives EE hotspots and `syscalls.txt`. Disassemble with
   `mips-linux-gnu-objdump -D -b binary -m mips:5900 -EL` on a `dump_ee_ram=1` dump.
5. Library code goes in by signature (several games gain). Game code goes in by exact
   match behind a setting (only that game, no risk to others).
6. Note it here.

## What the numbers say about the whole library

- **Scheduling overhead was the largest single cost in RE4's menus**, more than any game
  code. This is true of the emulator, not of RE4, so expect most games to gain from
  `ee_slice`.
- **Full speed needs about 16.6 ms per vblank.** RE4's movie costs about 36. Replacing
  routines natively looks like it can reach about 25 ms. Going further probably needs
  tier 2 recompilation of EE and IOP code ([AOT_TRANSLATION.md](AOT_TRANSLATION.md),
  estimated 1.5-2.5x), which would help every game. These two figures are estimates.
- **Movies:** the native decoder takes about 28 ms of CPU per 512x336 picture. Any movie
  game should now be limited by the rest of the emulator, not by decoding, provided nothing
  shares its core at equal priority.
- **Check for sleeping before optimising.** In RE4's intro a third of the time was a wait,
  with idle CPU on all three cores, and no hotspot pointed to it.
