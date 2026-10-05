// Software GS throughput benchmark. Each workload mimics a common PS2 draw
// pattern and reports millions of pixels shaded per second.
//
//   gs_benchmark [seconds-per-workload]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>
#include "EmuSession.h"
#include "TestGs.h"

using namespace GsTest;

namespace
{
	constexpr uint32 TEX_PTR = 0x200000;
	constexpr uint32 CLUT_PTR = 0x380000;

	void UploadTextures(CTestGs& gs)
	{
		//256x256 8-bit indexed texture + 256-entry 32-bit CLUT, and a 256x256 CT32 texture
		for(uint32 y = 0; y < 256; y++)
		{
			for(uint32 x = 0; x < 256; x++)
			{
				GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMT8, TEX_PTR, 4, x, y, (x ^ y) & 0xFF);
				GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMCT32, TEX_PTR + 0x40000, 4, x, y,
				                   0x80000000 | (x << 16) | (y << 8) | ((x + y) & 0xFF));
			}
		}
		for(uint32 i = 0; i < 256; i++)
		{
			//CSM1 layout of a 256 color CLUT
			uint32 swizzled = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
			GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMCT32, CLUT_PTR, 1, swizzled & 0xF, swizzled >> 4,
			                   0x80000000 | (i * 0x010203));
		}
	}

	uint64 Tex0(uint32 psm, bool loadClut)
	{
		uint32 ptr = (psm == CGSHandler::PSMT8) ? TEX_PTR : TEX_PTR + 0x40000;
		return static_cast<uint64>(ptr / 256) | (4ULL << 14) | (static_cast<uint64>(psm) << 20) | (8ULL << 26) |
		       (8ULL << 30) | (1ULL << 34) | (static_cast<uint64>(CGSHandler::TEX0_FUNCTION_MODULATE) << 35) |
		       (static_cast<uint64>(CLUT_PTR / 256) << 37) | ((loadClut ? 1ULL : 0ULL) << 61);
	}

	struct WORKLOAD
	{
		const char* name;
		std::function<void(CTestGs&)> setup;
		std::function<uint64(CTestGs&)> draw; //returns pixels covered
	};

	uint64 FullScreenSprite(CTestGs& gs, bool uv)
	{
		if(uv) gs.Write(GS_REG_UV, Uv(0, 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 0x1000));
		if(uv) gs.Write(GS_REG_UV, Uv(256 << 4, 256 << 4));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(640), OffX(448), 0x1000));
		return 640 * 448;
	}

	// A mesh of 32x32 pixel quads (2 triangles each) covering the screen.
	uint64 TriangleGrid(CTestGs& gs, bool textured)
	{
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_TRIANGLESTRIP | (1 << 3) | (textured ? (1 << 4) : 0) |
		                          (textured ? (1 << 6) : 0));
		uint64 pixels = 0;
		for(uint32 y = 0; y < 448; y += 32)
		{
			gs.Write(GS_REG_PRIM, CGSHandler::PRIM_TRIANGLESTRIP | (1 << 3) | (textured ? (1 << 4) : 0) |
			                          (textured ? (1 << 6) : 0));
			for(uint32 x = 0; x <= 640; x += 32)
			{
				for(uint32 row = 0; row < 2; row++)
				{
					uint32 py = y + row * 32;
					gs.Write(GS_REG_RGBAQ, Rgbaq(x & 0xFF, py & 0xFF, 0x80, 0x60));
					if(textured)
					{
						uint32 s, t;
						float fs = x / 640.0f, ft = py / 448.0f, one = 1.0f;
						std::memcpy(&s, &fs, 4);
						std::memcpy(&t, &ft, 4);
						gs.Write(GS_REG_ST, s | (static_cast<uint64>(t) << 32));
						(void)one;
					}
					gs.Write(GS_REG_XYZ2, Xyz(OffX(x), OffX(py), 0x2000 + x));
				}
			}
			pixels += 640 * 32;
		}
		return pixels;
	}

	std::vector<WORKLOAD> MakeWorkloads()
	{
		return {
		    {"flat sprite fill (clear)",
		     [](CTestGs& gs) { gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE); },
		     [](CTestGs& gs) {
			     gs.Write(GS_REG_RGBAQ, Rgbaq(1, 2, 3, 0x80));
			     return FullScreenSprite(gs, false);
		     }},
		    {"textured sprite, T8+CLUT (2D games, FMV)",
		     [](CTestGs& gs) {
			     gs.Write(GS_REG_TEX0_1, Tex0(CGSHandler::PSMT8, true));
			     gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE | (1 << 4) | (1 << 8));
		     },
		     [](CTestGs& gs) {
			     gs.Write(GS_REG_RGBAQ, Rgbaq(0x80, 0x80, 0x80, 0x80));
			     return FullScreenSprite(gs, true);
		     }},
		    {"textured sprite, CT32 bilinear + alpha blend",
		     [](CTestGs& gs) {
			     gs.Write(GS_REG_TEX0_1, Tex0(CGSHandler::PSMCT32, false));
			     gs.Write(GS_REG_TEX1_1, 1ULL << 5); //MMAG linear
			     gs.Write(GS_REG_ALPHA_1, 0 | (1 << 2) | (0 << 4) | (1 << 6));
			     gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE | (1 << 4) | (1 << 6) | (1 << 8));
		     },
		     [](CTestGs& gs) {
			     gs.Write(GS_REG_RGBAQ, Rgbaq(0x80, 0x80, 0x80, 0x40));
			     return FullScreenSprite(gs, true);
		     }},
		    {"gouraud triangles + z test",
		     [](CTestGs& gs) {
			     gs.Write(GS_REG_ZBUF_1, 150);
			     gs.Write(GS_REG_TEST_1, (1ULL << 16) | (CGSHandler::DEPTH_TEST_GEQUAL << 17));
		     },
		     [](CTestGs& gs) { return TriangleGrid(gs, false); }},
		    {"STQ textured T8 triangles + blend + z (3D games)",
		     [](CTestGs& gs) {
			     gs.Write(GS_REG_ZBUF_1, 150);
			     gs.Write(GS_REG_TEST_1, (1ULL << 16) | (CGSHandler::DEPTH_TEST_GEQUAL << 17));
			     gs.Write(GS_REG_TEX0_1, Tex0(CGSHandler::PSMT8, true));
			     gs.Write(GS_REG_ALPHA_1, 0 | (1 << 2) | (0 << 4) | (1 << 6));
		     },
		     [](CTestGs& gs) { return TriangleGrid(gs, true); }},
		};
	}
}

int main(int argc, char** argv)
{
	double secondsPerWorkload = (argc > 1) ? std::atof(argv[1]) : 1.0;
	CEmuSession::SetDataPaths("vitaps2_test_data", ".");

	double total = 0;
	int count = 0;
	for(auto& workload : MakeWorkloads())
	{
		CTestGs gs;
		SetupContext(gs);
		UploadTextures(gs);
		workload.setup(gs);

		uint64 pixels = 0;
		auto start = std::chrono::steady_clock::now();
		double elapsed = 0;
		do
		{
			pixels += workload.draw(gs);
			elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		} while(elapsed < secondsPerWorkload);

		double mpixels = pixels / elapsed / 1e6;
		total += mpixels;
		count++;
		// A 640x448 frame at 60fps with 3x overdraw needs ~52 Mpix/s.
		std::printf("%-48s %8.1f Mpix/s  (%5.1f full frames/s)\n", workload.name, mpixels, mpixels * 1e6 / (640.0 * 448.0));
	}
	std::printf("%-48s %8.1f Mpix/s\n", "average", total / count);
	return 0;
}
