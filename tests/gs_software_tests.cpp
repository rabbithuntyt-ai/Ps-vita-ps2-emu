// Unit tests for the software GS renderer. They drive CGSH_Software exactly
// like the EE would (GS register writes) and inspect GS local memory.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "GSH_Software.h"
#include "GsMemory.h"
#include "EmuSession.h"
#include "GsDriver.h"

using namespace GsTest;

namespace
{
	int g_failures = 0;
	int g_checks = 0;

#define CHECK_EQ(expected, actual)                                                                  \
	do                                                                                              \
	{                                                                                               \
		g_checks++;                                                                                 \
		auto e_ = (expected);                                                                       \
		auto a_ = (actual);                                                                         \
		if(e_ != a_)                                                                                \
		{                                                                                           \
			g_failures++;                                                                           \
			std::printf("  FAIL %s:%d: expected 0x%llX, got 0x%llX (%s)\n", __FILE__, __LINE__,    \
			            (unsigned long long)e_, (unsigned long long)a_, #actual);                  \
		}                                                                                           \
	} while(0)


	void TestSpriteFill()
	{
		std::printf("sprite fill\n");
		CTestGs gs;
		SetupContext(gs);
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);
		gs.Write(GS_REG_RGBAQ, Rgbaq(0x10, 0x20, 0x30, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(10), OffX(20), 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(30), OffX(25), 0));

		CHECK_EQ(0x80302010u, gs.Pixel32(0, FBW, 10, 20));
		CHECK_EQ(0x80302010u, gs.Pixel32(0, FBW, 29, 24));
		CHECK_EQ(0u, gs.Pixel32(0, FBW, 30, 24)); //right edge exclusive
		CHECK_EQ(0u, gs.Pixel32(0, FBW, 10, 25)); //bottom edge exclusive
		CHECK_EQ(0u, gs.Pixel32(0, FBW, 9, 20));
	}

	void TestScissor()
	{
		std::printf("scissor\n");
		CTestGs gs;
		SetupContext(gs);
		gs.Write(GS_REG_SCISSOR_1, Scissor(5, 7, 5, 7));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);
		gs.Write(GS_REG_RGBAQ, Rgbaq(0xFF, 0, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(100), OffX(100), 0));
		CHECK_EQ(0x800000FFu, gs.Pixel32(0, FBW, 5, 5));
		CHECK_EQ(0x800000FFu, gs.Pixel32(0, FBW, 7, 7));
		CHECK_EQ(0u, gs.Pixel32(0, FBW, 8, 7));
		CHECK_EQ(0u, gs.Pixel32(0, FBW, 4, 5));
		CHECK_EQ(1u, gs.GetPrimitiveCount());
	}

	// Two triangles sharing a diagonal must cover a square exactly once.
	void TestTriangleFillConvention()
	{
		std::printf("triangle fill convention\n");
		CTestGs gs;
		SetupContext(gs);
		//Additive blend: Cs + Cd (A=Cs, B=0, C=FIX(128), D=Cd)
		gs.Write(GS_REG_ALPHA_1, 0 | (2 << 2) | (2 << 4) | (1 << 6) | (128ULL << 32));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_TRIANGLE | (1 << 6)); //ABE
		gs.Write(GS_REG_RGBAQ, Rgbaq(1, 0, 0, 0x80));

		uint32 x0 = OffX(8), x1 = OffX(24), y0 = OffX(8), y1 = OffX(24);
		gs.Write(GS_REG_XYZ2, Xyz(x0, y0, 0));
		gs.Write(GS_REG_XYZ2, Xyz(x1, y0, 0));
		gs.Write(GS_REG_XYZ2, Xyz(x0, y1, 0));
		gs.Write(GS_REG_XYZ2, Xyz(x1, y0, 0));
		gs.Write(GS_REG_XYZ2, Xyz(x1, y1, 0));
		gs.Write(GS_REG_XYZ2, Xyz(x0, y1, 0));

		uint32 covered = 0, doubled = 0;
		for(uint32 y = 0; y < 40; y++)
		{
			for(uint32 x = 0; x < 40; x++)
			{
				uint32 red = gs.Pixel32(0, FBW, x, y) & 0xFF;
				if(red == 1) covered++;
				if(red > 1) doubled++;
			}
		}
		CHECK_EQ(16u * 16u, covered);
		CHECK_EQ(0u, doubled);
		CHECK_EQ(1u, gs.Pixel32(0, FBW, 8, 8) & 0xFF);
		CHECK_EQ(0u, gs.Pixel32(0, FBW, 24, 8) & 0xFF);
	}

	void TestGouraud()
	{
		std::printf("gouraud\n");
		CTestGs gs;
		SetupContext(gs);
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_TRIANGLESTRIP | (1 << 3));
		gs.Write(GS_REG_RGBAQ, Rgbaq(0, 0, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 0));
		gs.Write(GS_REG_RGBAQ, Rgbaq(200, 0, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(100), OffX(0), 0));
		gs.Write(GS_REG_RGBAQ, Rgbaq(0, 0, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(100), 0));
		gs.Write(GS_REG_RGBAQ, Rgbaq(200, 0, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(100), OffX(100), 0));

		uint32 left = gs.Pixel32(0, FBW, 5, 50) & 0xFF;
		uint32 mid = gs.Pixel32(0, FBW, 50, 50) & 0xFF;
		uint32 right = gs.Pixel32(0, FBW, 95, 50) & 0xFF;
		CHECK_EQ(true, left < mid && mid < right);
		CHECK_EQ(true, mid >= 95 && mid <= 105);
	}

	void TestDepth()
	{
		std::printf("depth test\n");
		CTestGs gs;
		SetupContext(gs);
		gs.Write(GS_REG_ZBUF_1, 150 | (0ULL << 24)); //Z32, writes enabled
		gs.Write(GS_REG_TEST_1, (1ULL << 16) | (CGSHandler::DEPTH_TEST_GEQUAL << 17));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);

		gs.Write(GS_REG_RGBAQ, Rgbaq(0xFF, 0, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 1000));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(16), OffX(16), 1000));

		gs.Write(GS_REG_RGBAQ, Rgbaq(0, 0xFF, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(8), OffX(0), 500));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(24), OffX(16), 500));

		CHECK_EQ(0x800000FFu, gs.Pixel32(0, FBW, 10, 4)); //occluded, kept red
		CHECK_EQ(0x8000FF00u, gs.Pixel32(0, FBW, 20, 4)); //uncovered area, green
		CHECK_EQ(1000u, GsMemory::ReadRaw(gs.Ram(), CGSHandler::PSMZ32, 150 * 8192, FBW, 10, 4));
	}

	void TestAlphaBlend()
	{
		std::printf("alpha blend\n");
		CTestGs gs;
		SetupContext(gs);
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);
		gs.Write(GS_REG_RGBAQ, Rgbaq(200, 100, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(4), OffX(4), 0));

		//(Cs - Cd) * As + Cd, As = 0x40 (50%)
		gs.Write(GS_REG_ALPHA_1, 0 | (1 << 2) | (0 << 4) | (1 << 6));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE | (1 << 6));
		gs.Write(GS_REG_RGBAQ, Rgbaq(0, 0, 200, 0x40));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(4), OffX(4), 0));

		uint32 pixel = gs.Pixel32(0, FBW, 1, 1);
		CHECK_EQ(100u, pixel & 0xFF);
		CHECK_EQ(50u, (pixel >> 8) & 0xFF);
		CHECK_EQ(100u, (pixel >> 16) & 0xFF);
	}

	void TestFrameMaskAnd16Bit()
	{
		std::printf("frame mask / 16-bit frame\n");
		CTestGs gs;
		SetupContext(gs);
		gs.Write(GS_REG_FRAME_1, Frame(0, CGSHandler::PSMCT16));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);
		gs.Write(GS_REG_RGBAQ, Rgbaq(0xF8, 0x08, 0x80, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(2), OffX(2), 0));
		uint32 raw = GsMemory::ReadRaw(gs.Ram(), CGSHandler::PSMCT16, 0, FBW, 1, 1);
		CHECK_EQ(0x1Fu | (1u << 5) | (0x10u << 10) | 0x8000u, raw);

		gs.Write(GS_REG_FRAME_1, Frame(0, CGSHandler::PSMCT32, 0xFFFFFF00));
		gs.Write(GS_REG_RGBAQ, Rgbaq(0x55, 0x66, 0x77, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(100), OffX(0), 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(102), OffX(2), 0));
		CHECK_EQ(0x00000055u, gs.Pixel32(0, FBW, 101, 1));
	}

	// 4-bit texture with a 32-bit CLUT loaded through TEX0.CLD, drawn with UVs.
	void TestTexturedSpriteClut()
	{
		std::printf("textured sprite (PSMT4 + CLUT)\n");
		CTestGs gs;
		SetupContext(gs);

		const uint32 texPtr = 0x200000; //bytes
		const uint32 clutPtr = 0x300000;
		//8x8 texture: texel index = x
		for(uint32 y = 0; y < 8; y++)
		{
			for(uint32 x = 0; x < 8; x++)
			{
				GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMT4, texPtr, 1, x, y, x);
			}
		}
		//CLUT (CSM1, 16 entries laid out 8x2): color i = (i * 16) red
		for(uint32 i = 0; i < 16; i++)
		{
			GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMCT32, clutPtr, 1, i & 7, i >> 3, 0x80000000 | (i * 16));
		}

		uint64 tex0 = static_cast<uint64>(texPtr / 256) | (1ULL << 14) | (static_cast<uint64>(CGSHandler::PSMT4) << 20) |
		              (3ULL << 26) | (3ULL << 30) | (1ULL << 34) /*TCC*/ | (static_cast<uint64>(CGSHandler::TEX0_FUNCTION_DECAL) << 35) |
		              (static_cast<uint64>(clutPtr / 256) << 37) | (0ULL << 51) /*CPSM CT32*/ | (1ULL << 61) /*CLD*/;
		gs.Write(GS_REG_TEX0_1, tex0);
		gs.Write(GS_REG_CLAMP_1, 0);
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE | (1 << 4) | (1 << 8)); //TME, FST
		gs.Write(GS_REG_RGBAQ, Rgbaq(0x80, 0x80, 0x80, 0x80));
		gs.Write(GS_REG_UV, Uv(0, 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 0));
		gs.Write(GS_REG_UV, Uv(8 << 4, 8 << 4));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(8), OffX(8), 0));

		for(uint32 x = 0; x < 8; x++)
		{
			CHECK_EQ(0x80000000u | (x * 16), gs.Pixel32(0, FBW, x, 3));
		}
	}

	void TestLocalToLocal()
	{
		std::printf("local to local transfer\n");
		CTestGs gs;
		SetupContext(gs);
		for(uint32 i = 0; i < 16; i++)
		{
			GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMCT32, 0, FBW, i, 0, 0x1000 + i);
		}
		//BITBLTBUF: src=0 (CT32, width 10), dst=0x100000 (CT32, width 10)
		uint64 bitbltbuf = (0ULL) | (static_cast<uint64>(FBW) << 16) | (static_cast<uint64>(0x100000 / 256) << 32) |
		                   (static_cast<uint64>(FBW) << 48);
		gs.Write(GS_REG_BITBLTBUF, bitbltbuf);
		gs.Write(GS_REG_TRXPOS, (static_cast<uint64>(4) << 32) | (static_cast<uint64>(2) << 48)); //dst (4,2)
		gs.Write(GS_REG_TRXREG, 16 | (1ULL << 32));
		gs.Write(GS_REG_TRXDIR, 2);
		CHECK_EQ(0x1000u, gs.Pixel32(0x100000, FBW, 4, 2));
		CHECK_EQ(0x100Fu, gs.Pixel32(0x100000, FBW, 19, 2));
	}

	void TestFlipOutput()
	{
		std::printf("display flip\n");
		CTestGs gs;
		SetupContext(gs);
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);
		gs.Write(GS_REG_RGBAQ, Rgbaq(0x11, 0x22, 0x33, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(640), OffX(448), 0));

		uint32 seenWidth = 0, seenHeight = 0, seenPixel = 0;
		gs.SetFrameSink([&](const uint32* pixels, uint32 w, uint32 h) {
			seenWidth = w;
			seenHeight = h;
			seenPixel = pixels[w * 10 + 10];
		});
		gs.WritePrivRegister(CGSHandler::GS_PMODE, 0x1);
		gs.WritePrivRegister(CGSHandler::GS_DISPFB1, (FBW << 9));
		gs.WritePrivRegister(CGSHandler::GS_DISPFB1 + 4, 0);
		//DISPLAY1: DX=0, DY=0, MAGH=3 (x4), W=2559, H=447
		uint64 display = (3ULL << 23) | (2559ULL << 32) | (447ULL << 44);
		gs.WritePrivRegister(CGSHandler::GS_DISPLAY1, static_cast<uint32>(display));
		gs.WritePrivRegister(CGSHandler::GS_DISPLAY1 + 4, static_cast<uint32>(display >> 32));
		gs.Present();
		CHECK_EQ(640u, seenWidth);
		CHECK_EQ(true, seenHeight > 0);
		CHECK_EQ(0xFF332211u, seenPixel);
	}
}

int main()
{
	// Play!'s configuration singleton needs a writable data directory.
	CEmuSession::SetDataPaths("vitaps2_test_data", ".");

	TestSpriteFill();
	TestScissor();
	TestTriangleFillConvention();
	TestGouraud();
	TestDepth();
	TestAlphaBlend();
	TestFrameMaskAnd16Bit();
	TestTexturedSpriteClut();
	TestLocalToLocal();
	TestFlipOutput();

	std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
	return (g_failures == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
