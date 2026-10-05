#pragma once

// Shared helpers to drive CGSH_Software directly with GS register writes.

#include <cstring>
#include "GSH_Software.h"
#include "GsMemory.h"

namespace GsTest
{
	class CTestGs : public CGSH_Software
	{
	public:
		CTestGs()
		    : CGSH_Software(false)
		{
			ResetBase();
			InitializeImpl();
			ResetImpl();
		}

		void Write(uint8 reg, uint64 value)
		{
			WriteRegisterImpl(reg, value);
		}

		void Present()
		{
			FlipImpl(GetCurrentDisplayInfo());
		}

		uint32 Pixel32(uint32 fbp, uint32 fbw, uint32 x, uint32 y)
		{
			return GsMemory::ReadRaw(m_pRAM, PSMCT32, fbp, fbw, x, y);
		}

		uint8* Ram()
		{
			return m_pRAM;
		}
	};

	constexpr uint32 FBW = 10; //640 pixels

	inline uint64 Frame(uint32 fbpPages, uint32 psm = CGSHandler::PSMCT32, uint32 mask = 0)
	{
		return static_cast<uint64>(fbpPages) | (static_cast<uint64>(FBW) << 16) | (static_cast<uint64>(psm) << 24) |
		       (static_cast<uint64>(mask) << 32);
	}

	inline uint64 Scissor(uint32 x0, uint32 x1, uint32 y0, uint32 y1)
	{
		return static_cast<uint64>(x0) | (static_cast<uint64>(x1) << 16) | (static_cast<uint64>(y0) << 32) |
		       (static_cast<uint64>(y1) << 48);
	}

	inline uint64 Rgbaq(uint8 r, uint8 g, uint8 b, uint8 a)
	{
		uint32 q;
		float one = 1.0f;
		std::memcpy(&q, &one, 4);
		return static_cast<uint64>(r) | (static_cast<uint64>(g) << 8) | (static_cast<uint64>(b) << 16) |
		       (static_cast<uint64>(a) << 24) | (static_cast<uint64>(q) << 32);
	}

	inline uint64 Xyz(uint32 x16, uint32 y16, uint32 z)
	{
		return static_cast<uint64>(x16 & 0xFFFF) | (static_cast<uint64>(y16 & 0xFFFF) << 16) | (static_cast<uint64>(z) << 32);
	}

	inline uint64 Uv(uint32 u16, uint32 v16)
	{
		return static_cast<uint64>(u16) | (static_cast<uint64>(v16) << 16);
	}

	// Common setup: context 1, 640x448 CT32 frame at page 0, Z32 at page 150,
	// XYOFFSET (2048,2048) like most games, no tests.
	inline void SetupContext(CTestGs& gs)
	{
		gs.Write(GS_REG_FRAME_1, Frame(0));
		gs.Write(GS_REG_ZBUF_1, 150 | (0ULL << 24) | (1ULL << 32)); //Z32, masked
		gs.Write(GS_REG_SCISSOR_1, Scissor(0, 639, 0, 447));
		gs.Write(GS_REG_XYOFFSET_1, (2048ULL << 4) | ((2048ULL << 4) << 32));
		gs.Write(GS_REG_TEST_1, 0);
		gs.Write(GS_REG_PRMODECONT, 1);
	}

	inline uint32 OffX(uint32 x)
	{
		return (2048 + x) << 4;
	}
}
