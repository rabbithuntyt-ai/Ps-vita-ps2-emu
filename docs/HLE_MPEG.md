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
