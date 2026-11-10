# Handoff: continuing VitaPS2 work from a PC with Vita Lab

This branch (`claude/vita-ps2-emulator`) is Play! (PS2 emulator) plus a Vita
frontend. Work so far was done in a cloud session without access to the Vita;
this note is for a session running on the owner's PC, which can reach the Vita
through **Vita Lab** (`vitalab` CLI + plugins; see the Vita-Lingo repo, `lab/`).

This is an emulator for every PS2 game. Whatever is learned from one game goes in
[LIBRARY_WIDE.md](LIBRARY_WIDE.md), with how it could help the others.

## Build

- Play! is a submodule (`external/Play`); our changes are `patches/NNNN-*.patch`,
  applied at configure time (`cmake/ApplyPatches.cmake`). When changing Play!
  or its CodeGen/Framework submodules, regenerate the matching patch.
- Vita build: vitasdk (the CI uses the `vitasdk/vitasdk` docker image; see
  `.github/workflows/build.yml`). Output: `VitaPS2.vpk` / `eboot.bin`,
  title ID `VTPS20000`.
- Host build + tests (Linux): `cmake -B build && cmake --build build && ctest`.
  ARM tests under qemu: `build-arm` (see CMakeLists for the toolchain).

## Testing a game in one go

`tools/vita_game_test.sh "Game.iso" 150 [setting=value...]` (Git Bash on the PC, Vita
Lab connected): keeps the Vita awake, optionally installs a build (`EBOOT=path`), does a
warm-up run (fills the JIT cache), then a measured run with scripted buttons (`PAD`,
default START/CROSS every 4 s), frame dumps, EE/IOP hotspots and RAM dumps. It then writes
`report.md` with `tools/game_report.py`: verdict against the goals (movies locked at
30, menus at 60 emulated fps), load phases, costs per phase, hotspots (named from the
game's symbol table when it ships one in RAM) and `sheet.png` with the frames. Use it
on each new game before anything else. Keep Vita Lab free during runs: a large
`vitalab get` (a disc image) makes its file server refuse every other transfer.

## Driving the Vita (Vita Lab)

- **Keep both machines awake, always (owner's rule, for every session working through
  Vita Lab).** The Vita: `vitalab cmd "nosleep on"` at the start of a session and before
  each run (`runs/re4/run.sh` does it), check with `vitalab cmd "nosleep status"`. It
  holds in and out of the emulator; VitaPS2 also calls `sceKernelPowerTick` every frame.
  The Vita's own Settings > Power Save > automatic standby should stay off. The PC: in
  the Claude app, ask for keep-awake (`request_keep_awake`, until the session is idle).
- `vitalab shot s.png` (look at it), `vitalab press cross`, `vitalab stats`
- Faster iteration: `vitalab push-eboot VTPS20000 eboot.bin`
- `tools/vitalab_run.sh GAME SECONDS OUTDIR [setting=value...]`: writes
  `ux0:data/VitaPS2/autostart.txt` (boots the game directly, quits after
  SECONDS), then collects `perf.csv` (one line per second: fps, EE idle/rate,
  PS2 thread time per subsystem, SPU waits, IPU activity, underruns, CPU per
  thread), `log.txt`, screenshots and crash dumps.
- In game: Select+Start = pause menu (settings), Select+L = overlay.
- Building on a Windows PC: WSL (Ubuntu 24.04, WSL1 works) with VitaSDK in
  /usr/local/vitasdk, then `tools/build_vita_wsl.sh` (sources on the Windows
  side, build directory in Linux). Check out with `core.autocrlf=false`: CRLF
  breaks the patches.
- `tools/vitalab_longrun.sh OUTDIR SECONDS [settings]`: like vitalab_run.sh
  with a screenshot every 15s. Extra autostart key `gs_diag=N` logs N flips
  (displayed buffer, draws per target, targets) to `ux0:data/VitaPS2/gsdiag.txt`.
- Real-world numbers: after a run that plays to the opening movie, record them
  with `tools/vita_realworld.sh RUN_DIR LABEL` (docs/perf/vita_realworld.*,
  README table). log.txt stays empty on the Vita (the stdout redirect does
  not work); uncaught exceptions write `ux0:data/VitaPS2/crash.txt`.

**Start with [RE4.md](RE4.md):** the current focus (30 fps in Resident Evil 4), with
findings, tools and traps that apply to every game.

## Open problems (Silent Hill 2, SLUS-20228)

1. **Intro movie is a slideshow** (measured on hardware: ~0.8 pictures/s,
   emulated fps ~10 during the movie). Found and fixed on the way:
   - The wait-loop idle check (0020) counted status reads per PC and never
     reset: once at 5000, every IPU_CMD/IPU_CTRL/Dn_CHCR read ended the EE
     slice, even when the unit was done (players read them once per IPU
     command, ~10 per macroblock). 0048: only reads that find the unit busy
     count. The ipu_decode_test probe never reaches 5000 reads from one PC.
   - Polls that found the IPU busy ended the slice before the IPU could
     advance (it only ran at slice boundaries): 0050 runs ExecuteIpu on reads
     of the IPU registers and DMA3/DMA4/D_STAT. IPU idle triggers: up to
     45000/s -> 0.
   - Flips were skipped when no GS register was written that vblank (movie
     players swap DISPFB): 0047.
   Now: the emulation thread is busy (EE ~33%, VIF ~27%, SPU ~26%) at ~10
   emulated fps since the EE clock fix (0037): raw EE speed is the limit.
   CHCR polls with STR set still idle ~9000 slices/s (perf.csv
   `idle_intc_gs_t1_ipu_dstat_chcr_vifgif`). User report: a white bar at the
   bottom of the movie every other frame (not caught in screenshots yet).
2. **Brightness flashing** on the Konami and title screens: fixed. The auto EE
   rate stepped down during loading (JIT compile time counts as EE time) and
   never climbed back; the triple-buffered game then showed unfinished
   buffers. No flicker at a fixed 100% (user confirmed). AutoCycleRate now
   ignores seconds spent compiling or waiting on the GS/SPU threads
   (stallShare) and climbs back on them.
3. **Black screen when the movie hands over to the game** (user report: let
   the movie play out; the screen stayed black afterwards). Unknown whether
   the game hangs or only the display is wrong: check whether vm_fps and EE
   activity continue and take a GS dump/screenshot at that point.
4. Timing-dependent IPU/DMA hang seen once at auto rate 75% (IPU idle with a
   full input FIFO, DMA4 still running).

## Where RE4 work stopped (2026-11-09) and what's next

Goal: RE4's intro movie locked at 30 pictures/s (60 emulated fps). Now about 21 (tier 2
off) to 21.5 (tier 2 on) pictures/s, 43-45 emulated fps. The emulation thread spends about
22.5 ms per emulated frame (EE code 11, IOP code 3.6, IOP kernel 2.3, the rest about 4.5)
against 16.7: about 6 ms to remove. Details: docs/RE4.md, docs/AOT_TRANSLATION.md.

**Measure from the save state, never from boot.** Runs from boot press buttons at fixed
real-time seconds, so any speed change moves the timeline (a run then "shows 0 pictures"
because the movie hadn't started). `runs/re4/bench.sh NAME [settings]` loads the title-menu
state (`load_state=1`), presses start/cross from 8 to 44 s, the movie plays from about 40 s;
`python3 brk4.py runs...` (in WSL) measures 55-140 s. Two runs agree within 0.3%. A new eboot
makes the JIT cache cold: run one warm-up (`bench.sh NAMEw`) first. The state file is on the
Vita (made by `runs/re4/mkstate.sh`: `save_state_at=44` with presses up to 31 s).

**The Vita is shared** with the RE4 gameplay session (worktree C:\Users\troy2\vps2-gp, its own
app VTPS20001 and data folder ux0:data/VitaPS2gp). Only one app can run: `runs/re4/run.sh`
takes the lock (`source /c/Users/troy2/vita_lock.sh; vita_lock movie` ... `vita_unlock`, stale
after 30 min). Scripts that `vitalab quit all` or push an eboot outside run.sh (the `ab*.sh`
batches, `tools/vita_jit_fuzz.sh`) must take it too, and never quit the Vita's app while the
lock's owner is "gameplay".

**Unfinished, in this order:**

1. **Test the disc read-ahead fix** (in the working tree, not tested on the Vita). Tier 2 runs
   hung while RE4 loads, about 1 in 3 (EE threads all blocked, later the emulation thread
   stopped: vm_fps 0). The region checking mode (`tier2_verify=1`: every region run done by
   the interpreter, undone, then natively, compared) found no wrong results in about 14000
   runs, and the hang showed with exits through the executor (`tier2_exits=0`) as well as
   linked ones, so it isn't tier 2's code. Suspect: `CVitaImageStream`'s read-ahead
   (DiskUtils.cpp, patch 0092) waited on condition variables, which lose wakeups on the
   Vita; a lost one there blocks the emulation thread's disc read for good, and tier 2's
   timing makes it likelier. Both waits are now polled (`sceKernelDelayThread`).
   To do: build is `runs/build/eboot_t2y.bin`; run `runs/re4/ab127.sh` (warm-up, six tier 2
   runs, two without; `fx.txt` gives the picture-line counts per run: about 38-40 is a full
   movie, under 10 a hang). If the six are clean, consider `tier2=1` by default. (Patches
   0092 and 0094 already carry the fix and the checking mode.) **Trap:** `runs/re4/mk86.sh`
   now also regenerates 0089 against the working tree, which includes tier 2's BasicBlock
   changes (they belong to 0094): keep the committed 0089 (`git checkout` it after running
   mk86) until mk86 diffs 0089 against a snapshot instead. If not, the checking mode's next
   step is to cover region runs that touch I/O (they are skipped today: about 20%).
2. **Tier 2 on the EE, faster code** (the biggest gain left, about 3-4 ms): it saves 0.8 ms
   of 11 so far. Blocks still end at `syscall`, `div`/`divu`, `ldl`/`ldr`, MMI `madd`, COP0;
   loads/stores test the RAM range each time; only 7 guest registers stay in ARM registers;
   regions average 6 KB of code. Direct exit links and lazy entry loads are done (no
   measurable gain yet).
3. **Tier 2 for the IOP** (about 1-1.5 ms): same front end, 32-bit only.
4. **A cheaper SPU2 interrupt path** (about 1 ms): RE4's driver never clears the SPU2 IRQ
   flag, so line 9 fires about 5600 times a second (IOP kernel time); delivering it less
   often slowed the driver (`spu_irq_edge`, off), so the BIOS dispatch path (stub at 0x200,
   10% of IOP time) has to get cheaper instead.

Smaller items:
- `Vpu.cpp`: `ExecuteMicroProgram` calls `steady_clock::now()` at every VU program start
  (several us each on the Vita); the gameplay branch only does it with diagnostics on.
- An RE4 ISO copy on the PC (made by the gameplay session; use it read-only):
  `C:\Users\troy2\re4iso\re4.iso`. The PC host runner can now run RE4 (`--tier2` interprets
  regions there): loading-hang and tier 2 questions can be checked on the PC first.

Tier 2 settings (main_vita.cpp): `tier2` (1 native, 2 interpreted), `tier2_eager`,
`tier2_hot` (samples, 24), `tier2_max` (800), `tier2_blocks`, `tier2_depth`, `tier2_regs`,
`tier2_exits` (0 executor, 1 dispatch, 2 linked), `tier2_verify`, `tier2_lo`/`tier2_hi` (native
only in that window), `tier2_dump`. Reports: `tier2.txt` (regions, what ends blocks, MMI
breakdown, checks), `tier2verify.txt` (mismatches). PC tools: `jit_arm_dump RAM ADDR --tier2`
(IR; ARM code in `TIER2_DUMP`), host runner `--tier2` (interpreted, every block start),
`tools/make_ee_fuzz_elf.py` options `--calls --spr --more --fpu`, Vita fuzz
`tools/vita_jit_fuzz.sh N tier2=1 tier2_eager=1`.

## Recent work (newest first)

- Hardware renderer (title screen 10-15 -> 26-30 fps on hardware, before the
  EE clock fix): render target readbacks limited to the rows a memory access
  needs, rows a whole-page transfer overwrites are dropped from the dirty
  rectangle instead of read back (the RGBA8 copy is lossy: reading it back
  over newer GS memory corrupts it), per-format span texture decode, texture
  content hash (unchanged re-uploads skip the decode), row-wise display of GS
  memory. perf.csv has GPU stage timings (gpu_*_ms, up/dl).
- FastNew.cpp: operator new from a size-class arena (newlib malloc is 1.3us per
  call on the Vita; new+delete now 0.48us). Headerless on purpose: something
  frees malloc memory with delete.
- The app keeps the Vita awake (sceKernelPowerTick) in menus and in game.
- JIT compile timing in perf.csv (jit_*): ~3-4 ms per block on hardware for
  ~30 statements (optimize ~1.5, regalloc ~1.5, codegen ~0.7 ms).

- SPU voice mixing in two passes with a NEON (SSE2 on x86) saturating mix,
  exact /0x7FFF via shifts, reader/ADSR inlined: same output (checksum), ~21%
  fewer instructions in spu_benchmark (patch 0036).
- IPU end-to-end decode test (tools/make_ipu_elf.py).
- Scripted runs (autostart/perf.csv), IPU and SPU wait counters on overlay.
- SPU thread: register reads and DMA no longer wait for the mixer backlog.
- Disc images read with sceIo (64-bit offsets, 128KB read-ahead): fixed the
  hang after the footprints loading screen. Memory card path fix.
- JIT (AArch32): fused guest memory ops, interval register allocator, r9,
  branchless branches, fast block epilogue, EE sign-extension tracking,
  VU sticky-flag merging, NEON clamp/blend/flag accumulation.
- Fuzz test for the EE JIT: `tools/make_ee_fuzz_elf.py` (+ ctest cases); on the Vita's
  ARM JIT: `tools/vita_jit_fuzz.sh`. The fuzzer generates EE code only: after any change
  to block exits or linking, also boot an IOP-heavy game on the Vita (RE4 to its movie).
  Indirect dispatch (on by default on the Vita, not on x86) skips the executor between
  blocks, so bugs that the PC build hides show there (e.g. `addiu zero, zero, N`, a
  syscall in Play's IOP stubs).
