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

## Tier 2 progress (started 2026-11-08)

Owner's goal: every movie locked at 30 pictures/s, which needs 60 emulated fps
(Resident Evil 4's intro is at 33). Built in steps, each measured on a PC first.

**Measuring without a Vita.** qemu-user does not run under WSL1 (the PC's WSL is
version 1), so `perf_gap.py` cannot run there; the CI still runs it. Instead:
- `jit_arm_dump` (host tool, `tools/jit_arm_dump.cpp`) compiles EE blocks from a
  RAM dump with the Vita's ARM code generator in the Vita's modes and writes the
  code. Nothing runs, so it works on x86.
- `tools/jit_arm_report.py RUN_DIR` takes a Vita run's `eehot.txt` and `eeram.bin`
  (`dump_ee_ram=1`), compiles every hot block, disassembles it and reports ARM
  instructions per EE instruction weighted by EE time, and where they go.
- The host runner counts returns to the executor (`dispatch:` line) and has
  `--vita-jit` (the Vita's JIT modes) and `--no-indirect`. `make_workload_elf.py
  call` is a call-heavy workload (jal, jr ra, jalr): 146 returns to the executor
  per 1000 EE instructions, 2.4x slower than the integer workload on a PC.

- `tools/vita_jit_fuzz.sh COUNT [settings]`: the ARM JIT checked on the Vita itself:
  random EE programs (plain and with loops) must end with the same checksum as on
  the PC (the Vita app writes the EE's registers to `eegprs.txt` when an autostart
  run ends). The defaults (indirect dispatch on, regions off) pass 20 of 20.
- `jit_arm_dump RAM --find-regions START END` lists regions; `--region ADDRESS`
  compiles one.

**What RE4's hot code costs** (`jit_arm_report.py`, 79 blocks, 47% of EE time):
blocks are 2 to 6 EE instructions; the static code is about 17 ARM instructions
per EE instruction (both exit paths counted). 13% are loads and stores of guest
registers, 13% the block exit's fields (cycle quota, PC, exception, delayed
jump), 8% branches. The exit costs about as much as the instructions in a block
this short, so the next steps are cheaper exits, then blocks merged into larger
units.

| Step | State | Result |
|---|---|---|
| 1. **Indirect jumps in compiled code** (patches 0077, 0078): `jr ra` and unlinked branches looked up in a per-CPU table of {guest PC, link entry} by the chained thunk, instead of returning to the C++ loop | Done, on by default (`indirect_jit=0` turns it off). AArch32 with chained frames only | RE4 intro: returns to the executor 188000 -> 21000 per second (EE), 69000 -> 26000 (IOP); 32.1 -> 32.9 emulated fps, 15.0 -> 15.3 pictures/s |
| 2. **Cheaper block exits** (patch 0081): blocks ending in `j`/`jal`/`jr`/`jalr` jump without testing the delayed jump address; blocks with only integer arithmetic, logic, shifts, HI/LO moves and branches (no memory access, coprocessor, MMI or syscall) skip the exception test (`CBasicBlock::AnalyseExit`) | Done, on by default (`cheap_exits=0` turns it off; part of the JIT cache fingerprint) | Exit of a hot RE4 block 17 -> 11 ARM instructions. RE4 intro: 31.5 -> 32.1 emulated fps, 15.1 -> 15.4 pictures/s. 40 PC fuzz programs, 20 + 12 on the Vita: same checksums |
| Bug found on the way: Play compiles `addiu zero, zero, N` as a syscall (the IOP's dynamic linking stubs). Counted as arithmetic, the exit skipped its exception, and with indirect dispatch (no return to the executor) the IOP never ran the call: RE4 hung at boot. Fixed: that form keeps the test. The EE fuzzer does not generate IOP code, so this showed only in a game: **JIT exit changes need an IOP-heavy game boot on the Vita**, not just the fuzz | Fixed | |
| 2b. **Conditional branch exits** (patch 0083): conditional branches were already branchless (the delayed jump address is a select); a taken exit now sets nPC from nPC + a constant instead of re-reading the delayed address through a copy | Done (part of `cheap_exits`) | 2 instructions less on each taken conditional exit |
| 3a'. **Pinned context fields** (patch 0082, `pinned_fields`, on by default): which context fields do RE4's hot blocks touch most? Not guest registers (the hottest, v0, is 1.6% of ARM instructions; 7 allocatable ARM registers can't hold many) but the emulator's: delayed jump address 4.0%, nPC 3.5%, page table pointer 2.1% (every memory access), cycle quota 2.0%. The quota (read and written) lives in r9 and the page table pointer in r10 for the whole chain of linked blocks: loaded by the prolog, the quota stored by the epilog and the unwinding thunks; both leave the register allocator (`IsSymbolPinned`) | Done | Quota check per block `ldr; sub; str; cmp` -> `sub r9; cmp`; one load less per memory access. RE4 hot blocks: 17.09 -> 16.58 ARM instructions per EE instruction. RE4 intro on the Vita: 31.9 -> 32.5 emulated fps, 15.3 -> 15.6 pictures/s (two runs). Vita fuzz 32 of 32 |
| 3c. **Fast RAM path** (patches 0086, 0087, `fast_ram`, on by default): EE and IOP loads and stores test the address for RAM (EE: `tst addr, #0xFE000000`; IOP: 2 MB mirrored in 8 MB at 0 and 0x80000000) and access `ram + addr` straight; the RAM's base pointer (`CMIPS::m_fastRamBase`, after `m_pageLookup`) takes the page table's pinned register r10. Everything else (scratchpad, mirrors, I/O) goes through the page lookup and handler, now **after the function's return** (cold code, `AddColdCode`): no branch over them in the hot path | Done | `lw` 9 -> 4 ARM instructions executed (add, tst, bne, ldr), `sq` about 11 -> 9; the slow paths leave the I-cache. 64-bit equality compares (every EE `beq`/`bne`) 11 -> 5 (`eor`/`orrs`). In the JIT cache fingerprint |
| 3d. **Likely branch exits** (patch 0088): a block ending in `beql`/`bnel`/`blezl`/`bgtzl`/`bltzl`/`bgezl`(`al`) jumps to its own not-taken exit when the branch isn't taken (the delay slot is skipped) and falls through to a taken exit otherwise: no `nDelayedJumpAddr` write, read or reset. Not used for blocks whose epilog has work both exits need (EE FPU rounding, idle loops) | Done (part of `cheap_exits`) | RE4's hottest loop (`bnel a0, zero; lw a0, 8(a0)`, a list walk, 3.7% of EE time): 41 -> 26 ARM instructions per iteration. Fuzzer: `--likely` adds them (host tests keep their recorded programs) |
| 3e. **Memory exceptions flagged in the quota** (patches 0086, 0089): a guest memory handler that leaves an exception (an I/O write raising an interrupt check, the idle detector) sets the top two bits of the pinned quota (r9, then about -2^30), so the exit's quota test fails and the slow exit, which adds 2^30 back, returns to the executor. Loads and stores no longer make the exit test `nHasException` (`ldr; cmp; bne` per block). The quota's `sub` and `cmp` merge into `subs` | Done | The list-walk loop: 26 -> 22 ARM instructions. First version used the sign bit alone: a block costing more than the quota left overflowed back to a positive quota (the EE would not return for 2^31 cycles); caught by reading, fixed before any run with it |
| Results of 3c-3e on the Vita (RE4 intro, warm, 4 runs) | | Fast RAM, compares and likely exits: 16.2 -> 17.65 pictures/s (33.8 -> 36.8 emulated fps). Exceptions in the quota, `subs`, cold 64/128-bit slow paths: 18.35 (38.2). Per emulated frame: EE code 14.3 -> 11.1 ms, IOP code 4.1 -> 3.0 ms. Vita fuzz with likely branches: 16 of 16 (fast RAM, likely exits), 7 of 7 (quota flag); host tests 33 of 33 |
| 3a. **Regions** (patches 0079, 0080): consecutive blocks of a function compiled as one (`CEeRegionBlock`): branches between them jump to labels (no exit, link or dispatch), cycles counted per part as before, quota checked on loops back, exits through the indirect dispatch | Done, **off by default** (`jit_regions=1`, host `--regions`) | Same results: 40 EE fuzz programs (plain and with loops, 125-276 regions each) give the same checksums; Silent Hill 2 on a PC, 1500 frames, 672 regions: the same final picture. RE4 on the Vita: stable, but **no gain** (15.0 pictures/s either way): its hot code is small functions joined by calls and returns, which end a region |
| Regions compiled again and again: after a quota exit a loop is entered mid-region, and each such entry compiled a new overlapping region of up to 256 instructions. Loop programs took over 12 s on the Vita (correct results, given 60 s). Fix: an entry inside a region's range gets a plain block | Fixed | Loop fuzz on the Vita with regions: 3 of 3 within 12 s; PC fuzz still 40 of 40 |
| 3b. Guest registers kept in host registers across a region's parts (the Jitter's allocator spills at every label today) | Superseded by 4 | |
| 4. **The tier 2 recompiler** (patch 0094, Play `ee/Tier2.cpp`, `tier2=1`, off by default): a compiler of its own next to the JIT. See below | First version done, correct, no gain yet | RE4 intro movie, 200 s runs, movie section: 18.54 pictures/s against 18.08 without (444 regions, 37000 EE instructions); EE code 11.1 ms per frame either way |

### The tier 2 recompiler (2026-11-09)

Not the Jitter: its own front end, IR and ARM code generator, so nothing is spilled at
block edges and the upper halves of registers are tracked across the whole region.

- **Regions.** From a block start, the code is followed through conditional branches,
  jumps and calls: `jal` inlines the callee (3 levels), whose `jr ra` checks that ra is
  the inlined call's return address (else it leaves to ra). Likely branches get their
  delay slot as a block of its own. Anything not handled (FPU, MMI, `lq`/`sq`, `mult`,
  `syscall`, COP0...) ends the block with an exit to that address: the JIT runs it.
  Idle loops (`CEeBasicBlock::IsIdleLoopRange`) and blocks with per-block settings are
  left to the JIT too. Up to 48 blocks, 768 instructions.
- **IR**: operations on guest registers with their full 64-bit meaning (32-bit ALU and
  shifts, 64-bit logic/add/shifts, `movz`/`movn`, compares, loads and stores of 8-64
  bits), a temporary for branch conditions the delay slot changes, terminators (goto,
  conditional branch, return check) with edges carrying the cycles of the guest block
  they end and whether the quota is checked there.
- **Two back ends.** An interpreter (any host: `--tier2` in the host runner, the fuzz
  tests) and AArch32 code (`tier2=1`, needs chained frames, pinned fields with the fast
  RAM base, exception poison, indirect dispatch: the Vita defaults). The ARM code keeps
  up to 7 guest registers in r4-r8, r2, r3 for the whole region (chosen by use, loops
  counted 8x), loaded at the link entry and stored at exits. Each register's upper half
  is "the sign extension of the lower 32 bits" (most 32-bit code) or "in the context":
  32-bit operations need no upper half at all, 64-bit compares and logic on sign-extended
  values use 32-bit instructions, and the upper half is only written where a block that
  needs it in the context is reached (edges between blocks) or the region is left.
  Loads and stores use the JIT's fast RAM path (`tst addr, #0xFE000000`), with the
  memory handlers after the code (r2/r3 saved in the frame, exceptions flagged in the
  quota like the JIT's).
- **Same conventions as the JIT's blocks**: chained frame (same prolog, link entry,
  epilog), cycles counted per guest block and the quota checked at each block's end, exits
  through the indirect dispatch (`RegionExitTrampoline`): JIT blocks link to regions and
  regions jump to JIT blocks.
- **Hot code only.** The profiler samples the EE's PC every millisecond; a block start
  seen 8 times (`tier2_hot`) gets a region, built before the next EE slice (the JIT's block
  there is dropped and its links moved to the region). `tier2_eager=1`: a region at every
  block start (tests). Regions are dropped with the code they hold (inlined callees
  included) when it is cleared.
- **Checks.** Host tests: 6 fuzz programs through tier 2 (`ee_fuzz_test_*_tier2`, new
  generator options `--calls`: leaf and non-leaf functions, `jalr`, callees returning past
  their call site; `--spr`: loads and stores in the scratchpad, the memory handlers). On
  the PC, 90 more programs against the JIT. On the Vita (`vita_jit_fuzz.sh ... tier2=1
  tier2_eager=1`): 59 of 59 with native code. RE4 boots, loads and plays its intro with
  native regions.
- **Debugging aids**: `jit_arm_dump RAM ADDRESS --tier2` prints a region's IR (and its ARM
  code in `TIER2_DUMP`), `tier2_dump=1` writes each region built in
  [`tier2_lo`, `tier2_hi`) with its guest code on the Vita, `tier2_lo`/`tier2_hi` keep
  native code to an address window (the rest interpreted), `tier2.txt` lists the regions
  and the instructions that ended blocks.
- **Added (same day)**: `lq`/`sq` (through q8, the memory handlers with a pointer into the
  context), `mult`/`multu` (HI/LO sign extended, rd = LO), `mfhi`/`mflo`/`mthi`/`mtlo`;
  fuzz option `--more`. 40 programs on the PC, 18 on the Vita with native code. RE4 movie
  section (200 s runs): 690 regions, 66000 EE instructions; EE code 10.75 ms per frame
  against 11.15 without (-4%); compile time 0.73 against 0.38 ms per frame. Still ending
  blocks: MMI 188, `syscall` 108, `swc1` 84, `mtc1`/`ctc1` 76, `lwc1` 61, `ldl` 41,
  `mfc0` 38, `divu` 36.
- **A trap found on the way**: RE4 test runs press buttons at fixed real-time seconds.
  A faster or slower EE moves the game's timeline, so a 115 s run can end before the movie
  starts. "No pictures" then looked like a hang (an address bisection chased it for an
  hour). Look at the frames (`frame_dump_every`) before calling a run broken.

**Why no gain yet**: what ends RE4's blocks (`tier2.txt`, movie section): `lq` 86, `swc1`
72, `sq` 67, MMI 59, `syscall` 57, `mult` 52, COP0 35, `lwc1` 35, COP1 arithmetic 31, `ldl`
31, `div` 16. Its hot code is FPU, 128-bit and multiply code; regions stop at the first such
instruction, and 325 hot block starts get no region at all. Next: `lq`/`sq` and the
common MMI moves (`por`, `pcpyld`...), `mult`/`div`/`mfhi`/`mflo`, then FPU loads, stores
and arithmetic with the JIT's clamping.

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
