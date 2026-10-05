// Multi-threaded rendering must be bit-identical to single-threaded rendering.
// Random register streams (state changes, textures, render-to-texture, CLUT
// loads, transfers) are fed to a 1-thread and a 3-thread GS; memory must match.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include "EmuSession.h"
#include "TestGs.h"

using namespace GsTest;

namespace
{
	std::mt19937 g_rng(7);

	uint32 Rand(uint32 n)
	{
		return g_rng() % n;
	}

	struct WRITE
	{
		uint8 reg;
		uint64 value;
	};

	std::vector<WRITE> MakeStream()
	{
		std::vector<WRITE> writes;
		auto w = [&](uint8 reg, uint64 value) { writes.push_back({reg, value}); };
		int batches = std::getenv("BATCHES") ? std::atoi(std::getenv("BATCHES")) : 60;
		for(int batch = 0; batch < batches; batch++)
		{
			//Render either to the main frame (page 0) or to a texture area (page 300)
			bool toTexture = Rand(4) == 0;
			w(GS_REG_FRAME_1, Frame(toTexture ? 300 : 0, Rand(4) == 0 ? CGSHandler::PSMCT16 : CGSHandler::PSMCT32));
			w(GS_REG_ZBUF_1, 150 | (static_cast<uint64>(Rand(2)) << 32));
			w(GS_REG_TEST_1, Rand(2) ? ((1ULL << 16) | (static_cast<uint64>(2 + Rand(2)) << 17)) : 0);
			w(GS_REG_ALPHA_1, Rand(3) | (Rand(3) << 2) | (Rand(3) << 4) | (Rand(3) << 6) | (static_cast<uint64>(Rand(256)) << 32));
			//Texture: sometimes the render target area itself (render-to-texture)
			uint64 tbp = Rand(2) ? (300 * 8192 / 256) : (0x300000 / 256);
			uint64 psm = Rand(2) ? CGSHandler::PSMCT32 : CGSHandler::PSMT8;
			uint64 tex0 = tbp | (10ULL << 14) | (psm << 20) | (7ULL << 26) | (7ULL << 30) | (1ULL << 34) |
			              (static_cast<uint64>(Rand(4)) << 35) | (static_cast<uint64>(0x3C0000 / 256) << 37) |
			              (static_cast<uint64>(Rand(2)) << 61);
			w(GS_REG_TEX0_1, tex0);
			w(GS_REG_TEX1_1, Rand(2) ? (1ULL << 5) : 0);

			uint32 primType = Rand(3) == 0 ? CGSHandler::PRIM_SPRITE : CGSHandler::PRIM_TRIANGLESTRIP;
			uint64 prim = primType | (Rand(2) << 3) | (Rand(2) << 4) | (Rand(2) << 6) | (Rand(2) << 8);
			w(GS_REG_PRIM, prim);
			for(int v = 0; v < 12; v++)
			{
				w(GS_REG_RGBAQ, Rgbaq(Rand(256), Rand(256), Rand(256), Rand(256)));
				w(GS_REG_UV, Uv(Rand(128 * 16), Rand(128 * 16)));
				uint32 st0, st1;
				float s = Rand(1000) / 800.0f, t = Rand(1000) / 800.0f;
				std::memcpy(&st0, &s, 4);
				std::memcpy(&st1, &t, 4);
				w(GS_REG_ST, st0 | (static_cast<uint64>(st1) << 32));
				w(GS_REG_XYZ2, Xyz(OffX(Rand(400)) + Rand(16), OffX(Rand(300)) + Rand(16), g_rng() & 0xFFFFFF));
			}
		}
		return writes;
	}

	void Seed(CTestGs& gs, uint32 seed)
	{
		std::mt19937 rng(seed);
		for(uint32 i = 0; i < CGSHandler::RAMSIZE; i++) gs.Ram()[i] = static_cast<uint8>(rng());
	}
}

int main()
{
	CEmuSession::SetDataPaths("vitaps2_test_data", ".");
	int failures = 0;
	for(int round = 0; round < 8; round++)
	{
		auto stream = MakeStream();
		if(std::getenv("ROUND") && std::atoi(std::getenv("ROUND")) != round) continue;
		uint32 threads = std::getenv("PAR_THREADS") ? std::atoi(std::getenv("PAR_THREADS")) : 3;
		CTestGs single(1), parallel(threads);
		Seed(single, round);
		Seed(parallel, round);
		for(auto* gs : {&single, &parallel})
		{
			SetupContext(*gs);
			gs->Write(GS_REG_XYOFFSET_1, (2048ULL << 4) | ((2048ULL << 4) << 32));
			gs->Write(GS_REG_SCISSOR_1, Scissor(0, 511, 0, 383));
			for(const auto& write : stream) gs->Write(write.reg, write.value);
			gs->Sync();
		}
		if(std::memcmp(single.Ram(), parallel.Ram(), CGSHandler::RAMSIZE) != 0)
		{
			if(std::getenv("DUMP"))
			{
				for(const auto& write : stream)
					if(write.reg != GS_REG_RGBAQ && write.reg != GS_REG_UV && write.reg != GS_REG_ST && write.reg != GS_REG_XYZ2)
						std::printf("  reg %02X = %016llX\n", write.reg, (unsigned long long)write.value);
				uint32 first = ~0u, last = 0;
				for(uint32 i = 0; i < CGSHandler::RAMSIZE; i++)
					if(single.Ram()[i] != parallel.Ram()[i]) { first = std::min(first, i); last = i; }
				std::printf("  diff range %06X-%06X (pages %u-%u)\n", first, last, first >> 13, last >> 13);
				{
					int n = 0;
					for(uint32 i = 0; i < CGSHandler::RAMSIZE && n < 16; i++)
						if(single.Ram()[i] != parallel.Ram()[i]) { std::printf("  @%06X %02X vs %02X\n", i, single.Ram()[i], parallel.Ram()[i]); n++; }
				}
				int shown = 0;
				for(uint32 y = 0; y < 384 && shown < 12; y++)
					for(uint32 x = 0; x < 512 && shown < 12; x++)
					{
						uint32 a = single.Pixel32(300 * 8192, FBW, x, y), b = parallel.Pixel32(300 * 8192, FBW, x, y);
						if(a != b) { std::printf("  (%u,%u) single %08X batched %08X\n", x, y, a, b); shown++; }
					}
			}
			uint32 diff = 0;
			for(uint32 i = 0; i < CGSHandler::RAMSIZE; i++) diff += single.Ram()[i] != parallel.Ram()[i];
			std::printf("round %d: %u bytes differ\n", round, diff);
			failures++;
		}
	}
	std::printf("parallel rendering: %s\n", failures ? "MISMATCH" : "bit-identical");
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
