#pragma once

// Pixel kernels of the native MPEG-2 decoder (Mpeg2Decoder.cpp), NEON on ARM
// (PS Vita) with plain C++ for other targets. Both give identical results
// (tests/mpeg2_simd_tests.cpp runs them against each other).

#include <cstdint>
#include <cstring>
#include <random>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace Mpeg2Simd
{
	// Cleared by SelfTest when the NEON kernels disagree with the scalar ones.
	inline bool g_neon = true;

	inline uint8_t Clamp8(int value)
	{
		return static_cast<uint8_t>((value < 0) ? 0 : ((value > 255) ? 255 : value));
	}

	// YCbCr -> RGBA32, the IPU's CSC command with both alpha thresholds at 0.
	// The IPU's 16.16 contributions are added to Y << 16, which has no fraction
	// bits: only their integer parts count, once R, G and B are clamped.
	struct CSC_OFFSETS
	{
		int16_t r[256], b[256];
		int32_t gCb[256], gCr[256]; //16.16: G adds both before dropping the fraction
		CSC_OFFSETS()
		{
			for(int i = 0; i < 256; i++)
			{
				int c = i - 128;
				r[i] = static_cast<int16_t>(static_cast<int32_t>(1.402 * 65536.0 * c) >> 16);
				b[i] = static_cast<int16_t>(static_cast<int32_t>(1.772 * 65536.0 * c) >> 16);
				gCb[i] = static_cast<int32_t>(-0.34414 * 65536.0 * c);
				gCr[i] = static_cast<int32_t>(-0.71414 * 65536.0 * c);
			}
		}
	};

	inline const CSC_OFFSETS& GetCscOffsets()
	{
		static const CSC_OFFSETS offsets;
		return offsets;
	}

	// One 16x16 macroblock, 'y' and the 8x8 chroma blocks with their strides, to
	// 256 RGBA pixels (bytes R, G, B, A, row by row).
	inline void ConvertMacroblockScalar(const uint8_t* y, uint32_t yStride, const uint8_t* cb, const uint8_t* cr, uint32_t cStride, uint8_t* out)
	{
		const auto& t = GetCscOffsets();
		for(unsigned int j = 0; j < 16; j++)
		{
			const uint8_t* cbRow = cb + (j / 2) * cStride;
			const uint8_t* crRow = cr + (j / 2) * cStride;
			for(unsigned int i = 0; i < 16; i++, out += 4)
			{
				int value = y[i];
				uint8_t u = cbRow[i / 2], v = crRow[i / 2];
				out[0] = Clamp8(value + t.r[v]);
				out[1] = Clamp8(value + ((t.gCb[u] + t.gCr[v]) >> 16));
				out[2] = Clamp8(value + t.b[u]);
				out[3] = 0x80;
			}
			y += yStride;
		}
	}

	inline void ConvertMacroblock(const uint8_t* y, uint32_t yStride, const uint8_t* cb, const uint8_t* cr, uint32_t cStride, uint8_t* out)
	{
#if defined(__ARM_NEON)
		if(!g_neon) return ConvertMacroblockScalar(y, yStride, cb, cr, cStride, out);
		const auto& t = GetCscOffsets();
		for(unsigned int j = 0; j < 8; j++)
		{
			int16_t rOffset[8], gOffset[8], bOffset[8];
			for(unsigned int i = 0; i < 8; i++)
			{
				uint8_t u = cb[i], v = cr[i];
				rOffset[i] = t.r[v];
				gOffset[i] = static_cast<int16_t>((t.gCb[u] + t.gCr[v]) >> 16);
				bOffset[i] = t.b[u];
			}
			int16x8x2_t r = vzipq_s16(vld1q_s16(rOffset), vld1q_s16(rOffset));
			int16x8x2_t g = vzipq_s16(vld1q_s16(gOffset), vld1q_s16(gOffset));
			int16x8x2_t b = vzipq_s16(vld1q_s16(bOffset), vld1q_s16(bOffset));
			for(unsigned int row = 0; row < 2; row++, y += yStride, out += 64)
			{
				uint8x16_t luma = vld1q_u8(y);
				int16x8_t low = vreinterpretq_s16_u16(vmovl_u8(vget_low_u8(luma)));
				int16x8_t high = vreinterpretq_s16_u16(vmovl_u8(vget_high_u8(luma)));
				uint8x16x4_t pixels;
				pixels.val[0] = vcombine_u8(vqmovun_s16(vaddq_s16(low, r.val[0])), vqmovun_s16(vaddq_s16(high, r.val[1])));
				pixels.val[1] = vcombine_u8(vqmovun_s16(vaddq_s16(low, g.val[0])), vqmovun_s16(vaddq_s16(high, g.val[1])));
				pixels.val[2] = vcombine_u8(vqmovun_s16(vaddq_s16(low, b.val[0])), vqmovun_s16(vaddq_s16(high, b.val[1])));
				pixels.val[3] = vdupq_n_u8(0x80);
				vst4q_u8(out, pixels);
			}
			cb += cStride;
			cr += cStride;
		}
#else
		ConvertMacroblockScalar(y, yStride, cb, cr, cStride, out);
#endif
	}

	// 8x8 IDCT output to the picture: intra blocks replace, others add (both
	// saturated to 0-255).
	inline void AddBlockScalar(uint8_t* base, uint32_t stride, const int16_t* block, bool intra)
	{
		for(unsigned int j = 0; j < 8; j++)
		{
			uint8_t* row = base + j * stride;
			for(unsigned int i = 0; i < 8; i++)
			{
				int value = block[j * 8 + i];
				row[i] = Clamp8(intra ? value : (row[i] + value));
			}
		}
	}

	inline void AddBlock(uint8_t* base, uint32_t stride, const int16_t* block, bool intra)
	{
#if defined(__ARM_NEON)
		if(!g_neon) return AddBlockScalar(base, stride, block, intra);
		for(unsigned int j = 0; j < 8; j++)
		{
			uint8_t* row = base + j * stride;
			int16x8_t value = vld1q_s16(block + j * 8);
			if(!intra) value = vaddq_s16(value, vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row))));
			vst1_u8(row, vqmovun_s16(value));
		}
#else
		AddBlockScalar(base, stride, block, intra);
#endif
	}

	// One row of a half-pel prediction, width 8 or 16: s is the reference row,
	// s1 the next one (for vertical half-pels); s[width] and s1[width] are
	// readable. average: (row + prediction + 1) / 2 (bidirectional).
	inline void PredictRowScalar(uint8_t* row, const uint8_t* s, const uint8_t* s1, uint32_t width, bool hx, bool hy, bool average)
	{
		uint8_t value[16];
		for(uint32_t i = 0; i < width; i++)
		{
			if(!hx && !hy) value[i] = s[i];
			else if(hx && !hy) value[i] = static_cast<uint8_t>((s[i] + s[i + 1] + 1) >> 1);
			else if(!hx && hy) value[i] = static_cast<uint8_t>((s[i] + s1[i] + 1) >> 1);
			else value[i] = static_cast<uint8_t>((s[i] + s[i + 1] + s1[i] + s1[i + 1] + 2) >> 2);
		}
		if(average)
		{
			for(uint32_t i = 0; i < width; i++) row[i] = static_cast<uint8_t>((row[i] + value[i] + 1) >> 1);
		}
		else
		{
			std::memcpy(row, value, width);
		}
	}

#if defined(__ARM_NEON)
	inline uint8x8_t Predict8(const uint8_t* s, const uint8_t* s1, bool hx, bool hy)
	{
		uint8x8_t a = vld1_u8(s);
		if(!hx && !hy) return a;
		if(hx && !hy) return vrhadd_u8(a, vld1_u8(s + 1));
		if(!hx) return vrhadd_u8(a, vld1_u8(s1));
		uint16x8_t sum = vaddq_u16(vaddl_u8(a, vld1_u8(s + 1)), vaddl_u8(vld1_u8(s1), vld1_u8(s1 + 1)));
		return vrshrn_n_u16(sum, 2);
	}
#endif

	inline void PredictRow(uint8_t* row, const uint8_t* s, const uint8_t* s1, uint32_t width, bool hx, bool hy, bool average)
	{
#if defined(__ARM_NEON)
		if(!g_neon) return PredictRowScalar(row, s, s1, width, hx, hy, average);
		for(uint32_t i = 0; i < width; i += 8)
		{
			uint8x8_t value = Predict8(s + i, s1 + i, hx, hy);
			if(average) value = vrhadd_u8(value, vld1_u8(row + i));
			vst1_u8(row + i, value);
		}
#else
		PredictRowScalar(row, s, s1, width, hx, hy, average);
#endif
	}
}

namespace Mpeg2Simd
{
	// Runs the kernels against the scalar versions on random data; returns the
	// number of mismatches and turns the NEON kernels off if there are any.
	inline unsigned int SelfTest(unsigned int iterations = 4000)
	{
		std::mt19937 random(1234);
		unsigned int failures = 0;
		for(unsigned int test = 0; test < iterations; test++)
		{
			uint8_t picture[8 * 24], expected[8 * 24];
			int16_t block[64];
			for(auto& value : picture) value = static_cast<uint8_t>(random());
			// Mostly small residuals, some saturating ones.
			for(auto& value : block) value = static_cast<int16_t>((random() % 8 == 0) ? (static_cast<int>(random() % 1024) - 512) : (static_cast<int>(random() % 64) - 32));
			bool intra = (random() & 1) != 0;
			std::memcpy(expected, picture, sizeof(picture));
			AddBlockScalar(expected, 24, block, intra);
			AddBlock(picture, 24, block, intra);
			if(std::memcmp(expected, picture, sizeof(picture)) != 0) failures++;
		}
		for(unsigned int test = 0; test < iterations; test++)
		{
			uint8_t source[2][17], row[16], expected[16];
			for(auto& line : source)
			{
				for(auto& value : line) value = static_cast<uint8_t>(random());
			}
			for(auto& value : row) value = static_cast<uint8_t>(random());
			std::memcpy(expected, row, sizeof(row));
			uint32_t width = (random() & 1) ? 16 : 8;
			bool hx = (random() & 1) != 0, hy = (random() & 1) != 0, average = (random() & 1) != 0;
			PredictRowScalar(expected, source[0], source[1], width, hx, hy, average);
			PredictRow(row, source[0], source[1], width, hx, hy, average);
			if(std::memcmp(expected, row, sizeof(row)) != 0) failures++;
		}
		for(unsigned int test = 0; test < iterations / 20; test++)
		{
			uint8_t luma[16 * 16], cb[8 * 8], cr[8 * 8], pixels[256 * 4], expected[256 * 4];
			for(auto& value : luma) value = static_cast<uint8_t>(random());
			for(auto& value : cb) value = static_cast<uint8_t>(random());
			for(auto& value : cr) value = static_cast<uint8_t>(random());
			ConvertMacroblockScalar(luma, 16, cb, cr, 8, expected);
			ConvertMacroblock(luma, 16, cb, cr, 8, pixels);
			if(std::memcmp(expected, pixels, sizeof(pixels)) != 0) failures++;
		}
		if(failures != 0) g_neon = false;
		return failures;
	}
}
