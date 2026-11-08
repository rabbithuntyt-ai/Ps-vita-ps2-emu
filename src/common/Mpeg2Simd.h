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
	// 8x8 inverse DCT: Chen-Wang in integers, exactly Framework's
	// IDCT::CIEEE1180::Transform (mpeg2decode's idct.c). The zero-row shortcuts
	// there give the same values as the full formula, so none are needed here.
	namespace IdctConstants
	{
		enum
		{
			W1 = 2841,
			W2 = 2676,
			W3 = 2408,
			W5 = 1609,
			W6 = 1108,
			W7 = 565,
		};
	}

	inline void IdctScalar(const int16_t* in, int16_t* out)
	{
		using namespace IdctConstants;
		int b[64];
		for(int i = 0; i < 64; i++) b[i] = in[i];
		for(int r = 0; r < 8; r++)
		{
			int* blk = b + 8 * r;
			int x0 = (blk[0] * 2048) + 128, x1 = blk[4] * 2048, x2 = blk[6], x3 = blk[2], x4 = blk[1], x5 = blk[7], x6 = blk[5], x7 = blk[3], x8;
			x8 = W7 * (x4 + x5);
			x4 = x8 + (W1 - W7) * x4;
			x5 = x8 - (W1 + W7) * x5;
			x8 = W3 * (x6 + x7);
			x6 = x8 - (W3 - W5) * x6;
			x7 = x8 - (W3 + W5) * x7;
			x8 = x0 + x1;
			x0 -= x1;
			x1 = W6 * (x3 + x2);
			x2 = x1 - (W2 + W6) * x2;
			x3 = x1 + (W2 - W6) * x3;
			x1 = x4 + x6;
			x4 -= x6;
			x6 = x5 + x7;
			x5 -= x7;
			x7 = x8 + x3;
			x8 -= x3;
			x3 = x0 + x2;
			x0 -= x2;
			x2 = (181 * (x4 + x5) + 128) >> 8;
			x4 = (181 * (x4 - x5) + 128) >> 8;
			blk[0] = (x7 + x1) >> 8;
			blk[1] = (x3 + x2) >> 8;
			blk[2] = (x0 + x4) >> 8;
			blk[3] = (x8 + x6) >> 8;
			blk[4] = (x8 - x6) >> 8;
			blk[5] = (x0 - x4) >> 8;
			blk[6] = (x3 - x2) >> 8;
			blk[7] = (x7 - x1) >> 8;
		}
		auto clip = [](int value) { return static_cast<int16_t>((value < -256) ? -256 : ((value > 255) ? 255 : value)); };
		for(int c = 0; c < 8; c++)
		{
			const int* blk = b + c;
			int x0 = (blk[0] * 256) + 8192, x1 = blk[32] * 256, x2 = blk[48], x3 = blk[16], x4 = blk[8], x5 = blk[56], x6 = blk[40], x7 = blk[24], x8;
			x8 = W7 * (x4 + x5) + 4;
			x4 = (x8 + (W1 - W7) * x4) >> 3;
			x5 = (x8 - (W1 + W7) * x5) >> 3;
			x8 = W3 * (x6 + x7) + 4;
			x6 = (x8 - (W3 - W5) * x6) >> 3;
			x7 = (x8 - (W3 + W5) * x7) >> 3;
			x8 = x0 + x1;
			x0 -= x1;
			x1 = W6 * (x3 + x2) + 4;
			x2 = (x1 - (W2 + W6) * x2) >> 3;
			x3 = (x1 + (W2 - W6) * x3) >> 3;
			x1 = x4 + x6;
			x4 -= x6;
			x6 = x5 + x7;
			x5 -= x7;
			x7 = x8 + x3;
			x8 -= x3;
			x3 = x0 + x2;
			x0 -= x2;
			x2 = (181 * (x4 + x5) + 128) >> 8;
			x4 = (181 * (x4 - x5) + 128) >> 8;
			out[c + 0] = clip((x7 + x1) >> 14);
			out[c + 8] = clip((x3 + x2) >> 14);
			out[c + 16] = clip((x0 + x4) >> 14);
			out[c + 24] = clip((x8 + x6) >> 14);
			out[c + 32] = clip((x8 - x6) >> 14);
			out[c + 40] = clip((x0 - x4) >> 14);
			out[c + 48] = clip((x3 - x2) >> 14);
			out[c + 56] = clip((x7 - x1) >> 14);
		}
	}

#if defined(__ARM_NEON)
	inline void Transpose4(int32x4_t& a, int32x4_t& b, int32x4_t& c, int32x4_t& d)
	{
		int32x4x2_t ab = vtrnq_s32(a, b), cd = vtrnq_s32(c, d);
		a = vcombine_s32(vget_low_s32(ab.val[0]), vget_low_s32(cd.val[0]));
		b = vcombine_s32(vget_low_s32(ab.val[1]), vget_low_s32(cd.val[1]));
		c = vcombine_s32(vget_high_s32(ab.val[0]), vget_high_s32(cd.val[0]));
		d = vcombine_s32(vget_high_s32(ab.val[1]), vget_high_s32(cd.val[1]));
	}

	// One pass over 4 lanes: v[k] is coefficient k of each lane's row (or column).
	// rowPass: the first pass's rounding; otherwise the second's.
	template <bool rowPass>
	inline void IdctPass(int32x4_t* v)
	{
		using namespace IdctConstants;
		int32x4_t x0, x1, x2 = v[6], x3 = v[2], x4 = v[1], x5 = v[7], x6 = v[5], x7 = v[3], x8;
		if(rowPass)
		{
			x0 = vaddq_s32(vshlq_n_s32(v[0], 11), vdupq_n_s32(128));
			x1 = vshlq_n_s32(v[4], 11);
			x8 = vmulq_n_s32(vaddq_s32(x4, x5), W7);
			x4 = vmlaq_n_s32(x8, x4, W1 - W7);
			x5 = vmlsq_n_s32(x8, x5, W1 + W7);
			x8 = vmulq_n_s32(vaddq_s32(x6, x7), W3);
			x6 = vmlsq_n_s32(x8, x6, W3 - W5);
			x7 = vmlsq_n_s32(x8, x7, W3 + W5);
		}
		else
		{
			x0 = vaddq_s32(vshlq_n_s32(v[0], 8), vdupq_n_s32(8192));
			x1 = vshlq_n_s32(v[4], 8);
			x8 = vmlaq_n_s32(vdupq_n_s32(4), vaddq_s32(x4, x5), W7);
			x4 = vshrq_n_s32(vmlaq_n_s32(x8, x4, W1 - W7), 3);
			x5 = vshrq_n_s32(vmlsq_n_s32(x8, x5, W1 + W7), 3);
			x8 = vmlaq_n_s32(vdupq_n_s32(4), vaddq_s32(x6, x7), W3);
			x6 = vshrq_n_s32(vmlsq_n_s32(x8, x6, W3 - W5), 3);
			x7 = vshrq_n_s32(vmlsq_n_s32(x8, x7, W3 + W5), 3);
		}
		x8 = vaddq_s32(x0, x1);
		x0 = vsubq_s32(x0, x1);
		if(rowPass)
		{
			x1 = vmulq_n_s32(vaddq_s32(x3, x2), W6);
			x2 = vmlsq_n_s32(x1, x2, W2 + W6);
			x3 = vmlaq_n_s32(x1, x3, W2 - W6);
		}
		else
		{
			x1 = vmlaq_n_s32(vdupq_n_s32(4), vaddq_s32(x3, x2), W6);
			x2 = vshrq_n_s32(vmlsq_n_s32(x1, x2, W2 + W6), 3);
			x3 = vshrq_n_s32(vmlaq_n_s32(x1, x3, W2 - W6), 3);
		}
		x1 = vaddq_s32(x4, x6);
		x4 = vsubq_s32(x4, x6);
		x6 = vaddq_s32(x5, x7);
		x5 = vsubq_s32(x5, x7);
		x7 = vaddq_s32(x8, x3);
		x8 = vsubq_s32(x8, x3);
		x3 = vaddq_s32(x0, x2);
		x0 = vsubq_s32(x0, x2);
		const int32x4_t round = vdupq_n_s32(128);
		x2 = vshrq_n_s32(vmlaq_n_s32(round, vaddq_s32(x4, x5), 181), 8);
		x4 = vshrq_n_s32(vmlaq_n_s32(round, vsubq_s32(x4, x5), 181), 8);
		int32x4_t r[8] = {vaddq_s32(x7, x1), vaddq_s32(x3, x2), vaddq_s32(x0, x4), vaddq_s32(x8, x6),
		                  vsubq_s32(x8, x6), vsubq_s32(x0, x4), vsubq_s32(x3, x2), vsubq_s32(x7, x1)};
		for(int k = 0; k < 8; k++) v[k] = rowPass ? vshrq_n_s32(r[k], 8) : vshrq_n_s32(r[k], 14);
	}
#endif

	inline void Idct(const int16_t* in, int16_t* out)
	{
#if defined(__ARM_NEON)
		if(!g_neon) return IdctScalar(in, out);
		// a: rows 0-3, b: rows 4-7; a[k] holds coefficient k of each row.
		int32x4_t a[8], b[8];
		for(int k = 0; k < 2; k++)
		{
			for(int r = 0; r < 4; r++)
			{
				a[k * 4 + r] = vmovl_s16(vld1_s16(in + 8 * r + 4 * k));
				b[k * 4 + r] = vmovl_s16(vld1_s16(in + 8 * (r + 4) + 4 * k));
			}
			Transpose4(a[k * 4], a[k * 4 + 1], a[k * 4 + 2], a[k * 4 + 3]);
			Transpose4(b[k * 4], b[k * 4 + 1], b[k * 4 + 2], b[k * 4 + 3]);
		}
		IdctPass<true>(a);
		IdctPass<true>(b);
		// Back to rows: lo[r] columns 0-3 of row r, hi[r] columns 4-7.
		int32x4_t lo[8] = {a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]};
		int32x4_t hi[8] = {a[4], a[5], a[6], a[7], b[4], b[5], b[6], b[7]};
		Transpose4(lo[0], lo[1], lo[2], lo[3]);
		Transpose4(lo[4], lo[5], lo[6], lo[7]);
		Transpose4(hi[0], hi[1], hi[2], hi[3]);
		Transpose4(hi[4], hi[5], hi[6], hi[7]);
		IdctPass<false>(lo);
		IdctPass<false>(hi);
		const int32x4_t low = vdupq_n_s32(-256), high = vdupq_n_s32(255);
		for(int r = 0; r < 8; r++)
		{
			int16x4_t left = vmovn_s32(vminq_s32(vmaxq_s32(lo[r], low), high));
			int16x4_t right = vmovn_s32(vminq_s32(vmaxq_s32(hi[r], low), high));
			vst1q_s16(out + 8 * r, vcombine_s16(left, right));
		}
#else
		IdctScalar(in, out);
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
		for(unsigned int test = 0; test < iterations; test++)
		{
			int16_t block[64] = {}, expected[64], result[64];
			// Sparse like real blocks, sometimes full, the whole range of values.
			unsigned int count = (random() % 8 == 0) ? 64 : (1 + random() % 12);
			for(unsigned int i = 0; i < count; i++) block[random() % 64] = static_cast<int16_t>(static_cast<int>(random() % 4096) - 2048);
			IdctScalar(block, expected);
			Idct(block, result);
			if(std::memcmp(expected, result, sizeof(result)) != 0) failures++;
		}
		if(failures != 0) g_neon = false;
		return failures;
	}
}
