#pragma once

// Render target pixels <-> GS memory (CGSH_Hardware's target uploads and read-backs).
// Targets hold RGBA8 with alpha doubled (GS 0x80 -> 255). Read: GS memory -> RGBA8 pixels
// (uploads). Write: RGBA8 pixels -> GS memory (read-backs). The block versions work a GS
// block (256 contiguous bytes, the same layout in every block of a page) at a time: a table
// of each pixel's place in the block replaces the address arithmetic per pixel, and for
// 32-bit formats the alpha conversion runs over the block's memory (NEON on ARM) and pixels
// move 8 bytes (two neighbours) at a time. Blocks the rectangle covers only in part use the
// per pixel versions. tests/target_pixels_tests.cpp compares the two.
#include <algorithm>
#include <cstring>
#include <vector>
#include "GsMemory.h"
#include "GsSurface.h"
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace TargetPixels
{
	inline uint8 GsAlphaToTarget(uint32 alpha)
	{
		return static_cast<uint8>(std::min<uint32>(alpha * 2, 255));
	}

	inline uint32 TargetAlphaToGs(uint32 alpha)
	{
		return (alpha + 1) / 2;
	}

	// Per pixel: the rectangle (x0, y0, width, height) of the buffer into 'pixels' (row
	// 'stride' pixels apart).
	inline void ReadPixels(const uint8* ram, uint32 psm, uint32 fbp, uint32 fbw, uint32 x0, uint32 y0, uint32 width, uint32 height,
	                       uint32* pixels, uint32 stride)
	{
		GS_SURFACE surface;
		surface.Init(psm, fbp, fbw);
		bool is16 = GsMemory::IsPsm16(psm);
		bool is24 = GsMemory::IsPsm24(psm);
		for(uint32 y = 0; y < height; y++)
		{
			uint32 rowBase = surface.RowBase(y0 + y);
			const uint32* rowTable = surface.RowTable(y0 + y);
			uint32* dst = pixels + y * stride;
			if(is16)
			{
				for(uint32 x = 0; x < width; x++)
				{
					uint32 offset = surface.Offset(rowBase, rowTable, x0 + x);
					uint32 color = GsMemory::Color16To32(*reinterpret_cast<const uint16*>(ram + offset));
					dst[x] = (color & 0x00FFFFFF) | (static_cast<uint32>(GsAlphaToTarget(color >> 24)) << 24);
				}
			}
			else
			{
				for(uint32 x = 0; x < width; x++)
				{
					uint32 offset = surface.Offset(rowBase, rowTable, x0 + x);
					uint32 color = *reinterpret_cast<const uint32*>(ram + offset);
					uint32 alpha = is24 ? 0x80 : (color >> 24);
					dst[x] = (color & 0x00FFFFFF) | (static_cast<uint32>(GsAlphaToTarget(alpha)) << 24);
				}
			}
		}
	}

	inline void WritePixels(uint8* ram, uint32 psm, uint32 fbp, uint32 fbw, uint32 x0, uint32 y0, uint32 width, uint32 height,
	                        const uint32* pixels, uint32 stride)
	{
		GS_SURFACE surface;
		surface.Init(psm, fbp, fbw);
		bool is16 = GsMemory::IsPsm16(psm);
		bool is24 = GsMemory::IsPsm24(psm);
		for(uint32 y = 0; y < height; y++)
		{
			uint32 rowBase = surface.RowBase(y0 + y);
			const uint32* rowTable = surface.RowTable(y0 + y);
			const uint32* src = pixels + y * stride;
			for(uint32 x = 0; x < width; x++)
			{
				uint32 color = src[x];
				uint32 gsColor = (color & 0x00FFFFFF) | (TargetAlphaToGs(color >> 24) << 24);
				uint32 offset = surface.Offset(rowBase, rowTable, x0 + x);
				if(is16)
				{
					*reinterpret_cast<uint16*>(ram + offset) = static_cast<uint16>(GsMemory::Color32To16(gsColor));
				}
				else if(is24)
				{
					auto pixel = reinterpret_cast<uint32*>(ram + offset);
					*pixel = (*pixel & 0xFF000000) | (gsColor & 0x00FFFFFF);
				}
				else
				{
					*reinterpret_cast<uint32*>(ram + offset) = gsColor;
				}
			}
		}
	}

	// A format's block: its size in pixels and the place (x | y << 8) of each unit of the
	// block in memory order, from the page table of GS_SURFACE; 'valid' false if the blocks
	// of a page do not all share the first one's layout.
	struct BLOCK_LAYOUT
	{
		bool valid = false;
		uint32 width = 0, height = 0, unitBytes = 0;
		uint16 place[128] = {};
		bool pairs = false; //every 8 bytes are two neighbouring pixels of one row (32-bit)
		//32-bit: the usual column layout (4 columns of 8x2 pixels; in each column's 16 units, group g
		//of 4 holds pixels 2g, 2g+1 of the column's first row, then the same of its second row):
		//the NEON block move below applies
		bool columns32 = false;
	};

	inline BLOCK_LAYOUT BuildBlockLayout(uint32 psm)
	{
		BLOCK_LAYOUT layout;
		GS_SURFACE surface;
		if(!surface.Init(psm, 0, 1) || surface.nibbles) return layout;
		bool is16 = GsMemory::IsPsm16(psm);
		layout.unitBytes = is16 ? 2 : 4;
		layout.width = is16 ? 16 : 8;
		layout.height = 8;
		uint32 pageWidth = surface.pwMask + 1, pageHeight = surface.phMask + 1;
		uint32 units = 256 / layout.unitBytes;
		std::vector<bool> seen(units, false);
		for(uint32 y = 0; y < layout.height; y++)
		{
			for(uint32 x = 0; x < layout.width; x++)
			{
				uint32 offset = surface.RowTable(y)[x];
				if((offset >= 256) || (offset % layout.unitBytes)) return layout;
				uint32 unit = offset / layout.unitBytes;
				if(seen[unit]) return layout;
				seen[unit] = true;
				layout.place[unit] = static_cast<uint16>(x | (y << 8));
			}
		}
		//Every block of the page: the same layout from its own start
		for(uint32 by = 0; by < pageHeight; by += layout.height)
		{
			for(uint32 bx = 0; bx < pageWidth; bx += layout.width)
			{
				uint32 base = surface.RowTable(by)[bx];
				if(base % 256) return layout;
				for(uint32 y = 0; y < layout.height; y++)
					for(uint32 x = 0; x < layout.width; x++)
						if(surface.RowTable(by + y)[bx + x] != base + surface.RowTable(y)[x]) return layout;
			}
		}
		layout.pairs = !is16;
		for(uint32 unit = 0; layout.pairs && (unit < units); unit += 2)
		{
			if(layout.place[unit + 1] != layout.place[unit] + 1) layout.pairs = false;
		}
		if(!is16)
		{
			layout.columns32 = true;
			for(uint32 unit = 0; unit < 64; unit++)
			{
				uint32 column = unit / 16, group = (unit % 16) / 4, k = unit % 4;
				uint32 x = 2 * group + (k & 1), y = 2 * column + (k >> 1);
				if(layout.place[unit] != (x | (y << 8))) layout.columns32 = false;
			}
		}
		layout.valid = true;
		return layout;
	}

	//target_pixels_neon (on): 32-bit blocks moved by NEON registers (columns32 layouts only)
	inline bool g_neonColumns = true;
	inline uint32 g_prefetchBlocks = 2; //target_pixels_prefetch: blocks ahead to prefetch (0: none)

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
	// One 32-bit block (columns32) to the pixels of its 8x8 rectangle ('p' its top left, 'stride'
	// pixels between rows), alpha as BlockAlphaToTarget
	inline void BlockToPixels32(const uint8* block, uint32* p, uint32 stride, bool is24)
	{
		const uint8x16_t alphaMask = vreinterpretq_u8_u32(vdupq_n_u32(0xFF000000));
		for(uint32 column = 0; column < 4; column++)
		{
			uint8x16_t q[4];
			for(uint32 g = 0; g < 4; g++)
			{
				uint8x16_t c = vld1q_u8(block + column * 64 + g * 16);
				uint8x16_t a = is24 ? vdupq_n_u8(0xFF) : vqaddq_u8(c, c);
				q[g] = vbslq_u8(alphaMask, a, c);
			}
			uint32* row0 = p + (2 * column) * stride;
			uint32* row1 = row0 + stride;
			vst1q_u8(reinterpret_cast<uint8*>(row0), vcombine_u8(vget_low_u8(q[0]), vget_low_u8(q[1])));
			vst1q_u8(reinterpret_cast<uint8*>(row0 + 4), vcombine_u8(vget_low_u8(q[2]), vget_low_u8(q[3])));
			vst1q_u8(reinterpret_cast<uint8*>(row1), vcombine_u8(vget_high_u8(q[0]), vget_high_u8(q[1])));
			vst1q_u8(reinterpret_cast<uint8*>(row1 + 4), vcombine_u8(vget_high_u8(q[2]), vget_high_u8(q[3])));
		}
	}

	// The pixels of an 8x8 rectangle into one 32-bit block (columns32), alpha as BlockAlphaToGs
	inline void PixelsToBlock32(const uint32* p, uint32 stride, uint8* block, bool is24)
	{
		const uint8x16_t alphaMask = vreinterpretq_u8_u32(vdupq_n_u32(0xFF000000));
		for(uint32 column = 0; column < 4; column++)
		{
			const uint32* row0 = p + (2 * column) * stride;
			const uint32* row1 = row0 + stride;
			uint8x16_t a0 = vld1q_u8(reinterpret_cast<const uint8*>(row0)), a1 = vld1q_u8(reinterpret_cast<const uint8*>(row0 + 4));
			uint8x16_t b0 = vld1q_u8(reinterpret_cast<const uint8*>(row1)), b1 = vld1q_u8(reinterpret_cast<const uint8*>(row1 + 4));
			uint8x16_t q[4] = {vcombine_u8(vget_low_u8(a0), vget_low_u8(b0)), vcombine_u8(vget_high_u8(a0), vget_high_u8(b0)),
			                   vcombine_u8(vget_low_u8(a1), vget_low_u8(b1)), vcombine_u8(vget_high_u8(a1), vget_high_u8(b1))};
			for(uint32 g = 0; g < 4; g++)
			{
				uint8* out = block + column * 64 + g * 16;
				uint8x16_t a = is24 ? vld1q_u8(out) : vrhaddq_u8(q[g], vdupq_n_u8(0));
				vst1q_u8(out, vbslq_u8(alphaMask, a, q[g]));
			}
		}
	}
#endif

	inline const BLOCK_LAYOUT& GetBlockLayout(uint32 psm)
	{
		static const BLOCK_LAYOUT ct32 = BuildBlockLayout(CGSHandler::PSMCT32);
		static const BLOCK_LAYOUT ct24 = BuildBlockLayout(CGSHandler::PSMCT24);
		static const BLOCK_LAYOUT ct16 = BuildBlockLayout(CGSHandler::PSMCT16);
		static const BLOCK_LAYOUT ct16s = BuildBlockLayout(CGSHandler::PSMCT16S);
		static const BLOCK_LAYOUT none;
		switch(psm)
		{
		case CGSHandler::PSMCT32:
			return ct32;
		case CGSHandler::PSMCT24:
			return ct24;
		case CGSHandler::PSMCT16:
			return ct16;
		case CGSHandler::PSMCT16S:
			return ct16s;
		default:
			return none;
		}
	}

	// 64 32-bit pixels: GS alpha -> target alpha (doubled, saturated), or 255 (24-bit)
	inline void BlockAlphaToTarget(const uint8* in, uint8* out, bool is24)
	{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		const uint8x16_t alphaMask = vreinterpretq_u8_u32(vdupq_n_u32(0xFF000000));
		for(uint32 i = 0; i < 256; i += 16)
		{
			uint8x16_t c = vld1q_u8(in + i);
			uint8x16_t a = is24 ? vdupq_n_u8(0xFF) : vqaddq_u8(c, c);
			vst1q_u8(out + i, vbslq_u8(alphaMask, a, c));
		}
#else
		for(uint32 i = 0; i < 256; i += 4)
		{
			std::memcpy(out + i, in + i, 3);
			out[i + 3] = is24 ? 0xFF : GsAlphaToTarget(in[i + 3]);
		}
#endif
	}

	// 64 32-bit pixels: target alpha -> GS alpha ((a + 1) / 2); 24-bit keeps memory's top byte
	inline void BlockAlphaToGs(const uint8* in, uint8* block, bool is24)
	{
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
		const uint8x16_t alphaMask = vreinterpretq_u8_u32(vdupq_n_u32(0xFF000000));
		for(uint32 i = 0; i < 256; i += 16)
		{
			uint8x16_t c = vld1q_u8(in + i);
			uint8x16_t a = is24 ? vld1q_u8(block + i) : vrhaddq_u8(c, vdupq_n_u8(0));
			vst1q_u8(block + i, vbslq_u8(alphaMask, a, c));
		}
#else
		for(uint32 i = 0; i < 256; i += 4)
		{
			std::memcpy(block + i, in + i, 3);
			if(!is24) block[i + 3] = static_cast<uint8>(TargetAlphaToGs(in[i + 3]));
		}
#endif
	}

	template <bool write>
	inline void BlockPass(uint8* ram, uint32 psm, uint32 fbp, uint32 fbw, uint32 x0, uint32 y0, uint32 width, uint32 height,
	                      uint32* pixels, uint32 stride)
	{
		const auto& layout = GetBlockLayout(psm);
		auto perPixel = [&](uint32 x, uint32 y, uint32 w, uint32 h) {
			if((w == 0) || (h == 0)) return;
			uint32* p = pixels + (y - y0) * stride + (x - x0);
			if(write)
				WritePixels(ram, psm, fbp, fbw, x, y, w, h, p, stride);
			else
				ReadPixels(ram, psm, fbp, fbw, x, y, w, h, p, stride);
		};
		// Whole blocks only inside a page row of the buffer (x below 2048, no wrap)
		if(!layout.valid || (x0 + width > 2048) || (y0 + height > 2048))
		{
			perPixel(x0, y0, width, height);
			return;
		}
		uint32 bx0 = (x0 + layout.width - 1) / layout.width * layout.width;
		uint32 bx1 = (x0 + width) / layout.width * layout.width;
		uint32 by0 = (y0 + layout.height - 1) / layout.height * layout.height;
		uint32 by1 = (y0 + height) / layout.height * layout.height;
		if((bx0 >= bx1) || (by0 >= by1))
		{
			perPixel(x0, y0, width, height);
			return;
		}
		perPixel(x0, y0, width, by0 - y0);                         //rows above the blocks
		perPixel(x0, by1, width, y0 + height - by1);               //rows below
		perPixel(x0, by0, bx0 - x0, by1 - by0);                    //left of the blocks
		perPixel(bx1, by0, x0 + width - bx1, by1 - by0);           //right of the blocks
		// Where each unit of a block goes in 'pixels', from the block's top left pixel
		uint32 units = 256 / layout.unitBytes;
		uint32 index[128];
		for(uint32 u = 0; u < units; u++) index[u] = (layout.place[u] >> 8) * stride + (layout.place[u] & 0xFF);
		GS_SURFACE surface;
		surface.Init(psm, fbp, fbw);
		bool is16 = layout.unitBytes == 2;
		bool is24 = GsMemory::IsPsm24(psm);
		alignas(16) uint8 temp[256];
		for(uint32 by = by0; by < by1; by += layout.height)
		{
			uint32 rowBase = surface.RowBase(by);
			const uint32* rowTable = surface.RowTable(by);
			for(uint32 bx = bx0; bx < bx1; bx += layout.width)
			{
				uint8* block = ram + (surface.Offset(rowBase, rowTable, bx) & ~255u);
				uint32* p = pixels + (by - y0) * stride + (bx - x0);
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
				if(g_neonColumns && layout.columns32 && !is16)
				{
					if(g_prefetchBlocks != 0)
					{
						//The pixel rows of the block g_prefetchBlocks ahead (8 rows 'stride' apart: the
						//prefetcher does not follow 8 streams) and its GS block
						uint32 ahead = g_prefetchBlocks * layout.width;
						if(bx + ahead < bx1)
						{
							const uint32* q = p + ahead;
							if(write) for(uint32 r = 0; r < 8; r++) __builtin_prefetch(q + r * stride, 0);
							__builtin_prefetch(ram + (surface.Offset(rowBase, rowTable, bx + ahead) & ~255u), write ? 1 : 0);
						}
					}
					if(write)
						PixelsToBlock32(p, stride, block, is24);
					else
						BlockToPixels32(block, p, stride, is24);
					continue;
				}
#endif
				if(!write)
				{
					if(is16)
					{
						const uint16* src = reinterpret_cast<const uint16*>(block);
						for(uint32 u = 0; u < units; u++)
						{
							uint32 color = GsMemory::Color16To32(src[u]);
							p[index[u]] = (color & 0x00FFFFFF) | (static_cast<uint32>(GsAlphaToTarget(color >> 24)) << 24);
						}
					}
					else
					{
						BlockAlphaToTarget(block, temp, is24);
						if(layout.pairs)
						{
							for(uint32 u = 0; u < units; u += 2)
								std::memcpy(p + index[u], temp + u * 4, 8);
						}
						else
						{
							for(uint32 u = 0; u < units; u++)
								std::memcpy(p + index[u], temp + u * 4, 4);
						}
					}
				}
				else
				{
					if(is16)
					{
						uint16* dst = reinterpret_cast<uint16*>(block);
						for(uint32 u = 0; u < units; u++)
						{
							uint32 color = p[index[u]];
							uint32 gsColor = (color & 0x00FFFFFF) | (TargetAlphaToGs(color >> 24) << 24);
							dst[u] = static_cast<uint16>(GsMemory::Color32To16(gsColor));
						}
					}
					else
					{
						if(layout.pairs)
						{
							for(uint32 u = 0; u < units; u += 2)
								std::memcpy(temp + u * 4, p + index[u], 8);
						}
						else
						{
							for(uint32 u = 0; u < units; u++)
								std::memcpy(temp + u * 4, p + index[u], 4);
						}
						BlockAlphaToGs(temp, block, is24);
					}
				}
			}
		}
	}

	// Block versions (the per pixel ones for formats or rectangles they do not handle)
	inline void Read(const uint8* ram, uint32 psm, uint32 fbp, uint32 fbw, uint32 x0, uint32 y0, uint32 width, uint32 height, uint32* pixels)
	{
		BlockPass<false>(const_cast<uint8*>(ram), psm, fbp, fbw, x0, y0, width, height, pixels, width);
	}

	inline void Write(uint8* ram, uint32 psm, uint32 fbp, uint32 fbw, uint32 x0, uint32 y0, uint32 width, uint32 height, const uint32* pixels)
	{
		BlockPass<true>(ram, psm, fbp, fbw, x0, y0, width, height, const_cast<uint32*>(pixels), width);
	}
}

namespace TargetPixels
{
	// The block versions against the per pixel ones on random rectangles in a random 1 MB of
	// GS memory: rectangles that differ. For builds the tests do not run on (NEON, Vita).
	inline uint32 SelfTest(uint32 trials)
	{
		const uint32 psms[] = {CGSHandler::PSMCT32, CGSHandler::PSMCT24, CGSHandler::PSMCT16, CGSHandler::PSMCT16S};
		//Rectangles stay inside the first 1 MB (fbp below 512 KB, 8 pages a row, 140 rows):
		//filled once, compared and kept equal per trial (a few ms on the Vita)
		const uint32 used = 1u << 20;
		std::vector<uint8> ram(CGSHandler::RAMSIZE), ramB(CGSHandler::RAMSIZE);
		uint32 seed = 0x2468ACE1;
		auto next = [&seed]() { seed = seed * 1664525 + 1013904223; return seed >> 8; };
		for(uint32 i = 0; i < used; i++) ram[i] = static_cast<uint8>(next());
		std::memcpy(ramB.data(), ram.data(), used);
		uint32 failures = 0;
		for(uint32 trial = 0; trial < trials; trial++)
		{
			uint32 psm = psms[trial % 4];
			uint32 fbw = 1 + next() % 8;
			uint32 fbp = (next() % 64) * 8192;
			uint32 x0 = (trial & 4) ? 0 : next() % 40;
			uint32 y0 = (trial & 4) ? 0 : next() % 40;
			uint32 width = std::min<uint32>(1 + next() % 200, fbw * 64 - std::min(x0, fbw * 64 - 1));
			uint32 height = 1 + next() % 100;
			std::vector<uint32> a(width * height), b(width * height), pixels(width * height);
			Read(ram.data(), psm, fbp, fbw, x0, y0, width, height, a.data());
			ReadPixels(ram.data(), psm, fbp, fbw, x0, y0, width, height, b.data(), width);
			for(auto& v : pixels) v = next() ^ (next() << 24);
			Write(ram.data(), psm, fbp, fbw, x0, y0, width, height, pixels.data());
			WritePixels(ramB.data(), psm, fbp, fbw, x0, y0, width, height, pixels.data(), width);
			if((a != b) || (std::memcmp(ram.data(), ramB.data(), used) != 0))
			{
				failures++;
				std::memcpy(ramB.data(), ram.data(), used);
			}
		}
		return failures;
	}
}
