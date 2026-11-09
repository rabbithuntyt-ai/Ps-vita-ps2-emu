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
| **Depth restored after a GPU scene break (Vita)** | vitaGL does not load depth, so it was 1.0 after any target switch or read-back and GEQUAL failed. In RE4: grey screen instead of the world | **Probably every 3D game with depth and render-to-texture or read-backs on the GPU renderer.** Expect wrong overlaps where a frame has breaks in the middle of its 3D. A real fix would keep depth across scenes (a change in vitaGL, or drawing the world's breaks less often) |
| **Alpha textures without read-backs** (`alphaDirty`) | Sampling a frame as PSMT8H/4HL/4HH only needs alpha: no read-back after draws that kept alpha. RE4: 28 -> 13 read-backs per frame | Every game that grades colours or does effects through an alpha-indexed palette, a common PS2 trick |
| **Texture cache of 512 textures or 48 MB, GL textures kept longer** | 24 textures and 64 binds were enough for menus only | **Every 3D game.** Heap use grows: watch `heap_used_mb` on large games |
| **SPU2 mixer priority above other core 2 work** | Register reads no longer wait for a movie picture to finish. RE4's intro: 10.2 to 14.2 pictures/s | Every game with `mpeg_hle`. Silent Hill 2 has the same layout, so test it there first when the time comes |
| Wait timers in `perf.csv` | Every place the emulation thread sleeps is timed. Found the largest gain so far | Every game: **look at them first**. A thread below 90% CPU with no fast game means a wait, not a CPU limit |
| `pad_script` | Scripted buttons for the game in autostart runs | Lets the compatibility harness get past menus without Vita Lab |
| Diagnostics (`frame_dump_every`, `ee_hot_after`, `syscalls.txt`, `mpeg_dump_mb`...) | See [RE4.md](RE4.md) | Finding the next bottleneck in any game |
| **Indirect jumps resolved in compiled code** (patches 0077/0078, `indirect_jit`) | Function returns (`jr ra`) and unlinked branches no longer go back to the C++ loop: the chained thunk looks the target up in a per-CPU table. RE4's movie: 89% fewer returns to the executor, +2.5% emulated fps | **Every game** (EE and IOP code). Most for C++ engines with many small functions. First step of tier 2 (docs/AOT_TRANSLATION.md) |
| **Kernel calls inside the time slice** (patch 0084, `iop_stay_in_slice`, `ee_stay_in_slice`) | A kernel call (HLE) or interrupt check no longer ends the CPU's run: `ExecuteCpu` handles it and goes on with its quota, so the VM loop's tick bookkeeping (counters, BIOS/module timers, DMA) runs once per slice instead of after every call. RE4 movie: IOP +2.3%, EE +1.3%. New profiler sections show the split (`ps2_EEkernel`, `ps2_IOPticks`...) | **Every game**, most for those calling IOP/EE kernel functions often (sound engines' critical sections) |
| **Fast RAM path, likely branch exits, memory exceptions in the quota** (patches 0086-0089, `fast_ram`, `cheap_exits`) | Loads and stores in RAM skip the page table (4 ARM instructions for `lw` instead of 9) with the slow paths moved after the function; blocks ending in likely branches exit without the delayed jump field; loads and stores no longer make block exits test for exceptions. RE4's movie: 16.2 -> 18.35 pictures/s, EE code -22% and IOP code -27% per frame | **Every game** (EE and IOP code on the Vita). Test note: toggling `fast_ram` changes the JIT cache fingerprint, so the next run is cold |
| **Movie pictures to the GS without a copy** (patch 0090, `movie_zero_copy`) | RGB32 pictures from `mpeg_hle` that the game sends to the GS (image transfers from where a recent picture was put in EE RAM) are handed to the GS thread as the decoder's buffer instead of a copy. RE4: +6% | Every game with `mpeg_hle` RGB32 movies (libmpeg 3.0 RGB32). A game that edits the picture in EE RAM before sending it would show the unedited one: check the frames |
| **SPU2 voice levels read without pausing the mixer** (patch 0091, `spu_fast_reads`) | ENVX/VOLX reads skip the wait for the mixer thread when no write is queued. RE4 +0.7% | Every game whose sound driver polls voice envelopes (common) |
| **Pinned context fields** (patch 0082, `pinned_fields`) | The cycle quota and the memory page table pointer stay in ARM registers (r9, r10) across linked blocks: no load/store of the quota per block, no load of the table per memory access. RE4 movie +2% | **Every game** (EE, IOP and VU blocks on the Vita) |
| **Cheaper block exits** (patch 0081, `cheap_exits`) | Blocks ending in a jump skip the "was the jump taken" test; blocks that cannot raise an exception skip that test. A hot block's exit: 17 -> 11 ARM instructions. RE4's movie: +1.8% emulated fps | **Every game** (EE and IOP code). Testing note: an IOP-heavy boot on the Vita is part of the check (the fuzzer is EE only; `addiu zero, zero, N` is an IOP syscall in Play) |
| **One test for a new game** (`tools/vita_game_test.sh`, `tools/game_report.py`) | Warm-up, measured run, report with verdict, phases, costs, hotspots and frames | Every game: run it first |
| **IOP hotspots** (`iophot.txt` with `ee_hot_after`, `iopram.bin` with `dump_ee_ram=1`) | Same sampler as the EE's, on the IOP's PC | Any game where `ps2_IOP` is large: shows which IOP module (sound driver, pad, disc) costs the time |
| **CRI ADX file waits count as idle** (`adx_idle`, patch 0076, `PatchAdxmExecMain`) | `while(ADXF_GetStat(h) != done) ADXM_ExecMain();` burns the EE while the IOP reads. From the third `ADXM_ExecMain` call in a frame, the rest of the slice is idle. RE4's boot: 29.7 to 8.8 s | **Every game using CRI ADX (Capcom, Sega, Namco and many more), but only with `mpeg_hle` on today.** Found by its call chain (`ADXM_ExecSvrAll`: 7 calls of one-line `SVM_ExecSvr*` functions), not by address. A game polling `ADXM_ExecMain` 3+ times per frame while it has real work would lose EE time: watch the last `idle_*` count in `perf.csv` |
| **SN Systems DLL linker's symbol scans native** (`PatchSnDll`) | ProDG's `snDll` resolves symbols by walking every module's table with `strcmp`. RE4's New Game load: 17 to 10 s | **Games built with ProDG that load DLL modules**, if their `snDll` is the same build (exact match, addresses masked). Other builds need their own signature. Only with `mpeg_hle` on today |

## Known problem for every 3D game

**Threaded VU1 hangs** at RE4's first real-time scene and in Silent Hill 2 (HLE_MPEG.md, open
bug 1): the EE waits for VIF1 forever, with no output. The signature in `perf.csv`: `vm_fps`
near 60, `out_fps` 0, no draws. It is on by default for Silent Hill 2 and through
`CAutoTuner` elsewhere, so any game can hit it. Worth a root cause: it frees a whole core
for VU1 work, which 3D games need.

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

**`CMpegHle` is now the home of every guest-code replacement**, not only movies: memcpy,
the ADX wait, the DLL linker. They need `mpeg_hle=1` only because that is where the
syscall hook is installed. Next step for the whole library: split them into a "game HLE"
class that is on for every game, with each replacement still behind its own signature.

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
- **Load screens are often a wait the EE spins through.** If a black load screen has the EE
  near 100% with `vm_fps` low and few disc reads, try `ee_cycle_rate=50`: if the load gets
  shorter, the game is waiting for time (IOP, disc), not working. Find the poll loop with
  `ee_hot_after` and end the slice there. If `vm_fps` is near full speed instead, the load
  is real EE work: look for a library routine to make native (RE4: the DLL linker).
- **Less EE work is not always more speed.** In RE4's movie, removing 9% of the EE's work
  changed nothing: CRI's MW idle thread used the freed cycles, and when that thread was
  made idle the IOP took the time instead. Before replacing a routine, check that the EE's
  share of the PS2 thread falls *and* `vm_fps` rises (an A/B run, warm JIT cache).
- **The native MPEG decoder is faster for every movie game**: NEON IDCT (bit exact),
  a one-load bit reader, MPEG-2 escape codes without the table search, recycled picture
  buffers. 512x336 pictures: 35 to 31 ms of CPU, 55 to 42 ms of wall time on core 2.
  Enough for about 26 pictures/s next to the SPU2 mixer.
- **Check for sleeping before optimising.** In RE4's intro a third of the time was a wait,
  with idle CPU on all three cores, and no hotspot pointed to it.
