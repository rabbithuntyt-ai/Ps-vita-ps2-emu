# VitaPS2

An experimental PlayStation 2 emulator for the PlayStation Vita / PS TV.

VitaPS2 runs the [Play!](https://github.com/jpd002/Play-) emulation core (EE and
IOP recompilers, VU, SPU2, high-level emulated BIOS, so **no BIOS dump is
needed**) on the Vita through its 32-bit ARM JIT, with a new portable
**software Graphics Synthesizer** renderer and a native Vita frontend.

> **Set expectations:** the Vita's Cortex-A9 is roughly 10x slower than what
> full-speed PS2 emulation needs on ARM. Commercial 3D games will run far
> below full speed. See [docs/RESEARCH.md](docs/RESEARCH.md) for the analysis and [docs/ROADMAP.md](docs/ROADMAP.md) for the full checklist,
> the options considered and the optimization roadmap.

## Status

| Component | State |
|---|---|
| EE / IOP / VU / SPU2 / HLE BIOS | Play! core, unchanged except 3 small patches |
| JIT on Vita (`sceKernelAllocMemBlockForVM` pool) | Implemented, needs hardware testing |
| Software GS renderer | Implemented and unit tested (see below) |
| Vita frontend (game list, display, controls, audio) | Implemented, needs hardware testing |
| Host build + automated tests | Passing |
| VPK build | CI with the official VitaSDK Docker image |

## Installing on a Vita

1. Requires HENkaku/Ensō with **Unsafe Homebrew enabled** (HENkaku Settings):
   the recompiler needs executable memory.
2. Install `VitaPS2.vpk` (from the CI artifacts) with VitaShell.
3. Copy games (`.iso`, `.cso`, `.chd`, `.isz`, `.cue`, `.mds`, `.bin`) or
   homebrew (`.elf`) to `ux0:data/VitaPS2/games/`.

### Controls

| Vita | PS2 |
|---|---|
| D-pad, face buttons, sticks, START, SELECT | same |
| L / R | L1 / R1 |
| Rear touch left / right half | L2 / R2 |
| Front touch bottom-left / bottom-right corner | L3 / R3 |
| SELECT + START | pause menu: speed hacks, aspect, overlay, quit |
| SELECT + L | toggle performance overlay |

Settings chosen in the pause menu are saved per game in
`ux0:data/VitaPS2/settings/`.

On PS TV, a DualShock 3/4's L2/R2/L3/R3 work directly.

## Performance work

The renderer is built for the Vita's Cortex-A9 (see `gs_benchmark`):

* **Span-based rasterizer**: exact integer scanline extents, fixed-point
  stepping, span loops specialized per state (texture/filter/depth/format/blend).
* **Decoded texture cache**: textures decoded to RGBA once (lazily per 8x8
  tile), invalidated by GS memory page stamps, CLUT hash and TEXA.
* **Multi-threaded GS**: primitives are batched and rasterized by several
  threads that own interleaved scanlines — output is bit-identical to single
  threaded rendering (verified by `gs_parallel_tests`, ThreadSanitizer clean).
* **Speed hacks** (pause menu, per game): EE cycle rate (underclock), interlaced
  half-line rendering, frame skip.

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

### Host (Linux/macOS) — development and tests

The host build uses the exact same core and software GS, headless:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/vitaps2_host game.iso --frames 300 --out frame.ppm
```

Tests:

* `gs_software_tests` — rasterizer and GS pipeline unit tests.
* `jit_pool_tests` — executable memory pool allocator.
* `gs_diff_tests` — optimized rasterizer vs. the simple reference
  implementation on random states and primitives.
* `gs_parallel_tests` — multi-threaded rendering is bit-identical to single
  threaded rendering.
* `gs_benchmark [seconds] [threads]` — renderer throughput.
* `elf_boot_test` — generates a PS2 program (`tools/make_test_elf.py`), boots
  it through the full emulator and checks the rendered frame.

## Layout

```
external/Play     Play! emulator (git submodule, pinned)
patches/          Minimal Vita patches for Play!/Framework/CodeGen, applied at configure time
src/gs/           Portable software GS renderer (CGSH_Software, CSoftwareRasterizer)
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
