// Host to local transfer writes: the block path (GsTransferWrite::g_blockWrites) against the
// per pixel path, for PSMCT32, PSMCT16, PSMT8 and PSMT4, on random transfers (positions,
// sizes, buffer widths, initial memory), aligned to blocks and not. Memory and the "changed"
// result must match exactly. The block path without comparing (g_compare = false) must leave the
// same memory and report a change wherever the comparing path does.
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "gs/GsTransferWrite.h"

namespace
{
	const uint32 g_ramSize = 4 * 1024 * 1024;

	struct TRANSFER
	{
		uint32 pointer, bufferWidth, dsax, dsay, rrw, rrh;
	};

	template <typename Storage>
	bool RunUnits(std::vector<uint8>& ram, const TRANSFER& t, const std::vector<uint8>& data, bool blocks)
	{
		GsTransferWrite::g_blockWrites = blocks;
		uint32 rrx = 0, rry = 0;
		GsTransferWrite::POSITION position{rrx, rry, t.dsax, t.dsay, t.rrw};
		uint32 count = t.rrw * t.rrh;
		return GsTransferWrite::WriteUnits<Storage>(ram.data(), g_ramSize, t.pointer, t.bufferWidth, position,
		                                            reinterpret_cast<const typename Storage::Unit*>(data.data()), count);
	}

	bool RunPsmt4(std::vector<uint8>& ram, const TRANSFER& t, const std::vector<uint8>& data, bool blocks)
	{
		GsTransferWrite::g_blockWrites = blocks;
		uint32 rrx = 0, rry = 0;
		GsTransferWrite::POSITION position{rrx, rry, t.dsax, t.dsay, t.rrw};
		return GsTransferWrite::WritePsmt4(ram.data(), g_ramSize, t.pointer, t.bufferWidth, position, data.data(), t.rrw * t.rrh / 2);
	}

	template <typename Run>
	int Check(const char* name, uint32 blockWidth, uint32 blockHeight, uint32 bitsPerPixel, Run run, std::mt19937& rng)
	{
		int failures = 0;
		for(int trial = 0; trial < 400; trial++)
		{
			TRANSFER t;
			bool aligned = (trial % 3) != 0;
			t.bufferWidth = 1 + rng() % 16;
			t.pointer = (rng() % 256) * 8192;
			uint32 widthUnits = aligned ? blockWidth : 2;
			t.rrw = widthUnits * (1 + rng() % (aligned ? 8 : 64));
			t.rrh = (aligned ? blockHeight : 1) * (1 + rng() % (aligned ? 6 : 40));
			t.dsax = (aligned ? blockWidth : 1) * (rng() % (aligned ? 16 : 300));
			t.dsay = (aligned ? blockHeight : 1) * (rng() % (aligned ? 16 : 300));
			if(t.dsax + t.rrw > 2048) t.dsax = 0;
			size_t bytes = static_cast<size_t>(t.rrw) * t.rrh * bitsPerPixel / 8;
			std::vector<uint8> data(bytes);
			for(auto& b : data) b = static_cast<uint8>(rng());
			std::vector<uint8> a(g_ramSize), b(g_ramSize);
			for(uint32 i = 0; i < g_ramSize; i += 4)
			{
				uint32 v = rng();
				std::memcpy(&a[i], &v, 4);
			}
			b = a;
			std::vector<uint8> c = a;
			bool dirtyBlocks = run(a, t, data, true);
			bool dirtyPixels = run(b, t, data, false);
			//Store only (feed_compare=0): the same memory, and a change reported whenever there is one
			GsTransferWrite::g_compare = false;
			bool dirtyStore = run(c, t, data, true);
			GsTransferWrite::g_compare = true;
			if((a != c) || (dirtyBlocks && !dirtyStore))
			{
				if(failures < 5)
					std::printf("  FAIL %s store only: buffer width %u, at %u,%u size %ux%u (memory %s, dirty %d/%d)\n", name, t.bufferWidth, t.dsax, t.dsay, t.rrw, t.rrh,
					            (a == c) ? "same" : "differs", dirtyBlocks ? 1 : 0, dirtyStore ? 1 : 0);
				failures++;
			}
			if((a != b) || (dirtyBlocks != dirtyPixels))
			{
				if(failures < 5)
					std::printf("  FAIL %s: buffer width %u, at %u,%u size %ux%u, pointer %X (memory %s, dirty %d/%d)\n", name, t.bufferWidth, t.dsax, t.dsay, t.rrw, t.rrh,
					            t.pointer, (a == b) ? "same" : "differs", dirtyBlocks ? 1 : 0, dirtyPixels ? 1 : 0);
				failures++;
			}
		}
		std::printf("  %s %s: 400 transfers\n", failures ? "FAIL" : "ok  ", name);
		return failures;
	}
}

int main()
{
	std::mt19937 rng(1234);
	if(!GsTransferWrite::GetPsmt4BlockTables().valid || !GsTransferWrite::GetBlockChunks<CGsPixelFormats::STORAGEPSMCT32>().valid)
	{
		std::printf("FAIL: PSMT4 or PSMCT32 block tables not built (layout not as expected)\n");
		return 1;
	}
	int failures = 0;
	uint32 selfTest = GsTransferWrite::SelfTestPsmt4Blocks(2000);
	std::printf("  %s PSMT4 block self-test: %u of 2000 differ\n", selfTest ? "FAIL" : "ok  ", selfTest);
	failures += selfTest;
	failures += Check("PSMCT32", 8, 8, 32, RunUnits<CGsPixelFormats::STORAGEPSMCT32>, rng);
	//feed_neon: store only by columns (NEON on ARM, 8-byte copies elsewhere) against the other paths
	if(!GsTransferWrite::ColumnLayoutMatches<CGsPixelFormats::STORAGEPSMCT32>())
	{
		std::printf("  FAIL PSMCT32 column layout not as the column path assumes\n");
		failures++;
	}
	GsTransferWrite::g_columns = true;
	failures += Check("PSMCT32 columns", 8, 8, 32, RunUnits<CGsPixelFormats::STORAGEPSMCT32>, rng);
	GsTransferWrite::g_columns = false;
	failures += Check("PSMCT16", 16, 8, 16, RunUnits<CGsPixelFormats::STORAGEPSMCT16>, rng);
	failures += Check("PSMT8", 16, 16, 8, RunUnits<CGsPixelFormats::STORAGEPSMT8>, rng);
	failures += Check("PSMT4", 32, 16, 4, RunPsmt4, rng);
	failures += Check("PSMT4 pages", 128, 128, 4, RunPsmt4, rng);
	GsTransferWrite::g_blockWrites = true;
	std::printf("%s\n", failures ? "FAILED" : "all transfers match");
	return failures ? 1 : 0;
}
