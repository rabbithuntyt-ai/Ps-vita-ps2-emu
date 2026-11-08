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
- Fuzz test for the EE JIT: `tools/make_ee_fuzz_elf.py` (+ ctest cases).
