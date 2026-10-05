# VitaPS2

An experimental PlayStation 2 emulator for the PlayStation Vita / PS TV.

VitaPS2 runs the [Play!](https://github.com/jpd002/Play-) emulation core (EE and
IOP recompilers, VU, SPU2, high-level emulated BIOS, so **no BIOS dump is
needed**) on the Vita through its 32-bit ARM JIT, with a new portable
**software Graphics Synthesizer** renderer and a native Vita frontend.

> **Set expectations:** the Vita's Cortex-A9 is roughly 10x slower than what
> full-speed PS2 emulation needs on ARM. Commercial 3D games will run far
> below full speed. See [docs/RESEARCH.md](docs/RESEARCH.md) for the analysis,
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
| SELECT + START | back to game list |
| SELECT + L | toggle performance overlay |

On PS TV, a DualShock 3/4's L2/R2/L3/R3 work directly.

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
