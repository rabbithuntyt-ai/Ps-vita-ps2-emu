# Native (HLE) MPEG movie decoding: notes

Goal: movies at full speed on the Vita. Emulated, Silent Hill 2's opening
movie costs ~1.3 ms of Vita CPU per macroblock (EE player code, IPU commands,
VIF unpack, VU, GS): ~0.8 pictures/s against 30. No single stage dominates, so
the plan is to take over whole pictures: decode the MPEG-2 stream natively
(ARM + NEON) and hand the game finished pictures.

## Tools

- Autostart keys: `dump_ee_ram=1` writes EE RAM to `ux0:data/VitaPS2/eeram.bin`
  30 s after MPEG decoding starts. During movies `ux0:data/VitaPS2/eehot.txt`
  lists the hottest EE PC buckets (32 bytes) every 10 s (profiler sampling).
- Disassembly on a PC: `apt install binutils-mips-linux-gnu`, then
  `mips-linux-gnu-objdump -D -b binary -m mips:5900 -EL --start-address=... eeram.bin`
  (the first 32 MB of the dump; file offsets are EE addresses).

## Silent Hill 2 (SLUS-20228)

- Sony libmpeg + libipu 2.2.0 (`PsIIlibipu  2200` at 0x3631E0). Movies are
  `.pss` (MPEG-2 program stream), `data/movie/*.pss`.
- Game call site 0x2A56C4: `sceMpegGetPicture(mp, buf, 1024)`; a negative
  result prints "sceMpegGetPicture() decode error".
- `sceMpegGetPicture` = 0x20E518: `sys = mp->sys (+0x40)`;
  `sys+0xE4 = mbcount`, `sys+0xD8 = buf | 0x20000000` (uncached),
  `sys+0xDC = 0`, `sys+0xB0 = 0`, `sys+0xE0 = 0`; then the common decode
  routine 0x20E8B0. Output: RGB32 macroblocks (16x16x4 bytes), raster order.
- `sceMpegGetPictureRAW8xy(mp, buf, w, h)` = 0x20E560 (sets `sys+0xDC = w*16`,
  `sys+0xE0 = h*16`, `sys+0xE4 = w*h`).
- 0x20E8B0 loops on 0x20B4D0 (next start code, returns 0..4) and dispatches
  through the table at 0x3B5F00; 0x20EB60 decodes a picture with the frame
  buffers at `sys+0x94/0x98/0x9C` (reference frames in EE RAM: motion
  compensation runs on the EE).
- 0x20AE88: issue an IPU command and spin on IPU_CTRL / IPU_CMD busy (every
  5001 spins it calls 0x20E728). ~20% of EE samples during the movie.
- 0x1F8xxx-0x1F90xx: kernel syscall stubs (~16% of EE samples: semaphores and
  interrupt handling around IPU commands).
- 0x207F00: scratchpad DMA (channel 9, 0x1000D400).
- The IPU gets the stream through DMA channel 4 from libmpeg's ring buffer,
  refilled by the game's callback (EE code).

### Mapped from a dump 30 s into the opening movie (`eeregs.txt` next to the dump)

- `mp` (sceMpeg) is a global at 0x1213080: width 512, height 384 (32x24
  macroblocks), `mp->sys` (+0x40) = 0x152E880.
- `sys+0x0C..0x38`: the game's callbacks (0x2A5810, 0x2A5820, 0x2A5870,
  0x2A58A0, 0x2A58D0), registered with sceMpegAddCallback; one of them
  refills the stream when the IPU runs dry.
- `sys+0xD8` = output buffer (0x18AB480 | 0x20000000), `sys+0xE4` = 0x400.
- `sys+0xFC..0x110`: four YCbCr 4:2:0 frame buffers, 0x48000 bytes each
  (512x384x1.5): 0x152EFC0, 0x1576FC0, 0x15BEFC0, 0x1606FC0.
- DMA3 (from IPU) targets the scratchpad (0x80003600): macroblocks go to SPR,
  then to the frame buffers.
- DMA4 (to IPU) runs a source chain: 253 REF tags + 1 REFE tag at
  0x1211F00..0x1212ED0, each 0x80 qwords (2 KB) from a contiguous ring
  0x14AE880..0x152D880 (~508 KB). The REFE tag ends the chain; libmpeg
  restarts DMA4 at the ring start to wrap.
- Stream read position = DMA4 MADR - IPU input FIFO + IPU_BP.
- The ring belongs to the game: Sony's ezmpeg sample `ViBuf` at 0x1211E40
  (the callbacks pass it to viBufAddDMA / StopDMA / RestartDMA / GetTs):

  | Offset | Field | Value in the dump |
  |---|---|---|
  | +0x00 | data (ring start) | 0x14AE880 |
  | +0x04 | tag (DMA tags, uncached) | 0x21211F00 |
  | +0x08 | n (2 KB units) | 256 |
  | +0x0C | dmaStart (first unit queued to the IPU) | 175 |
  | +0x10 | dmaN (units queued) | 79 |
  | +0x14 | readBytes (written, not queued yet) | 70479 |
  | +0x18 | buffSize | 0x80000 |
  | +0x1C.. | saved env: D4 MADR/TADR/QWC/CHCR, D3 MADR/QWC/CHCR, IPU_BP, IPU_CTRL | |
  | +0x40 | sema, +0x44 isActive, +0x48 totalBytes | |
  | +0x50 | ts table, +0x54 n_ts, +0x58 count_ts, +0x5C wt_ts | |

  Valid stream = from the IPU read position through the queued units, plus
  readBytes after them (wrapping at buffSize).
- Callbacks: 0x2A5810 returns 1; 0x2A5820 (background: read the disc, then
  viBufAddDMA 0x2A4620); 0x2A5870 (0x2A4890, stop DMA); 0x2A58A0 (0x2A4980,
  restart DMA); 0x2A58D0 (timestamps, 0x2A5040 returns PTS/DTS).

### libmpeg details found so far

- `sceMpeg` layout (SDK): +0 width, +4 height, +8 frameCount, +0x10 pts,
  +0x18 dts, +0x20 flags, +0x28 pts2nd, +0x30 dts2nd, +0x38 flags2nd,
  +0x40 sys. The game reads flags right after each picture (0x2A56EC:
  `(flags >> 3) & 3 == 3`, then `flags & 0x180`).
- Callback table at `sys+0x08`: (data, function) per type: 0 error
  (0x2A5810), 1 no data (0x2A5820: reads the disc, viBufAddDMA), 2 stop DMA
  (0x2A5870), 3 restart DMA (0x2A58A0), 4 background (none), 5 timestamp
  (0x2A58D0: writes pts/dts into the cbdata at +8/+16).
- At picture output (0x20C950) libmpeg calls 0x20C638(frame, &mp->pts,
  &mp->dts, &mp->flags), then the same for the second frame into pts2nd...,
  and ORs 0x40 into the flags when its state (0x363EAC) is 2. 0x20C638 takes
  the frame's recorded PTS or interpolates it (sys+0x70..0x90, helper
  0x1F51D0) and builds the flags from picture structure state (sys+0xF8).

### Plan: shadow mode first

The native decoder (src/common/Mpeg2Decoder.cpp) decodes Silent Hill 2's
movies correctly on a PC (tests/mpeg2_decoder_tests: I/P/B, 268 pictures/s on
x86). Before replacing libmpeg:

1. Shadow mode: tee every byte DMA4 delivers to the IPU into the native
   decoder; after each sceMpegGetPicture compare the native picture with
   libmpeg's RGB32 output and log frameCount/pts/dts/flags next to the native
   picture type. Measures the native decoder's speed on the Vita too.
2. Replace mode with the observed rules for flags and timestamps.

### Takeover protocol

At `sceMpegGetPicture` (0x20E518): decode the next picture natively from the
ring into `buf`, then move DMA4 (MADR/TADR/QWC) and the IPU (FIFO, BP)
forward as if the IPU had consumed those bytes. The game's ViBuf code reads
DMA progress from the registers as usual, so no game code changes. Start at
the first picture of a stream (the decoder needs the sequence header); fall
back to the emulated path if the queued data does not hold a whole picture.
Still to settle: the fields libmpeg updates per picture that the game reads
(frame count, picture type, timestamp callback per picture).

## Takeover design (next steps)

1. Map the input: dump `mp`, `mp->sys` and the DMA4 chain during the movie;
   find the ring buffer (start, size, read/write positions) and how libmpeg
   requests more data (callback, sceMpegCbNodata).
2. Intercept 0x20E518 (identified by its code bytes, not the address, so other
   libmpeg 2.2 games match): decode the next picture natively from the ring
   buffer into `buf` (RGB32 macroblocks), advance the ring read position,
   update the `sys` fields the caller reads, return like the original. Fall
   back to the emulated path when the ring buffer does not hold a whole
   picture (the game's callback has to run first).
3. Native MPEG-2 video decoder: headers, VLC (Play's IPU tables), IDCT
   (Framework fast IDCT), motion compensation and YCbCr->RGB with NEON.

## Replace mode (src/common/MpegHle.cpp, autostart key `mpeg_hle=1`)

Status: works up to the display hand-off; not verified to play through yet.

- Patch 0060 adds the hooks: IPU input sink (DMA4 data to the frontend
  instead of the IN FIFO, with back pressure), IPU reset notification, an HLE
  syscall range 0xA00-0xA0F (0x800-0x900 belongs to CLibMc2: 0x900 is
  GETDBCSTATUS) and a vblank hook on the EE thread for code patches.
- `sceMpegGetPicture` is found by its code (masked jal) and its first 14
  words become a stub; the original epilogue stays, so a state saved inside
  libmpeg still returns correctly. The stub loops: `syscall 0xA00` (start) /
  `0xA01` (again) either returns a picture (v0 = 1) or asks for the game's
  no-data callback (`sys+0x14`, called with a cbdata of type 1 at sp+8).
- Silent Hill 2 asks for RAW8 macroblocks (256 Y, 64 Cb, 64 Cr; 0x48000
  bytes for 512x384; its output buffers are 0x60000 apart) and converts the
  colours itself. Writing RGB32 overflowed into the next buffer.
- The game reads `mp->frameCount` (+8): the vblank display handler (0x2A20D8)
  shows a buffer only while frameCount < the movie's end frame (0x12118F4),
  and after a picture with frameCount 0 it builds each output buffer's VIF1
  DMA chain (0x2A5738 -> 0x2A17C0). A nonzero first frameCount leaves the
  chains unbuilt: the display then waits on VIF1 forever (sceDmaSync at
  0x1F8338 from 0x2A1DF8). frameCount starts at 0 for a new movie and
  continues from the saved value after a state load.
- Every DMA4 register write is the game's own ViBuf code (0x2A4xxx:
  viBufAddDMA reads D4_MADR to count consumed 2 KB units and appends REF tags),
  so the sink needs nothing else from libmpeg.
- Decoder speed (host, x86, 300 pictures of the opening movie, output
  identical at every step): 268 pictures/s -> 588 with a word-wise bit reader,
  11-bit VLC lookup tables built from Framework's tables, dequantisation of
  the decoded coefficients only and an unclamped prediction path. On ARM the
  prediction and block add use NEON (Mpeg2Simd.h); the HLE runs a NEON vs
  scalar self-test at startup and falls back to scalar on any mismatch.
