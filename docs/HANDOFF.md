# Handoff: continuing VitaPS2 work from a PC with Vita Lab

This branch (`claude/vita-ps2-emulator`) is Play! (PS2 emulator) plus a Vita
frontend. Work so far was done in a cloud session without access to the Vita;
this note is for a session running on the owner's PC, which can reach the Vita
through **Vita Lab** (`vitalab` CLI + plugins; see the Vita-Lingo repo, `lab/`).

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

- `vitalab shot s.png` (look at it), `vitalab press cross`, `vitalab stats`
- Faster iteration: `vitalab push-eboot VTPS20000 eboot.bin`
- `tools/vitalab_run.sh GAME SECONDS OUTDIR [setting=value...]`: writes
  `ux0:data/VitaPS2/autostart.txt` (boots the game directly, quits after
  SECONDS), then collects `perf.csv` (one line per second: fps, EE idle/rate,
  PS2 thread time per subsystem, SPU waits, IPU activity, underruns, CPU per
  thread), `log.txt`, screenshots and crash dumps.
- In game: Select+Start = pause menu (settings), Select+L = overlay.

## Open problems (Silent Hill 2, SLUS-20228)

1. **Intro movie plays at ~4 frames/s** while the VM runs at ~59 fps.
   Audio is ruled out (no change with the SPU thread on/off). The overlay
   line `IPU/s: idec/bdec/vdec/fdec/csc, out KB, stalls` (only while a movie
   decodes) shows whether the IPU starves, waits on DMA3 (stalls) or whether
   the game issues few commands. `CSubSystem::ExecuteIpu` (Ee_SubSystem.cpp)
   decodes until the OUT FIFO has data, then flushes once per EE slice.
2. **Brightness flashing** on the title screen: not the renderer (software
   too), not VU1 threading, not the JIT (Safe JIT). Less at EE rate 130%:
   timing related. Auto EE rate changes speed mid-game; test fixed rates.
3. Timing-dependent IPU/DMA hang seen once at auto rate 75% (IPU idle with a
   full input FIFO, DMA4 still running).

## Recent work (newest first)

- Scripted runs (autostart/perf.csv), IPU and SPU wait counters on overlay.
- SPU thread: register reads and DMA no longer wait for the mixer backlog.
- Disc images read with sceIo (64-bit offsets, 128KB read-ahead): fixed the
  hang after the footprints loading screen. Memory card path fix.
- JIT (AArch32): fused guest memory ops, interval register allocator, r9,
  branchless branches, fast block epilogue, EE sign-extension tracking,
  VU sticky-flag merging, NEON clamp/blend/flag accumulation.
- Fuzz test for the EE JIT: `tools/make_ee_fuzz_elf.py` (+ ctest cases).
