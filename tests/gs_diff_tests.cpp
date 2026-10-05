// Differential test: the optimized rasterizer must produce the same output as
// the simple reference implementation for random states and primitives.
//
// Coverage and state handling must match. Fixed-point interpolation may
// legitimately differ by rounding (one color step) or pick a neighbouring
// texel exactly on a texel boundary, so a small tolerance is applied.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <set>
#include <vector>
#include "SoftwareRasterizer.h"
#include "GsMemory.h"
#include "reference/ReferenceRasterizer.h"
#include "xxhash.h"

namespace
{
	constexpr uint32 FRAME_PTR = 0x000000;
	constexpr uint32 DEPTH_PTR = 0x100000;
	constexpr uint32 TEX_PTR = 0x200000;
	constexpr uint32 FBW = 8; //512 pixels
	constexpr int32 AREA = 256;

	std::mt19937 g_rng(20261005);

	uint32 Rand(uint32 n)
	{
		return g_rng() % n;
	}

	template <typename T>
	T Pick(std::initializer_list<T> values)
	{
		return *(values.begin() + Rand(static_cast<uint32>(values.size())));
	}

	template <typename State>
	State RandomState()
	{
		State s;
		s.gouraud = Rand(2);
		s.textured = Rand(3) != 0;
		s.fog = Rand(4) == 0;
		s.alphaBlend = Rand(2);
		s.fst = Rand(2);

		s.fbp = FRAME_PTR;
		s.fbw = FBW;
		s.fpsm = Pick<uint32>({CGSHandler::PSMCT32, CGSHandler::PSMCT32, CGSHandler::PSMCT24, CGSHandler::PSMCT16, CGSHandler::PSMCT16S});
		s.fbmsk = Rand(5) == 0 ? (g_rng() & 0xFF00FF00) : 0;
		s.zbp = DEPTH_PTR;
		s.zpsm = Pick<uint32>({0, 1, 2, 0xA});
		s.zmsk = Rand(4) == 0;

		s.scax0 = Rand(16);
		s.scay0 = Rand(16);
		s.scax1 = AREA - 1 - Rand(16);
		s.scay1 = AREA - 1 - Rand(16);

		s.ate = Rand(3) == 0;
		s.atst = Rand(8);
		s.aref = Rand(256);
		s.afail = Rand(4);
		s.date = Rand(6) == 0;
		s.datm = Rand(2);
		s.zte = Rand(3) != 0;
		s.ztst = Pick<uint32>({1, 2, 3, 2, 3});

		s.blendA = Rand(3);
		s.blendB = Rand(3);
		s.blendC = Rand(3);
		s.blendD = Rand(3);
		s.blendFix = Rand(256);
		s.pabe = Rand(5) == 0;
		s.fba = Rand(5) == 0;
		s.colClamp = Rand(4) != 0;
		s.fogColor = g_rng() & 0xFFFFFF;

		s.tbp = TEX_PTR;
		s.tbw = 4;
		s.tpsm = Pick<uint32>({CGSHandler::PSMCT32, CGSHandler::PSMCT24, CGSHandler::PSMCT16, CGSHandler::PSMCT16S,
		                       CGSHandler::PSMT8, CGSHandler::PSMT4, CGSHandler::PSMT8H, CGSHandler::PSMT4HL, CGSHandler::PSMT4HH});
		s.tw = 1u << (2 + Rand(7));
		s.th = 1u << (2 + Rand(7));
		s.tcc = Rand(2);
		s.tfx = Rand(4);
		s.cpsm = Pick<uint32>({CGSHandler::PSMCT32, CGSHandler::PSMCT16});
		s.csa = CGsPixelFormats::IsPsmIDTEX4(s.tpsm) ? Rand(16) : 0;
		s.bilinear = Rand(3) == 0;
		s.wms = Rand(4);
		s.wmt = Rand(4);
		s.minu = Rand(64);
		s.maxu = s.minu + Rand(64);
		s.minv = Rand(64);
		s.maxv = s.minv + Rand(64);
		if(s.wms == CGSHandler::CLAMP_MODE_REGION_REPEAT) s.minu = Rand(256), s.maxu = Rand(256);
		if(s.wmt == CGSHandler::CLAMP_MODE_REGION_REPEAT) s.minv = Rand(256), s.maxv = Rand(256);
		s.ta0 = Rand(256);
		s.ta1 = Rand(256);
		s.aem = Rand(2);
		return s;
	}

	template <typename Vertex>
	Vertex RandomVertex(const CSoftwareRasterizer::STATE& state)
	{
		Vertex v;
		v.x = static_cast<int32>(Rand(AREA * 16 + 160)) - 80;
		v.y = static_cast<int32>(Rand(AREA * 16 + 160)) - 80;
		v.z = g_rng();
		if(Rand(2)) v.z &= 0xFFFFFF;
		v.r = Rand(256);
		v.g = Rand(256);
		v.b = Rand(256);
		v.a = Rand(256);
		v.fog = Rand(256);
		v.q = 0.5f + Rand(1000) / 500.0f;
		v.s = (Rand(2000) / 1000.0f - 0.25f) * v.q;
		v.t = (Rand(2000) / 1000.0f - 0.25f) * v.q;
		v.u = Rand(state.tw * 32) / 16.0f;
		v.v = Rand(state.th * 32) / 16.0f;
		return v;
	}

	CReferenceRasterizer::STATE ToReference(const CSoftwareRasterizer::STATE& s)
	{
		CReferenceRasterizer::STATE r;
		static_assert(sizeof(r) == sizeof(s), "STATE layouts must match");
		std::memcpy(&r, &s, sizeof(s));
		return r;
	}

	CReferenceRasterizer::VERTEX ToReference(const CSoftwareRasterizer::VERTEX& v)
	{
		CReferenceRasterizer::VERTEX r;
		static_assert(sizeof(r) == sizeof(v), "VERTEX layouts must match");
		std::memcpy(&r, &v, sizeof(v));
		return r;
	}

	struct RESULT
	{
		uint32 pixels = 0;
		uint32 colorMismatch = 0;
		uint32 depthMismatch = 0;
		std::set<uint32> columns, rows; //where color mismatches happened
	};

	bool g_verbose = false;

	void Compare(uint8* ramA, uint8* ramB, const CSoftwareRasterizer::STATE& s, RESULT& result)
	{
		for(uint32 y = 0; y < AREA; y++)
		{
			for(uint32 x = 0; x < AREA; x++)
			{
				result.pixels++;
				uint32 ca = CSoftwareRasterizer::ReadColor32(ramA, s.fpsm, s.fbp, s.fbw, x, y);
				uint32 cb = CSoftwareRasterizer::ReadColor32(ramB, s.fpsm, s.fbp, s.fbw, x, y);
				if(ca != cb)
				{
					uint32 tolerance = GsMemory::IsPsm16(s.fpsm) ? 8 : (s.textured && s.bilinear) ? 6 : 2;
					for(uint32 shift = 0; shift < 32; shift += 8)
					{
						int32 d = static_cast<int32>((ca >> shift) & 0xFF) - static_cast<int32>((cb >> shift) & 0xFF);
						if(static_cast<uint32>(std::abs(d)) > tolerance)
						{
							if(g_verbose) std::printf("  (%u,%u) %08X vs %08X\n", x, y, ca, cb);
							result.colorMismatch++;
							result.columns.insert(x);
							result.rows.insert(y);
							break;
						}
					}
				}
				uint32 zpsm = 0x30 | (s.zpsm & 0xF);
				uint32 za = GsMemory::ReadRaw(ramA, zpsm, s.zbp, s.fbw, x, y);
				uint32 zb = GsMemory::ReadRaw(ramB, zpsm, s.zbp, s.fbw, x, y);
				int64 dz = static_cast<int64>(za) - static_cast<int64>(zb);
				if(std::llabs(dz) > 2) result.depthMismatch++;
			}
		}
	}
}

int main(int argc, char** argv)
{
	int verboseCase = (argc > 1) ? std::atoi(argv[1]) : -1;
	std::vector<uint8> ramA(CGSHandler::RAMSIZE), ramB(CGSHandler::RAMSIZE);
	std::vector<uint16> clut(CGSHandler::CLUTENTRYCOUNT);

	RESULT total;
	uint32 failingCases = 0;
	const uint32 caseCount = 400;
	for(uint32 testCase = 0; testCase < caseCount; testCase++)
	{
		for(auto& byte : ramA) byte = static_cast<uint8>(g_rng());
		ramB = ramA;
		for(auto& entry : clut) entry = static_cast<uint16>(g_rng());

		CSoftwareRasterizer fast;
		CReferenceRasterizer reference;
		fast.SetMemory(ramA.data(), clut.data());
		fast.SetClutHash(XXH3_64bits(clut.data(), clut.size() * 2));
		reference.SetMemory(ramB.data(), clut.data());

		auto state = RandomState<CSoftwareRasterizer::STATE>();
		fast.SetState(state);
		reference.SetState(ToReference(state));

		for(uint32 prim = 0; prim < 6; prim++)
		{
			auto v0 = RandomVertex<CSoftwareRasterizer::VERTEX>(state);
			auto v1 = RandomVertex<CSoftwareRasterizer::VERTEX>(state);
			auto v2 = RandomVertex<CSoftwareRasterizer::VERTEX>(state);
			switch(Rand(5))
			{
			case 0:
			case 1:
				fast.DrawTriangle(v0, v1, v2);
				reference.DrawTriangle(ToReference(v0), ToReference(v1), ToReference(v2));
				break;
			case 2:
			case 3:
				fast.DrawSprite(v0, v1);
				reference.DrawSprite(ToReference(v0), ToReference(v1));
				break;
			case 4:
				fast.DrawLine(v0, v1);
				reference.DrawLine(ToReference(v0), ToReference(v1));
				fast.DrawPoint(v2);
				reference.DrawPoint(ToReference(v2));
				break;
			}
		}

		RESULT result;
		g_verbose = (static_cast<int>(testCase) == verboseCase);
		Compare(ramA.data(), ramB.data(), state, result);
		total.pixels += result.pixels;
		total.colorMismatch += result.colorMismatch;
		total.depthMismatch += result.depthMismatch;
		// Allow a handful of rounding pixels per case, or mismatches confined to
		// one or two lines (a texel boundary where float and fixed point
		// coordinates round to adjacent texels).
		// Nearest-sampled random textures turn any neighbouring texel pick into a
		// large color difference, so textured cases get a slightly larger budget.
		bool boundaryOnly = (result.columns.size() <= 2) || (result.rows.size() <= 2);
		uint32 budget = state.textured ? 128 : 40;
		if((result.colorMismatch > budget && !boundaryOnly) || result.depthMismatch > 40)
		{
			failingCases++;
			if(failingCases <= 10)
			{
				std::printf("case %u: %u color / %u depth mismatches (fpsm %X zpsm %X tex %d tpsm %X fst %d bilinear %d gouraud %d zte %d ztst %d ate %d blend %d)\n",
				            testCase, result.colorMismatch, result.depthMismatch, state.fpsm, state.zpsm, state.textured,
				            state.tpsm, state.fst, state.bilinear, state.gouraud, state.zte, state.ztst, state.ate, state.alphaBlend);
			}
		}
	}

	std::printf("differential test: %u cases, %u failing, %.4f%% color / %.4f%% depth pixel mismatches\n",
	            caseCount, failingCases, 100.0 * total.colorMismatch / total.pixels, 100.0 * total.depthMismatch / total.pixels);
	double mismatchRate = static_cast<double>(total.colorMismatch) / total.pixels;
	return ((failingCases == 0) && (mismatchRate < 0.0001)) ? EXIT_SUCCESS : EXIT_FAILURE;
}
