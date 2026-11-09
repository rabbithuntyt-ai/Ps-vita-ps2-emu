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

### The IOP in movies (2026-11-07, `iophot.txt`)

`ee_hot_after` now also writes IOP hotspots (`iophot.txt`), and `dump_ee_ram=1` also
writes `iopram.bin`. Disassemble with `-m mips:3000`. To find which module an address is
in, look for the `0x41E00000` import tables at the start of each module and the strings
after them (`runs/re4/c19.py`, `c20.py`).

| IOP share | Module | What |
|---|---|---|
| 37% | **Capcom Sound Engine** (CSE Ver.0.13, 0x7B77C-0x903E0) | Software mixing: 16-bit samples times volume `>> 8`, added into a mix buffer (0x87FE0, 0x88BE0). Its own critical section counter (0x8ABA8 enter, 0x8ABF4 leave, calling `CpuSuspendIntr`/`CpuResumeIntr`) |
| 16% | `sio2man`/`padman` region (0x5FCA0-0x73780) | Pad polling |
| 7% | 0x200 | The HLE IOP kernel's stubs |
| 7% | libsd region | Sony's sound library |

The IOP is 15% of the emulation thread, so the whole CSE is about 5%. No single IOP item
is worth a native replacement for the movie. CSE is in other Capcom games of 2005 (worth
a signature if one of them is IOP bound). Why the IOP's time grew when the EE's MW idle
thread was cut is still open.

### The native decoder, faster (2026-11-07)

Measured over the same 22 s of the intro (warm), the decoder thread now in `threads`
(`MPEG decoder`) and its stages in `mpeghle.txt` (`stages`, sampled by the profiler,
which now runs above the decoder: at equal priority it could not sample it at all).

| Build | Decoder CPU per picture | Wall time per picture |
|---|---|---|
| Before | 35 ms | 52-56 ms |
| + NEON IDCT, bit reader with one load, no 688 KB clear and no log line per picture | 31.8 ms | 45 ms |
| + MPEG-2 escape codes decoded directly (about 2000 per picture took the bit by bit table search) | **30.9 ms** | **42 ms** |

- The NEON IDCT gives exactly the integer IDCT's results: the start-up self-test compares
  them on the Vita (0 mismatches), `idct_tests` compares the scalar copy with
  Framework's on 200000 blocks, and a whole stream decodes to the same bytes as before.
- Stage split of the decoder's busy time: parsing about 45%, output (CSC into libmpeg's
  layout) about 20%, IDCT and prediction about 15% each.
- Core 2 also runs the SPU2 mixer (17%), so the decoder can now keep up with about 26
  pictures/s. **It is no longer the next limit; the emulation thread (14) is.** For 30:
  parsing (a cheaper bit reader in `DecodeMacroblock`) and the CSC.

### State on 2026-11-08 (the profiler fixed, tier 2 started)

With the profiler sampling fairly (it was starved on core 2 before, which made the
EE look larger and produced the "IOP takes the EE's time" artefact), the intro is at
about 31.5-33 emulated fps, 15-15.3 pictures/s. The emulation thread per emulated
frame: EE about 53%, IOP 21%, other 10%, waits (mostly SPU2 sync) 5%, about 32 ms in
all against 16.6.

- Indirect jumps in compiled code (tier 2 step 1): +2.5%, on by default.
- Regions (tier 2 step 3a): no gain on RE4, off by default.
- The CPU is already at 444 MHz (main_vita.cpp).
- `tools/jit_arm_report.py runs/re4/ih1`: RE4's hot blocks are 2-6 EE instructions;
  each block's exit costs about as much as its instructions. The large remaining
  lever is keeping guest registers in host registers across blocks (docs/AOT_TRANSLATION.md
  step 3b), which needs work in the Jitter's register allocator.

### Where the emulation thread's time goes, finer (2026-11-08, patch 0084)

New profiler sections split the old "EE", "IOP" and "other" (perf.csv `ps2_EEio`,
`ps2_EEkernel`, `ps2_EEticks`, `ps2_IOPio`, `ps2_IOPkernel`, `ps2_IOPticks`). Intro
movie, share of the emulation thread (31 ms per emulated frame):

| Part | Before | After the changes below |
|---|---|---|
| EE compiled code and executor | 45.9% | 48% |
| IOP compiled code | 13.1% | 13.5% |
| IOP tick bookkeeping (root counters, BIOS/module timers, SPU2 DMA) | 7.3% | 4.8% |
| EE kernel calls (HLE) | 6.9% | 7.8% |
| IOP kernel calls and interrupts | 6.8% | 6.3% |
| VIF1 DIRECT to the GS | 5.7% | |
| EE tick bookkeeping (timers, DMA, VIF/GIF/IPU) | 3.5% | 2.2% |
| SPU2 register reads, SPU, other | 7% | |

The tick bookkeeping split (a diagnostic build): root counters 2.8%, BIOS and module
timers 3.4%, SPU2 DMA 0.5%. None of it does much per call: it ran after **every** kernel
call, because a kernel call (on the IOP often an `addiu zero, zero, N` stub) ended
`ExecuteCpu` and went back to the VM loop. Now kernel calls and interrupt checks are
handled inside `ExecuteCpu`, which goes on with the rest of its quota (it still returns
for idle, VU0 calls, TLB and the HLE idle request). Settings `iop_stay_in_slice`,
`ee_stay_in_slice` (both on). The root counters also skip the division for clock ratios
of 1, 8, 16 and 256 (the Vita's CPU has no divide instruction).

| RE4 intro, warm | Emulated fps | Pictures/s |
|---|---|---|
| Both off | 32.5 | 15.6 |
| IOP on (two runs) | 33.1, 33.3 | 15.9, 16.0 |
| IOP and EE on (two runs) | 33.8, 33.8 | 16.2, 16.2 |

Silent Hill 2 on a PC, 1500 frames: the IOP change gives the same final picture; with the
EE change the same title menu (its animated background a little further on).

### Kernel calls and IOP interrupts in the movie (2026-11-08, patch 0085)

New measurements (all only while `ee_hot_after` profiles, so normal runs pay nothing):
`syscalls.txt` now also has `time N calls microseconds` per EE kernel call (and for
Play's own paths: 128 reschedule, 129 exit interrupt, 131 dispatch unlock, 132-147 the
HLE calls 0xA00-0xA0F); `iopcalls.txt` counts IOP HLE calls by `module:function`, IOP
syscalls, interrupts by line, and how the SPU2 IRQ flag is set and cleared. Note: the
Vita's clock costs about 8 us per call pair, so per-call times of small calls are mostly
that; the counts are the reliable part. Autostart `copy_bench=1` times copies of a
movie picture (`copybench.txt`).

What they showed:
- **The picture handoff** (HLE 0xA04, `sceMpegGetPicture` RGB32): 2.7 ms per picture on
  the emulation thread, nearly all the copy of 1.15 MB into EE RAM. `copy_bench`:
  memcpy 310 MB/s, two threads 362 MB/s, NEON with prefetch 183, `sceDmacMemcpy` 151:
  the Vita's memory is the limit. Only not copying would help (the decoder writing
  into the game's buffer, which the game may still be reading).
- **EE kernel calls**: about 50 per emulated frame from the game (`WaitSema` and
  `SignalSema` 24000 each in 55 s); cheap each.
- **IOP interrupts: 97% are the SPU2 IRQ**, about 256 per emulated frame (all other
  lines 1-2). Play sets the flag about 3 times a frame (the sound input area) and RE4's
  driver **never clears it** (neither IRQINFO reads nor the IRQ enable bit), so the
  per-1000-ticks check raises the interrupt again and again. Two fixes were tried and
  dropped: raising it only when the flag goes from clear to set (the driver got one
  interrupt and then polled: `libsd` GetParam 3x, twice the audio underruns, slower)
  and once per IRQ event (the driver polled the SPU2: 31.6 against 33.8 emulated fps,
  twice the SPU2 read waits). The driver uses the repeated interrupt as its wake-up;
  the current behaviour is the fastest of the three.
- IOP HLE calls (55 s): `intrman` 17/18 (CpuSuspendIntr/CpuResumeIntr) 138000/106000,
  `libsd` 6 86000, thevent/thbase 15000-20000. The `libsd` calls reach Play's HLE
  dispatcher, which has no registered libsd module in release builds (CLibSd is only
  used for the debugger's names): worth checking what RE4's sound driver gets back.
- SPU2 register accesses (`iopcalls.txt` lines `spu2 ADDRESS reads N writes N`): RE4's
  driver reads each voice's envelope and the core attributes about once a frame, never
  reads IRQINFO and writes almost nothing (it mixes on the IOP and streams by DMA). So it
  never acknowledges the SPU2 IRQ at all; Play's repeated interrupt is what wakes it.

### Where the EE's compiled code goes (2026-11-08, `runs/re4/opcost.py`)

Each EE instruction of RE4's hot blocks compiled alone (with and without it) by the
Vita's code generator, weighted by how often its block runs: **memory accesses are
about 60% of the in-block ARM code**: `lw` 20 ARM instructions (8 on the fast path: base,
add, page lookup, test, offset extract, load; 6 for the handler call; 4 to store the
result and its sign-extended upper half), `lq`/`sq` 24, `ld`/`sd` 20, `lbu` 20, `sw` 19;
`addiu` 4. Per-access savings left are about one instruction (page entries pre-biased
so the offset extract goes away, about 1.5% overall). A large cut needs guest memory
mapped straight into the host address space (not available to Vita user code) or
code that keeps guest values in registers across blocks (tier 2).

### Tier 2, second round: fast RAM and cheaper exits (2026-11-08, patches 0086-0090)

Measured warm on the Vita, intro, four runs each (AOT_TRANSLATION.md has the details):

| Build | Pictures/s | Emulated fps |
|---|---|---|
| Before (kernel calls inside the slice) | 16.2 | 33.8 |
| Fast RAM path, 64-bit compares, likely branch exits | 17.65 | 36.8 |
| Memory exceptions in the quota, `subs`, cold 64/128-bit slow paths | **18.35** | **38.2** |

Per emulated frame (ms, sampling profiler, `ps2_*` columns): EE code 14.3 -> 11.1, IOP code
4.1 -> 3.0. Left: EE kernel calls 2.8 (half of it the picture copy into EE RAM), IOP
kernel calls 1.9, VIF direct 1.7, SPU2 register reads 1.4, IOP ticks 1.4, other 0.9, EE
ticks 0.6, EE I/O 0.5: about 26 ms per frame against 16.7 for 60.

Found on the way:
- **`steady_clock::now()` costs several microseconds on the Vita.** The per-call timers of
  `syscalls.txt` (patch 0085) mostly measure themselves: the EE "reschedule" at 7 us per call
  is the timer. Only 10700 of 269000 reschedule calls switch threads. A profiled run
  (`ee_hot_after`) runs at 12 pictures/s; use the sampling sections for shares.
- **The SPU2 IRQ re-assertion is wanted by RE4's driver**: `spu_irq_ticks` (IOP ticks
  between re-assertions, 1000 by default) at 4000 or 8000 gives 17.3-17.4 pictures/s
  against 18.35.
- **Movie pictures go to the GS without a copy** (`movie_zero_copy`, on by default, patch
  0090): RE4 decodes into a ring of three EE RAM buffers and sends a picture in 16-pixel
  columns a few vblanks later. `FeedImageData` used to copy each column for the GS thread;
  now, when a column lies in one of the last three pictures' places (until a newer picture
  is written there, at most 16 vblanks), the GS thread reads the decoder's own buffer. All
  688 KB of each picture go that way. **19.5 pictures/s (40.7 emulated fps)** against 18.3,
  twice each. Frames checked: the same pictures. A first version kept only the newest
  picture and caught 28 KB per picture: the game shows an older one of the ring.
- **SPU2 voice levels read without pausing the mixer** (`spu_fast_reads`, on by default,
  patch 0091): RE4's driver polls ENVX of 24 voices; these reads (ENVX, VOLXL, VOLXR) no
  longer wait for the mixer thread's block when no register write is queued. +0.7%.
- **The copy into EE RAM off the emulation thread** (`movie_async_copy`, on by default):
  with zero copy the GS no longer reads that copy, so a helper thread on the decoder's core
  does it. 19.5 -> 19.9 pictures/s (two runs each; less than its 2.7 ms suggests: both
  threads share the memory bus). Something reading the picture from EE RAM right after
  `sceMpegGetPicture` could see it half written; RE4 doesn't.

- **Movie pictures uploaded to GL from the decoder's buffers** (`picture_textures`, on by
  default): the GPU thread was at 85-93% and decoded the 512x512 movie texture from GS
  memory for each picture (11 ms). Picture-backed transfers are now recorded by the texture
  cache (`AddPictureSource`, dropped when anything else writes there), and when they cover
  the sampled rows exactly, each 16x336 column goes to `glTexSubImage2D` straight from the
  decoder's buffer; only the rows below are decoded. Decode 11.1 -> 0 ms, GL 1.9 -> 5.4 ms
  per picture, GPU thread 85-93% -> 67-70%. Frames checked. (A first version filled the
  texture cache's texels from the buffers instead: memory-bound, 9 ms, no gain.)

**Now: 21.1 pictures/s, 44.0 emulated fps** (from 16.2 / 33.8 this morning).

### The IOP on its own thread (2026-11-08, patch 0093, `iop_thread`, off by default)

The IOP and what goes with it (its code, kernel calls, ticks, SPU2 waits) is about 7.6 of
the emulation thread's 23 ms per frame, and cores 1 and 2 have room. `iop_thread=1` runs the
IOP on its own thread (`ParallelIop.h`): the EE thread grants it each slice's ticks
(credit), the IOP thread runs them a few slices behind; at vblanks and VM calls the EE thread
waits until the IOP has caught up; where one CPU's code touches the other's side (the SIF:
DMA, packets, HLE module calls, the IOP's I/O manager) the other CPU parks first
(`CCrossing`, about 160 a second in RE4's intro).

It runs correctly (boot, title, intro; Silent Hill 2 on the PC) but **is slower**: 17.0
pictures/s against 20.3 without it, the IOP thread on core 1 above the GPU thread (core 2:
15.0). The EE thread's own work per frame drops only by about 4 of the 7.6 ms moved off (two
cores share caches and memory), and it waits for the IOP about 6 ms per frame (crossings,
vblank drains). Found on the way, for anything threaded on the Vita: condition variables
lost wakeups (a waiter sharing one with a timed waiter never woke; one each with timed
waits still stalled for seconds), so the IOP thread sleeps on a kernel semaphore and the EE
thread spins; and a 50 us sleep lasts far longer, so polling with sleeps starved the IOP.
Left as a setting for later work: fewer crossings (the SIF packet queue), a cheaper
drain.

Tuning (2026-11-09). New settings: `iop_core`, `iop_priority` (default 100), `spu_core`,
`spu_priority`, `mpeg_copy_core`, `mpeg_copy_priority`, `iop_batch` (slices of credit
that wake the IOP thread, default 4), `iop_lag` (credit at which the EE thread waits,
default 16), `iop_inline_drain` (default on). The IOP thread's sections are sampled too
(`iopt_*` columns in perf.csv). Best layout: the IOP thread on core 2 at priority 100, the
SPU2 mixer on core 1 at 120, the movie copy thread on core 0, `iop_batch=8 iop_lag=32`:
**20.1 pictures/s against 20.5 serial** (same build, back to back; core 1: 15–17; batch 4:
18.9; batch 16: 19.6). With inline drain, a drain that finds the IOP thread asleep runs the
credit left on the EE thread instead of waking it (about 30 a second); no measurable gain.
Why it can't win yet: all three cores are full. Core 2 holds the decoder (62%), the IOP
thread (31%) and the copy thread (7%). Every millisecond the IOP thread takes, the decoder
loses: 62% decoder time against 71% serial, so fewer pictures. The EE thread still waits
about 6 ms per frame (IOPlag 4.4, drain 1.5). The IOP thread only pays off once the decoder or
the GPU thread frees a core's worth of time.

Silent Hill 2 checked on the same build (2026-11-08): its movie hangs with threaded VU1
(HLE_MPEG.md open bug 1, there since the threaded VU1 change of 2026-11-06), and runs at
2 fps without it (VU1 colour conversion on the emulation thread). Title screen 34 emulated
fps, 17 shown, threaded VU1 off.

### Is a locked 30 reachable? The CPU budget (2026-11-08)

The Vita gives an app three cores at 444 MHz (already set). At a real 30 pictures/s
(60 emulated fps) RE4's intro would need, from today's measurements:

| Work | Now (15 pictures/s) | At 30 pictures/s |
|---|---|---|
| Emulation thread (EE, IOP, VM), one core | 95% at 31.5 emulated fps (32 ms per frame) | about 190%: impossible on one core; needs 2x faster |
| MPEG decoder (31 ms of CPU per picture) | 45-50% of core 2 | about 95% |
| SPU2 mixer | 17% of core 2 | about 34% |
| GPU thread | 65% of core 1 | 65-100% |
| **Total** | **about 225%** | **about 390% of 300%** |

So the lock needs about 1.4x efficiency everywhere and 2x on the emulation thread,
where the EE is only half the cost (IOP work, mostly Capcom's software mixer, plus
the VM, is about 11.5 ms of the 16.6 ms budget by itself). Routes, from most to
least work:
1. Tier 2 (AOT_TRANSLATION.md) on the EE **and** the IOP: whole functions in host
   registers. The design's estimate is 1.5-2.5x on guest code. Several sessions.
2. Fewer host instructions in the VM loop (timers, DMA, interrupts per slice).
3. A faster decoder (parsing and colour conversion are 65% of its time) so that it
   and the SPU2 mixer share core 2 at 30 pictures/s.

### What a real 30 pictures/s needs

The emulation thread costs about 32 ms per emulated frame against 16.6. It is spread out:
the game engine running under the movie (room data, `IdCalcVertex`, `PS2OT`), CRI's
servers, Capcom's IOP mixer, kernel calls. The largest single item left is under 10%, and
cutting EE work alone has not raised the frame rate. Two big pieces would:
1. **Faster EE and IOP code for every game**: tier 2 recompilation (AOT_TRANSLATION.md)
   or JIT work on block linking and register allocation. Roughly 2x is needed.
2. **A faster native MPEG decoder**: done in part, 42 ms of wall time per picture (was
   55). Enough for about 26 pictures/s; 30 needs under about 28 ms of CPU.

## Ideas not done yet

1. Find why the IOP's time grows when the EE gets cheaper (above).
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
| `ee_hot_after=S` | Sample EE hotspots from this second on; also writes `syscalls.txt` (kernel calls by number, with time per call from S on) and `iopcalls.txt` (IOP HLE calls, interrupts by line, SPU2 IRQ flag set/clear counts) |
| `copy_bench=1` | At start, time copies of a 1.15 MB movie picture by four methods (`copybench.txt`) |
| `mpeg_core=N` | Core of the movie decoder (default 2) |
| `thread_lock_hle=1` | RE4 USA: critical sections without kernel calls (also a per-game setting) |

`perf.csv` gained `jit_pool_kb`, `jit_flushes`, `mpeg_pictures`, `tex_decode_ms`,
`tex_gl_ms`, `mpeg_wait_ms`, `mpeg_decode_ms`, `mpeg_starved`, `disc_ms`, `disc_reads`,
`vu_sync_ms`, `spu_wait_ms`, `mailbox_wait_ms`, `limiter_ms`. `tests/mpeg2_decoder_tests FILE --es` replays a stream dump on a PC.

## Traps when testing

- Keep the Vita and the PC awake (docs/HANDOFF.md): `vitalab cmd "nosleep on"`, and
  keep-awake in the Claude app.
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
