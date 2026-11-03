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
		if(failures != 0) g_neon = false;
		return failures;
	}
}
