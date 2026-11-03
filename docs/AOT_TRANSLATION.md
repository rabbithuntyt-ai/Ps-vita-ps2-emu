# Ahead-of-time translation: PS2 code at near-native speed

Status: design. Nothing here is implemented yet beyond the JIT it builds on.

## Why the JIT costs 5-8 ARM instructions per PS2 instruction

The performance gap chart (README) measures 5.3 (integer) to 7.9 (FPU) ARM
instructions per EE instruction. The JIT compiles one basic block at a time,
a few instructions long, and knows nothing outside it. So every block pays for:

- **64/128-bit registers.** EE general registers are 128 bits; most code
  only ever uses the low 32. A block can't prove the upper halves are dead,
  so it keeps or rebuilds them.
- **Guest state at block edges.** Guest registers live in a context
  structure in memory; blocks load what they use and store what they change.
  Chained frames and loop promotion reduce this, but every boundary is still a
  guess.
- **Memory access checks.** Every load and store goes through the page
  table (RAM, scratchpad, hardware registers?), even a stack access.
- **Timing and exceptions.** Cycle counting and interrupt checks per block.
- **FPU clamping.** PS2 floats aren't IEEE: results are clamped, because the
  block can't prove values stay in range.
- **Compile time.** About 3 ms per block on the Vita (scene-change stalls).

A static recompiler that sees a whole function, or the whole program, can
remove most of this. Projects that recompile whole games ahead of time
(N64Recomp, XenonRecomp, PS2Recomp) get close to native speed, but each needs
manual work per game. The goal here is the same code quality **without
per-game work**, entirely on the Vita.

## The idea: an install step that rewrites the game's code

```
disc ELF + overlays ──► discovery ──► lift to SSA IR ──► whole-program analysis
                                                              │
     JIT (fallback) ◄── runtime guards ◄── ARM code bundle ◄── optimize + codegen
```

1. **Discovery.** Code comes from the boot ELF and the overlays it loads
   from the disc, plus every block entry the JIT saw while the compatibility
   harness played the game (this catches computed jumps and code loaded at
   run time). Functions are recovered from `jal` targets and `jr ra` returns;
   jump tables come from `jr` patterns with bounded indices.
2. **Lift** each function to an SSA intermediate form with explicit 128-bit
   registers, HI/LO, FPU, COP0 and memory.
3. **Whole-program analysis**, the part a block JIT can't do:
   - **Register width:** prove which values never need the upper 32/96 bits,
     and keep those in plain 32-bit ARM registers. Most game code is 32-bit C
     compiled for a 64-bit CPU, so this is the biggest single win.
   - **Calling convention:** recognize MIPS ABI functions (arguments in
     a0-a3, callee-saved s0-s7, `sp` frames). Translated functions call each
     other natively with guest registers in host registers; guest state goes
     to memory only where unknown code can observe it (calls into the JIT,
     interrupts, syscalls).
   - **Memory classes:** addresses derived from `sp`, `gp` or known RAM
     pointers go straight to RAM (base + offset, masked); only addresses that
     can reach hardware registers keep the full check.
   - **Float ranges:** skip clamps where values provably stay finite
     (conversions from integers, bounded arithmetic). Everywhere else keeps the
     exact PS2 behavior.
   - **MMI to NEON:** 128-bit parallel instructions map to NEON with known
     lane widths.
   - **Timing:** cycles counted per function and per loop iteration;
     interrupt checks at loop back edges and calls instead of every block.
4. **Optimize and generate code** with our own Jitter (the compiler the JIT
   already uses), extended to whole functions: function-wide register
   allocation and the analyses above. It runs **on the Vita**: no PC is
   needed at any point.
5. **Runtime.** Translated functions are mapped by guest PC. Each carries a
   hash of the guest code it came from: if the game overwrites or loads
   different code there (overlays, self-modifying code), the guard fails and
   the JIT runs that code. Anything never translated also runs in the JIT, so
   the game always works.

## How it runs on the Vita: tier 2 in the background

Everything happens on the device, while the game plays:

- The JIT keeps per-block execution counts (the hotspot sampler already
  exists).
- A low-priority thread on an idle core (core 2 between VU1 and SPU2 work)
  regroups hot blocks into whole functions and loops, and recompiles them
  with the analyses above.
- The new code is swapped in at a block boundary, and written to the
  persistent JIT cache, so the next session starts with it.

This is the "longer load times" trade spread over play: the first sessions
of a game are as fast as today and get faster as hot code is translated;
later sessions start translated.

## Persistent JIT cache (implemented: patches 0064 and 0065)

The cache is the first building block (it removes scene-change stalls from
the second play on, and it stores tier-2 code later). What the code
generator does today, and what has to change:

- **Absolute addresses:** every pointer the AArch32 code generator embeds
  (calls into the emulator, memory handlers, constant tables) goes through
  one function, `CCodeGen_AArch32::LoadConstantPtrInRegister`, as a
  MOVW/MOVT pair. The eboot loads at a different base each run (seen in
  crash dumps: 0x8103D000, for example), so these need relocation records:
  an offset into the eboot image, or one of a few registered runtime bases
  (EE RAM, the context structure). A block with any other pointer is not
  cached.
- **Links between blocks** are patched at run time (chained frames): the
  cache stores each block as compiled, before linking, and links it again
  after loading.
- **Validation:** key = hash of the block's guest code + its compile hints
  (FPU rounding, idle-loop flag) + the emulator build. Any mismatch is a
  miss and the JIT compiles the block as today.
- **Testing:** the ARM build runs Play!'s tests under qemu in CI; a
  save-then-load test there (compile, store, reload in a fresh process,
  compare register checksums) checks the relocations without a Vita.

## What it could gain

Static recompilers with a strong optimizer typically reach 1.5-2.5 host
instructions per guest instruction, against our 5-8 (2.5-4x). Our Jitter is
a simpler optimizer than an offline compiler, so the on-device tier 2 should
be expected to land lower: **about 1.5-2.5x on EE-bound code**, mostly from
register width, calling conventions and direct RAM access. That's a large
part, not all, of the gap between the Vita and full-speed EE emulation in
heavy games. It doesn't speed up VU1 or the GS (VU1 on the GPU is a separate
item).

**Load time:** spread over the first play sessions: translation runs in the
background on an idle core, and later sessions load the translated code.

**First milestone:** run the four performance-gap workloads through the
translator. `tools/perf_gap.py` measures ARM instructions per PS2 instruction
for translated code the same way it does for the JIT, so the gain is
measurable before any game runs.

## Correctness

- The EE fuzzer (`tools/make_ee_fuzz_elf.py`) already checks register
  checksums between x86, ARM and every JIT mode; translated code joins the
  same test.
- A lockstep mode for development: run each translated function and its
  JIT version from the same state, and compare guest registers and stores at
  exit.
- Analyses are conservative: whatever can't be proven keeps the JIT's
  behavior.

## Legal note

Translation runs on the user's own disc image, on their Vita. The cache files
contain translated game code and are personal, like a save file: they aren't
distributed.
