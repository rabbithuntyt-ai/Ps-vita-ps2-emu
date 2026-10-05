// The IPU's IDCT (Framework's IDCT::CIEEE1180, patched to a fast integer
// implementation) must meet the IEEE 1180-1990 accuracy requirements, checked
// against a double precision reference, and be fast.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <initializer_list>
#include "idct/IEEE1180.h"

namespace
{
	double g_c[8][8];

	void InitTables()
	{
		for(int freq = 0; freq < 8; freq++)
		{
			double scale = (freq == 0) ? std::sqrt(0.125) : 0.5;
			for(int time = 0; time < 8; time++) g_c[freq][time] = scale * std::cos((M_PI / 8.0) * freq * (time + 0.5));
		}
	}

	void ForwardDct(const int* in, int16* out)
	{
		double tmp[64];
		for(int i = 0; i < 8; i++)
			for(int j = 0; j < 8; j++)
			{
				double sum = 0;
				for(int k = 0; k < 8; k++) sum += g_c[j][k] * in[8 * i + k];
				tmp[8 * i + j] = sum;
			}
		for(int j = 0; j < 8; j++)
			for(int i = 0; i < 8; i++)
			{
				double sum = 0;
				for(int k = 0; k < 8; k++) sum += g_c[i][k] * tmp[8 * k + j];
				int v = static_cast<int>(std::floor(sum + 0.5));
				out[8 * i + j] = static_cast<int16>(std::min(std::max(v, -2048), 2047));
			}
	}

	void ReferenceIdct(const int16* in, int* out)
	{
		double tmp[64];
		for(int i = 0; i < 8; i++)
			for(int j = 0; j < 8; j++)
			{
				double sum = 0;
				for(int k = 0; k < 8; k++) sum += g_c[k][j] * in[8 * i + k];
				tmp[8 * i + j] = sum;
			}
		for(int j = 0; j < 8; j++)
			for(int i = 0; i < 8; i++)
			{
				double sum = 0;
				for(int k = 0; k < 8; k++) sum += g_c[k][i] * tmp[8 * k + j];
				int v = static_cast<int>(std::floor(sum + 0.5));
				out[8 * i + j] = std::min(std::max(v, -256), 255);
			}
	}

	// IEEE 1180 random generator.
	long g_randx = 1;
	long IeeeRand(long L, long H)
	{
		static const double z = static_cast<double>(0x7fffffff);
		g_randx = (g_randx * 1103515245) + 12345;
		long i = g_randx & 0x7ffffffe;
		double x = static_cast<double>(i) / z;
		x *= (L + H + 1);
		long j = static_cast<long>(x);
		return j - L;
	}

	bool RunCase(long L, long H, int sign)
	{
		const int blocks = 10000;
		g_randx = 1;
		double sumErr[64] = {}, sumSq[64] = {};
		int peak = 0;
		auto idct = IDCT::CIEEE1180::GetInstance();
		for(int b = 0; b < blocks; b++)
		{
			int pixels[64];
			for(int i = 0; i < 64; i++) pixels[i] = static_cast<int>(IeeeRand(L, H)) * sign;
			int16 coeffs[64];
			ForwardDct(pixels, coeffs);
			int reference[64];
			ReferenceIdct(coeffs, reference);
			int16 result[64];
			idct->Transform(coeffs, result);
			for(int i = 0; i < 64; i++)
			{
				int err = result[i] - reference[i];
				peak = std::max(peak, std::abs(err));
				sumErr[i] += err;
				sumSq[i] += err * err;
			}
		}
		double worstMse = 0, worstMean = 0, totalMse = 0, totalMean = 0;
		for(int i = 0; i < 64; i++)
		{
			worstMse = std::max(worstMse, sumSq[i] / blocks);
			worstMean = std::max(worstMean, std::fabs(sumErr[i] / blocks));
			totalMse += sumSq[i];
			totalMean += sumErr[i];
		}
		totalMse /= 64.0 * blocks;
		totalMean = std::fabs(totalMean / (64.0 * blocks));
		bool ok = (peak <= 1) && (worstMse <= 0.06) && (totalMse <= 0.02) && (worstMean <= 0.015) && (totalMean <= 0.0015);
		std::printf("  %s L=%ld H=%ld sign=%+d: peak %d, mse %.4f (max %.4f), mean %.5f (max %.4f)\n", ok ? "ok  " : "FAIL",
		            L, H, sign, peak, totalMse, worstMse, totalMean, worstMean);
		return ok;
	}
}

int main()
{
	InitTables();
	bool ok = true;
	for(int sign : {1, -1})
	{
		ok &= RunCase(256, 255, sign);
		ok &= RunCase(5, 5, sign);
		ok &= RunCase(300, 300, sign);
	}

	// All-zero input must give all-zero output (IEEE 1180 requirement).
	int16 zero[64] = {}, out[64];
	IDCT::CIEEE1180::GetInstance()->Transform(zero, out);
	for(int16 v : out) ok &= (v == 0);

	// Speed (informational).
	int16 coeffs[64];
	int pixels[64];
	g_randx = 7;
	for(int i = 0; i < 64; i++) pixels[i] = static_cast<int>(IeeeRand(256, 255));
	ForwardDct(pixels, coeffs);
	const int iterations = 200000;
	auto start = std::chrono::steady_clock::now();
	for(int i = 0; i < iterations; i++)
	{
		coeffs[i & 63] ^= 1;
		IDCT::CIEEE1180::GetInstance()->Transform(coeffs, out);
	}
	double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count() / iterations;
	std::printf("fast IDCT: %.0f ns/block (%d)\n", ns, out[0]);

	std::printf("%s\n", ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}
