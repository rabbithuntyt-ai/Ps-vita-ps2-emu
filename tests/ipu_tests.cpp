// IPU color space conversion (CSC): the integer implementation must match the
// original floating point one (channels within 1, identical alpha classes
// except where a channel straddles a threshold), and be fast.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include "ee/IPU.h"

namespace
{
	// Play!'s original per-pixel conversion.
	uint32 ReferencePixel(uint8 y, uint8 cb, uint8 cr, uint16 th0, uint16 th1)
	{
		float nY = y, nCb = cb, nCr = cr;
		float nR = std::clamp(nY + 1.402f * (nCr - 128), 0.f, 255.f);
		float nG = std::clamp(nY - 0.34414f * (nCb - 128) - 0.71414f * (nCr - 128), 0.f, 255.f);
		float nB = std::clamp(nY + 1.772f * (nCb - 128), 0.f, 255.f);
		uint8 r = static_cast<uint8>(nR), g = static_cast<uint8>(nG), b = static_cast<uint8>(nB);
		uint16 a0 = th0 & 0x1FF, a1 = th1 & 0x1FF;
		uint8 a = 0x80;
		if(r < a0 && g < a0 && b < a0) a = 0;
		else if(r < a1 && g < a1 && b < a1) a = 0x40;
		return (a << 24) | (b << 16) | (g << 8) | r;
	}

	bool Close(uint32 a, uint32 b)
	{
		for(int shift = 0; shift < 24; shift += 8)
		{
			int d = static_cast<int>((a >> shift) & 0xFF) - static_cast<int>((b >> shift) & 0xFF);
			if(std::abs(d) > 1) return false;
		}
		return true;
	}
}

int main()
{
	uint64 channelMismatches = 0, alphaMismatches = 0, pixels = 0;
	uint8 block[0x180];
	uint32 result[0x100];

	// Every (Y, Cb, Cr) combination: one macroblock per (Cb, Cr) pair holds
	// all 256 Y values with constant chroma.
	for(int i = 0; i < 0x100; i++) block[i] = static_cast<uint8>(i);
	for(int cb = 0; cb < 256; cb++)
	{
		for(int cr = 0; cr < 256; cr++)
		{
			std::fill(block + 0x100, block + 0x140, static_cast<uint8>(cb));
			std::fill(block + 0x140, block + 0x180, static_cast<uint8>(cr));
			CIPU::ConvertMacroblockToRgba32(block, result, 0, 0);
			for(int p = 0; p < 0x100; p++)
			{
				uint32 expected = ReferencePixel(block[p], cb, cr, 0, 0);
				pixels++;
				if(!Close(expected, result[p])) channelMismatches++;
			}
		}
	}

	// Random macroblocks with alpha thresholds: chroma subsampling layout and
	// alpha classes. Alpha may only differ when a channel is within 1 of a
	// threshold (both implementations round differently).
	std::mt19937 rng(1234);
	for(int n = 0; n < 20000; n++)
	{
		for(auto& v : block) v = static_cast<uint8>(rng());
		uint16 th0 = rng() % 0x100, th1 = th0 + rng() % 0x100;
		CIPU::ConvertMacroblockToRgba32(block, result, th0, th1);
		for(int y = 0; y < 16; y++)
			for(int x = 0; x < 16; x++)
			{
				int c = (y / 2) * 8 + x / 2;
				uint32 expected = ReferencePixel(block[y * 16 + x], block[0x100 + c], block[0x140 + c], th0, th1);
				uint32 actual = result[y * 16 + x];
				pixels++;
				if(!Close(expected, actual))
				{
					channelMismatches++;
				}
				else if((expected >> 24) != (actual >> 24))
				{
					bool nearThreshold = false;
					for(int shift = 0; shift < 24; shift += 8)
					{
						int e = (expected >> shift) & 0xFF;
						if(std::abs(e - (th0 & 0x1FF)) <= 1 || std::abs(e - (th1 & 0x1FF)) <= 1) nearThreshold = true;
					}
					if(!nearThreshold) alphaMismatches++;
				}
			}
	}

	// Speed (informational): 640x448 is 1120 macroblocks.
	auto start = std::chrono::steady_clock::now();
	for(int n = 0; n < 1120 * 30; n++)
	{
		block[n & 0xFF] ^= 1;
		CIPU::ConvertMacroblockToRgba32(block, result, 0, 0x100);
	}
	double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	std::printf("CSC: %.2f ms for 30 frames of 640x448 (%u)\n", ms, result[0]);

	std::printf("%llu pixels: %llu channel mismatches (>1), %llu alpha mismatches\n", (unsigned long long)pixels,
	            (unsigned long long)channelMismatches, (unsigned long long)alphaMismatches);
	bool ok = (channelMismatches == 0) && (alphaMismatches == 0);
	std::printf("%s\n", ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}
