# Per-block render target coherency (design, 26 Nov 2026)

## Why

RE4 streams its textures and palettes every frame into GS memory that its post-processing render
targets also use (0x310000 512x512, 0x348000 128x512). Each transfer into a target that is still
GPU-dirty reads the target's rows back first. The rows then become stale and are uploaded again
when the target is used.

Measured with host `--readback-liveness` on the RE4 walk:
- Of the 610,176 GS blocks read back for transfers, 47% are never read before a transfer or
  another read-back overwrites them.
- Most of the rest are read only by the uploads into the target that follow.
- On the Vita this costs about 23 ms of read-backs and about 15 ms of uploads per game frame.

The row-range model is what causes this:
- A target holds one dirty rectangle and one stale row range.
- Read-back pixels must leave the dirty rectangle. The RGBA8 copy is lossy (alpha above 0x80
  saturates), so writing GPU pixels over newer GS bytes would corrupt them.
- So any transfer into the middle of a dirty target reads whole rows back, or even the whole
  rectangle.

## Model

Each target gets per-block flags. A block is one 256-byte GS block: 8x8 pixels for 32-bit
formats, 16x8 for 16-bit.

- **STALE:** GS memory is newer than the GPU copy for this block. A transfer wrote it while the
  target was dirty there. Read-backs must not write this block, and the GPU copy must be
  refreshed from GS memory before the target is drawn into or sampled.
- **CLEAN:** the block lies inside the dirty rectangle, but the GPU copy equals GS memory (it was
  refreshed from GS memory after being stale, and not drawn since). Read-backs must not write
  it back: a lossy round trip would corrupt transfer data such as palette alpha.

## Operations

1. **Transfer into a dirty target (BeginTransferWrite), different layout.**
   - For the GS blocks the transfer writes, find the target blocks holding those bytes (the
     same 256 bytes in the target's layout).
   - A block the transfer writes completely needs no read-back: mark it STALE after the transfer.
   - A block written partly is read back first (that block alone), then marked STALE.
   - Rows outside the touched blocks are not read back.
2. **Read-back (DownloadTarget):** blocks flagged STALE or CLEAN are skipped when writing
   pixels to GS memory.
3. **Before drawing into or sampling the target:** STALE blocks are uploaded from GS memory
   (GPU upload of small rectangles) and become CLEAN.
4. **Drawing (MarkTargetDirty):** blocks in the drawn rectangle lose CLEAN. STALE blocks were
   already resolved in step 3.
5. **Target removal, relayout, shrink, trim, pool:** resolve STALE blocks first (upload), then
   the existing logic runs. Downloads skip CLEAN blocks.
6. **A target that is not dirty where a transfer lands:** keeps the existing row logic
   (`validRows` cut or `MarkRowsStale`).

## Checks

- PC: screenshots identical over the RE4 walk with the toggle on and off. Read-back and upload
  pixel counts fall.
- Host test: a unit test of the block mapping (transfer GS block to target block), checked
  against GS_SURFACE per pixel.
- Vita: interleaved A/B; frame dumps.
- Toggle: `block_coherency` (off until verified).

## Status (26 Nov 2026): implemented behind `block_coherency` (off), not exact yet

Host `--block-coherency`; `--state-breaks` adds a read-back breakdown, and a block mapping self-check
is printed: 0 failures over the 32- and 16-bit layouts.

**First version** (block path only for transfers that are not whole pages):
- Screenshots identical over the RE4 walk.
- But the transfer read-backs only fell from 36.5M to 34.5M pixels: the big one came from
  whole-page transfers, which still went through the row path.

**With whole-page transfers too:**
- Transfer-triggered read-backs went to zero.
- Bugs found on the way:
  - texture cache invalidation was missing for partial-block read-backs
  - a pending block transfer could point to a target deleted by a state load
  - stale blocks were resolved inside a loop over m_targets (target removal there corrupted the
    heap: a crash in the EE thread's MPEG patcher)
- Still not exact:
  - shots 1 and 7 differed
  - after making flatten upload only stale blocks, 6 of 8 differed
  - the target lifecycle changes (0x310000 appears as 512x256 and 256x512 variants, with large
    FindTarget read-backs): the block flags interact with FindTarget's row-based decisions
    (validRows, stale rows, relayout refusal for flagged targets, the strip cover)

**Next, for exactness:**
- Trace the first frame that differs (frame dumps of every target around it).
- Compare the target list per flip with the toggle off and on (`--gs-diag`).
- Make FindTarget/RemoveTargetsOverlapping treat flagged targets as the row model would. For
  example, flatten instead of refusing relayout, so relayouts still happen.

With the toggle off the code is inert: no flags are ever set, and PC screenshots are identical.
