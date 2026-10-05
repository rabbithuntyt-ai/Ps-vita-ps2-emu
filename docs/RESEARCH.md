# PS2 emulation on the PS Vita: research and design decisions

## 1. The hardware gap

| | PlayStation 2 | PlayStation Vita |
|---|---|---|
| Main CPU | MIPS R5900 "Emotion Engine" @ 294.9 MHz, 128-bit SIMD (MMI) | 4x ARM Cortex-A9 @ 333 MHz (444 MHz with homebrew), VFPv3 + NEON |
| Vector units | VU0 + VU1 (VU1 does nearly all T&L), ~6.2 GFLOPS | NEON only (64-bit datapath on A9) |
| I/O CPU | MIPS R3000A @ 36.8 MHz (IOP) | — |
| GPU | Graphics Synthesizer: 4 MB eDRAM, 48 GB/s, fill-rate monster | PowerVR SGX543MP4+, 128 MB VRAM |
| RAM | 32 MB + 2 MB IOP + 2 MB SPU | 512 MB (≈365 MB usable by apps in extended mode) |

The Vita is one generation ahead on the GPU side but its CPU is weaker per clock
than what PS2 emulation needs. Desktop PCSX2 needs a modern x86 core at
3+ GHz; AetherSX2/NetherSX2 on Android need Snapdragon 845-class
(Cortex-A75 @ 2.8 GHz) phones for full speed. A Vita core is roughly
an order of magnitude slower than that.

**Conclusion: full-speed PS2 emulation of commercial 3D games on the Vita is
not achievable today.** The community consensus on GBAtemp and r/vitahacks has
said the same for years. What *is* achievable is a working, correct emulator
that runs 2D/light games and homebrew at partial-to-full speed and that can be
optimized incrementally — which is what this project sets out to build.

## 2. Options considered

### A. Write a new emulator from scratch
Rejected. A PS2 emulator is a multi-year, multi-person effort (EE, VU0/VU1,
IOP, SPU2, GS, DMAC/GIF/VIF, CDVD, SIO2, BIOS). Anything written in one
project would be far less compatible than existing work.

### B. Port PCSX2 / AetherSX2
Rejected. PCSX2's recompilers are x86-64 only; the ARM64 work (AetherSX2,
later merged in part) targets AArch64, not the Vita's 32-bit ARMv7. PCSX2 also
requires a BIOS dump, 64-bit address space tricks (fastmem) and far more RAM.

### C. Static recompilation (PS2Recomp)
Interesting but not a general emulator. PS2Recomp (2025–2026) translates one
game's ELF to C++ ahead of time. It removes CPU-emulation overhead, but each
game needs per-title work, the project is still early and states it does not
yet work end to end, and GS/VU1 emulation is still required. It is a good
*future* path for specific showcase titles, and the GS renderer built here is
reusable for it.

### D. Port Play! (chosen)
[Play!](https://github.com/jpd002/Play-) by Jean-Philip Desjardins is a
BSD-2-Clause PS2 emulator written in portable C++17 that already runs on
Android, iOS, macOS, Linux, Windows and the web. Decisive properties:

1. **It has a 32-bit ARM (AArch32) JIT backend** in its code generator
   (`Jitter_CodeGen_AArch32*`), originally written for 32-bit iOS/Android.
   That is exactly the Vita's ISA. It does not require hardware integer
   division (the Cortex-A9 lacks it) and only passes integer/pointer
   arguments to helpers, so it is independent of the soft/hard float ABI.
2. **High-level emulated BIOS.** Play! emulates the PS2 kernel and IOP
   modules, so no copyrighted BIOS dump is required.
3. **Clean frontend/backend split**: GS, pad and sound handlers are
   interfaces; frontends implement them (Qt, Android, iOS, libretro).
4. Permissive license compatible with distributing a VPK.

## 3. What a Vita port needs, and how it is solved here

| Problem | Solution in this repository |
|---|---|
| JIT memory: the Vita has no `mmap(PROT_EXEC)`; executable memory comes from `sceKernelAllocMemBlockForVM` in ≥1 MB blocks and requires an *unsafe* app | One pool reserved at startup and carved by `CJitPool` (first-fit, coalescing, unit tested). Writes go through `sceKernelOpenVMDomain/CloseVMDomain`, followed by `sceKernelSyncVMDomain` for I/D cache coherency. CodeGen patch: `patches/0002`. |
| GS renderer: Play!'s renderers need OpenGL 3.2/GLES 3 or Vulkan; the Vita has GXM (and vitaGL, a GLES-2-class wrapper with no integer shader ops) | New **software GS** (`src/gs`) rendering directly into emulated GS memory: all primitive types, Gouraud/flat, UV/STQ texturing, all PSMs (32/24/16/8/4/8H/4HL/4HH, Z formats), CLUT + TEXA, bilinear, wrap modes, TFX, fog, alpha/destination-alpha/depth tests, blending, PABE, FBA, FBMSK, COLCLAMP, local→local transfers. Portable, so it is tested on the host. |
| Paths / filesystem: Framework has no Vita branch | Patch `0001` adds a Vita branch to `PathUtils`; libstdc++'s `std::filesystem` is used (ghc::filesystem does not support the Vita). |
| Aligned allocation: `framework_aligned_alloc` falls back to plain `malloc` on unknown platforms (unsafe for 16-byte vector data) | Patch `0001` uses `memalign` on the Vita. |
| Raw disc device access (`/dev/...`, `statvfs`) | Not applicable on the Vita; disabled by patch `0003`. |
| Memory budget | ~180 MB resident on x86-64 for a simple program; less on 32-bit ARM. App built with `ATTRIBUTE2=12` (extended memory), 288 MB heap + 40 MB JIT pool. |
| Input | `CPH_Vita`: buttons/sticks; rear touch halves = L2/R2, front bottom corners = L3/R3; real L2/R2/L3/R3 on PS TV. |
| Audio | `CSH_Vita`: ring buffer + BGM port thread at 44.1 kHz. |

## 4. Verification done so far

* `gs_software_tests`: 37 checks covering fill rules, scissor, Gouraud,
  depth, blending, 16-bit framebuffers, FBMSK, CLUT textures, transfers and
  display readout.
* `jit_pool_tests`: allocator correctness under random churn.
* `elf_boot_test`: `tools/make_test_elf.py` generates a PS2 executable (no
  PS2SDK needed) that calls `SetGsCrt`, programs the CRTC and sends a GIF
  packet over DMA every vblank. It boots through Play!'s HLE BIOS and EE
  recompiler, renders through the software GS and the output frame is checked
  pixel-wise.
* CI builds the actual VPK with the official VitaSDK Docker image.

Not verified: running on real Vita hardware (no device available to the
author of this commit). Expect the first hardware runs to need fixes.

## 5. Performance roadmap

The software GS is written for correctness first. Ordered by expected payoff:

1. **Profile on hardware** (EE JIT vs. VU1 vs. GS split).
2. **GS on its own core with a tiled, multi-threaded rasterizer** — the Vita
   has 3 usable cores; Play! already runs the GS on a separate thread.
3. **Specialized span functions** (templated on PSM/test/blend state, the way
   GSdx's software renderer does) and NEON for texturing/blending.
4. **VU1 → ARM JIT tuning**: Play! already recompiles VU microcode; check the
   AArch32 `Md` (128-bit) paths use NEON well on Cortex-A9.
5. **GXM hardware renderer** for games whose effects don't need GS-memory
   accuracy, with the software path as a fallback.
6. **Per-game speed hacks** (EE cycle rate, VU skipping, frame skip) — Play!
   exposes EE frequency scaling.
7. **Static recompilation** for selected titles (PS2Recomp-style), reusing
   this GS.
