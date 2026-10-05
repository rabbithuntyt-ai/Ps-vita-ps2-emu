# VitaPS2

An experimental PlayStation 2 emulator for the PlayStation Vita / PS TV.

VitaPS2 runs the [Play!](https://github.com/jpd002/Play-) emulation core (EE and
IOP recompilers, VU, SPU2, high-level emulated BIOS, so **no BIOS dump is
needed**) on the Vita through its 32-bit ARM JIT, with new **GPU** and **software**
Graphics Synthesizer renderers and a native vitaGL frontend.

> **Set expectations:** the Vita's Cortex-A9 is roughly 10x slower than what
> full-speed PS2 emulation needs on ARM. Commercial 3D games will run far
> below full speed. See [docs/RESEARCH.md](docs/RESEARCH.md) for the analysis and [docs/ROADMAP.md](docs/ROADMAP.md) for the full checklist,
> the options considered and the optimization roadmap.

## Status

| Component | State |
|---|---|
| EE / IOP / VU / SPU2 / HLE BIOS | Play! core, unchanged except 3 small patches |
| JIT on Vita (`sceKernelAllocMemBlockForVM` pool) | Implemented, needs hardware testing |
| GPU GS renderer (vitaGL, default) | Implemented, tested against the software renderer on the host; needs hardware testing |
| Software GS renderer (accuracy fallback) | Implemented and unit tested (see below) |
| Vita frontend (game list, display, controls, audio) | Implemented, needs hardware testing |
| Host build + automated tests | Passing |
| VPK build | CI with the official VitaSDK Docker image |

## Installing on a Vita

1. Requires HENkaku/Ensō with **Unsafe Homebrew enabled** (HENkaku Settings):
   the recompiler needs executable memory.
2. Requires the runtime shader compiler `ur0:data/libshacccg.suprx` (used by
   vitaGL; many Vita ports need it). Extract it once with
   [ShaRKBR33D](https://github.com/Rinnegatamante/ShaRKBR33D). VitaPS2 shows an
   explanation screen if it is missing.
3. Install `VitaPS2.vpk` (from the CI artifacts) with VitaShell.
4. Copy games (`.iso`, `.cso`, `.chd`, `.isz`, `.cue`, `.mds`, `.bin`) or
   homebrew (`.elf`) to `ux0:data/VitaPS2/games/`.

### Controls

| Vita | PS2 |
|---|---|
| D-pad, face buttons, sticks, START, SELECT | same |
| L / R | L1 / R1 |
| Rear touch left / right half | L2 / R2 |
| Front touch bottom-left / bottom-right corner | L3 / R3 |
| SELECT + START | pause menu: renderer, speed hacks, aspect, overlay, quit |
| SELECT + L | toggle performance overlay |

Settings chosen in the pause menu are saved per game in
`ux0:data/VitaPS2/settings/`.

On PS TV, a DualShock 3/4's L2/R2/L3/R3 work directly.

## Performance work

### GPU renderer (default)

Measured on hardware, the A9 can rasterize only 1-5 Mpix/s of textured,
blended PS2 primitives in software, far below what 3D games draw. The GPU
renderer (`src/gs/hw/`) translates GS primitives into fixed-function OpenGL
draws (vitaGL on the Vita):

* GS framebuffers live in GPU render targets; GS memory is synchronized only
  when the CPU side needs it (transfers, CLUT loads, textures of a different
  layout), so typical frames never leave the GPU.
* Textures are decoded with the software renderer's texture cache and cached
  as GL textures; render-to-texture effects sample render targets directly.
* GS blending `(A-B)*C+D`, alpha/depth tests, color masks and texture
  functions map onto GL blend equations and the texture combiner (0x80 = 1.0
  handled through combiner scales). Perspective (STQ) uses clip-space
  `w = 1/Q`.
* The GS runs on the main thread (which owns the GL context), freeing a CPU
  core compared to the software renderer.
* Host tests render scenes with both renderers through Mesa and compare them.

### Software renderer (Pause menu -> Renderer)

Built for the Vita's Cortex-A9 (see `gs_benchmark`):

* **Span-based rasterizer**: exact integer scanline extents, fixed-point
  stepping, span loops specialized per state (texture/filter/depth/format/blend).
* **Decoded texture cache**: textures decoded to RGBA once (lazily per 8x8
  tile), invalidated by GS memory page stamps, CLUT hash and TEXA.
* **Multi-threaded GS**: primitives are batched and rasterized by several
  threads that own interleaved scanlines — output is bit-identical to single
  threaded rendering (verified by `gs_parallel_tests`, ThreadSanitizer clean).
* **Speed hacks** (pause menu, per game): EE cycle rate (underclock, or
  *Auto*: stepped down to 60% while the EE is the bottleneck and back up when
  there is headroom), interlaced half-line rendering, frame skip, VU1 on its
  own core.
* **Per-game profiles**: `assets/game_profiles.ini` ships default settings per
  disc serial (e.g. `[SLUS-20228]`); the player's own choices in the pause
  menu override them. The serial is shown in the overlay and the log.

| Workload (host x86, 1 thread → 3 threads) | Mpix/s |
|---|---|
| Clear / flat fill | 64 → ~930 |
| Textured sprite, 8-bit CLUT | 31 → 216 |
| Bilinear + alpha blend | 15 → 64 |
| Gouraud + Z | 21 → 152 |
| Perspective textured + blend + Z (3D) | 11 → 63 |

(left column: first implementation; right: current with 3 threads)

## Building

```sh
git clone --recursive <this repo>
```

### PS Vita

With [VitaSDK](https://vitasdk.org) installed (`$VITASDK` set):

```sh
cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita -j
# -> build-vita/VitaPS2.vpk
```

or with Docker: `docker run --rm -v $PWD:/src -w /src vitasdk/vitasdk:latest sh -c 'cmake -S . -B build-vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake && cmake --build build-vita -j'`

### ARM Linux under qemu — the Vita's recompiler on a PC

`cmake/toolchains/armhf-linux.cmake` cross-builds everything for a Cortex-A9
and runs the tests through `qemu-arm`, including Play!'s AArch32 recompiler
(the code path used on the Vita). See the file header for the packages.

### Host (Linux/macOS) — development and tests

The host build uses the exact same core and software GS, headless:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/vitaps2_host game.iso --frames 300 --out frame.ppm
./build/vitaps2_host game.iso --frames 300 --out frame.ppm --hw   # GPU renderer via Mesa
```

Tests:

* `gs_software_tests` — rasterizer and GS pipeline unit tests.
* `jit_pool_tests` — executable memory pool allocator.
* `gs_diff_tests` — optimized rasterizer vs. the simple reference
  implementation on random states and primitives.
* `gs_parallel_tests` — multi-threaded rendering is bit-identical to single
  threaded rendering.
* `gs_benchmark [seconds] [threads]` — renderer throughput.
* `gs_hardware_tests` — GPU renderer vs. software renderer on the same
  scenes (Mesa OSMesa, needs `libosmesa6-dev`).
* `ui_render_test` — menu/overlay drawing layer.
* `codegen_tests`, `VuTest` — Play!'s recompiler and VU test suites.
* `idct_tests`, `ipu_tests`, `audio_stretch_tests` — movie decoding and audio
  time stretching.
* `elf_boot_test`, `elf_boot_test_hw` — generates a PS2 program (`tools/make_test_elf.py`), boots
  it through the full emulator and checks the rendered frame.

## Layout

```
external/Play     Play! emulator (git submodule, pinned)
patches/          Minimal Vita patches for Play!/Framework/CodeGen, applied at configure time
src/gs/           Portable software GS renderer (CGSH_Software, CSoftwareRasterizer)
src/gs/hw/        GPU GS renderer (CGSH_Hardware, OpenGL fixed-function / vitaGL)
src/ui/           2D drawing layer (menus, overlays) and embedded font
src/common/       Emulator session, frame mailbox, JIT pool allocator
src/vita/         Vita frontend: app/UI, JIT memory, controls, audio
src/host/         Headless host runner
tests/            Unit and end-to-end tests
tools/            Test ELF generator, LiveArea asset generator, image helpers
sce_sys/          LiveArea assets
```

## License

VitaPS2's own code is released under the BSD 2-Clause license, like Play!,
whose license applies to everything under `external/Play`.
