// GPU renderer tests: every scene is rendered by the software renderer (the
// accuracy reference) and by CGSH_Hardware on an offscreen Mesa context, and
// the displayed frames are compared.
//
// The GPU path uses OpenGL's rasterization rules and fixed-function blending,
// so exact equality is not expected: channels may differ by a few steps and a
// small fraction of pixels (primitive edges) may differ more.

#include <GL/osmesa.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>
#include "EmuSession.h"
#include "GSH_Hardware.h"
#include "GsDriver.h"
#include "GsMemory.h"

using namespace GsTest;

namespace
{
	struct FRAME
	{
		std::vector<uint32> pixels;
		uint32 width = 0, height = 0;
	};

	// Common register interface for both renderers.
	class IDriver
	{
	public:
		virtual ~IDriver() = default;
		virtual void Write(uint8 reg, uint64 value) = 0;
		virtual void WritePriv(uint32 reg, uint32 value) = 0;
		virtual uint8* Ram() = 0;
		virtual void Present() = 0;
		virtual void Transfer(const uint8* data, uint32 size) = 0;
		FRAME frame;
	};

	class CSoftwareDriver : public IDriver
	{
	public:
		CSoftwareDriver()
		{
			m_gs.SetFrameSink([this](std::vector<uint32>& pixels, uint32 w, uint32 h) {
				frame.pixels = pixels;
				frame.width = w;
				frame.height = h;
			});
		}
		void Write(uint8 reg, uint64 value) override
		{
			m_gs.Write(reg, value);
		}
		void WritePriv(uint32 reg, uint32 value) override
		{
			m_gs.WritePrivRegister(reg, value);
		}
		uint8* Ram() override
		{
			return m_gs.Ram();
		}
		void Present() override
		{
			m_gs.Present();
		}
		void Transfer(const uint8* data, uint32 size) override
		{
			m_gs.FeedImageData(data, size);
		}

	private:
		CTestGs m_gs;
	};

	class CHardwareGs : public CGSH_Hardware
	{
	public:
		explicit CHardwareGs(const OPTIONS& options)
		    : CGSH_Hardware(options)
		{
			ResetBase();
			InitializeImpl();
			ResetImpl();
		}
		~CHardwareGs() override
		{
			ReleaseImpl();
		}
		void Write(uint8 reg, uint64 value)
		{
			WriteRegisterImpl(reg, value);
		}
		void Present()
		{
			FlipImpl(GetCurrentDisplayInfo());
		}
		uint8* Ram()
		{
			return m_pRAM;
		}
	};

	class CHardwareDriver : public IDriver
	{
	public:
		CHardwareDriver()
		    : m_gs(MakeOptions())
		{
		}
		void Write(uint8 reg, uint64 value) override
		{
			m_gs.Write(reg, value);
		}
		void WritePriv(uint32 reg, uint32 value) override
		{
			m_gs.WritePrivRegister(reg, value);
		}
		uint8* Ram() override
		{
			return m_gs.Ram();
		}
		void Present() override
		{
			m_gs.Present();
		}
		void Transfer(const uint8* data, uint32 size) override
		{
			m_gs.FeedImageData(data, size);
		}
		CGSH_Hardware::STATS Stats() const
		{
			return m_gs.GetCurrentFrameStats();
		}

		// The displayed image came from a GPU render target (not GS memory).
		bool PresentedRenderTarget() const
		{
			return m_gs.GetDisplayTexture().textureWidth == 640;
		}

	private:
		CGSH_Hardware::OPTIONS MakeOptions()
		{
			CGSH_Hardware::OPTIONS options;
			options.readbackFrames = true;
			options.frameSink = [this](std::vector<uint32>& pixels, uint32 w, uint32 h) {
				frame.pixels = pixels;
				frame.width = w;
				frame.height = h;
			};
			return options;
		}
		CHardwareGs m_gs;
	};

	void SetupDisplay(IDriver& gs)
	{
		gs.WritePriv(CGSHandler::GS_PMODE, 0x1);
		gs.WritePriv(CGSHandler::GS_DISPFB1, (FBW << 9));
		gs.WritePriv(CGSHandler::GS_DISPFB1 + 4, 0);
		uint64 display = (3ULL << 23) | (2559ULL << 32) | (447ULL << 44);
		gs.WritePriv(CGSHandler::GS_DISPLAY1, static_cast<uint32>(display));
		gs.WritePriv(CGSHandler::GS_DISPLAY1 + 4, static_cast<uint32>(display >> 32));
	}

	void Setup(IDriver& gs)
	{
		gs.Write(GS_REG_FRAME_1, Frame(0));
		gs.Write(GS_REG_ZBUF_1, 150 | (0ULL << 24) | (1ULL << 32));
		gs.Write(GS_REG_SCISSOR_1, Scissor(0, 639, 0, 447));
		gs.Write(GS_REG_XYOFFSET_1, (2048ULL << 4) | ((2048ULL << 4) << 32));
		gs.Write(GS_REG_TEST_1, 0);
		gs.Write(GS_REG_PRMODECONT, 1);
		gs.Write(GS_REG_COLCLAMP, 1); //games clamp; wrapping is not emulated on the GPU
		SetupDisplay(gs);
	}

	void Sprite(IDriver& gs, uint32 x0, uint32 y0, uint32 x1, uint32 y1, uint64 rgbaq, uint32 z = 0)
	{
		gs.Write(GS_REG_RGBAQ, rgbaq);
		gs.Write(GS_REG_XYZ2, Xyz(OffX(x0), OffX(y0), z));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(x1), OffX(y1), z));
	}

	void Clear(IDriver& gs, uint64 rgbaq)
	{
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);
		Sprite(gs, 0, 0, 640, 448, rgbaq);
	}

	uint64 St(float s, float t)
	{
		uint32 si, ti;
		std::memcpy(&si, &s, 4);
		std::memcpy(&ti, &t, 4);
		return static_cast<uint64>(si) | (static_cast<uint64>(ti) << 32);
	}

	uint64 RgbaqQ(uint8 r, uint8 g, uint8 b, uint8 a, float q)
	{
		uint32 qi;
		std::memcpy(&qi, &q, 4);
		return static_cast<uint64>(r) | (static_cast<uint64>(g) << 8) | (static_cast<uint64>(b) << 16) |
		       (static_cast<uint64>(a) << 24) | (static_cast<uint64>(qi) << 32);
	}

	// 64x64 CT32 checker/gradient texture at 'texPtr' (bytes), width 1 (64 px).
	void WriteTexture32(IDriver& gs, uint32 texPtr)
	{
		for(uint32 y = 0; y < 64; y++)
			for(uint32 x = 0; x < 64; x++)
			{
				uint32 checker = ((x / 8) ^ (y / 8)) & 1;
				uint32 color = checker ? (0x80000000 | (x * 4) | ((y * 4) << 8) | (0xC0 << 16))
				                       : (0x40000000 | 0x202020);
				GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMCT32, texPtr, 1, x, y, color);
			}
	}

	uint64 Tex0(uint32 texPtr, uint32 tbw, uint32 psm, uint32 logW, uint32 logH, bool tcc, uint32 tfx, uint32 cbp = 0, bool cld = false)
	{
		return static_cast<uint64>(texPtr / 256) | (static_cast<uint64>(tbw) << 14) | (static_cast<uint64>(psm) << 20) |
		       (static_cast<uint64>(logW) << 26) | (static_cast<uint64>(logH) << 30) | (static_cast<uint64>(tcc) << 34) |
		       (static_cast<uint64>(tfx) << 35) | (static_cast<uint64>(cbp / 256) << 37) | (static_cast<uint64>(cld ? 1 : 0) << 61);
	}

	//-------------------------------------------------------------------------
	// Scenes
	//-------------------------------------------------------------------------

	void SceneFlat(IDriver& gs)
	{
		Setup(gs);
		Clear(gs, Rgbaq(0x10, 0x20, 0x30, 0x80));
		Sprite(gs, 32, 32, 200, 100, Rgbaq(0xFF, 0x80, 0x00, 0x80));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_TRIANGLE | (1 << 3)); //gouraud
		gs.Write(GS_REG_RGBAQ, Rgbaq(0xFF, 0, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(320), OffX(40), 0));
		gs.Write(GS_REG_RGBAQ, Rgbaq(0, 0xFF, 0, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(600), OffX(400), 0));
		gs.Write(GS_REG_RGBAQ, Rgbaq(0, 0, 0xFF, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(100), OffX(300), 0));
		// Sub-pixel positions.
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_TRIANGLESTRIP);
		gs.Write(GS_REG_RGBAQ, Rgbaq(0xE0, 0xE0, 0x20, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(10) + 3, OffX(300) + 7, 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(90) + 11, OffX(310) + 1, 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(20) + 5, OffX(440) + 9, 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(80) + 13, OffX(430) + 2, 0));
		gs.Present();
	}

	void SceneDepth(IDriver& gs)
	{
		Setup(gs);
		gs.Write(GS_REG_ZBUF_1, 150 | (0ULL << 24) | (0ULL << 32)); //Z32 written
		// Clear Z to 0 with ZTST always.
		gs.Write(GS_REG_TEST_1, (1ULL << 16) | (1ULL << 17)); //ZTE, ALWAYS
		Clear(gs, Rgbaq(0, 0, 0, 0x80));
		gs.Write(GS_REG_TEST_1, (1ULL << 16) | (2ULL << 17)); //ZTE, GEQUAL
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_TRIANGLE);
		// Far triangle drawn last must lose against the near one.
		gs.Write(GS_REG_RGBAQ, Rgbaq(0xFF, 0x40, 0x40, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(100), OffX(50), 0x80000000));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(500), OffX(80), 0x80000000));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(300), OffX(400), 0x80000000));
		gs.Write(GS_REG_RGBAQ, Rgbaq(0x40, 0xFF, 0x40, 0x80));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(50), OffX(200), 0x10000000));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(600), OffX(150), 0x10000000));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(350), OffX(300), 0xF0000000));
		gs.Present();
	}

	void SceneBlend(IDriver& gs)
	{
		Setup(gs);
		Clear(gs, Rgbaq(0x80, 0x40, 0x20, 0x80));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE | (1 << 6)); //ABE
		// (Cs - Cd) * As + Cd
		gs.Write(GS_REG_ALPHA_1, 0 | (1 << 2) | (0 << 4) | (1 << 6));
		Sprite(gs, 20, 20, 300, 200, Rgbaq(0x00, 0x80, 0xFF, 0x40));
		// (Cs - 0) * FIX + Cd (additive)
		gs.Write(GS_REG_ALPHA_1, 0 | (2 << 2) | (2 << 4) | (1 << 6) | (0x40ULL << 32));
		Sprite(gs, 200, 100, 500, 300, Rgbaq(0x60, 0x20, 0x20, 0x80));
		// (Cd - Cs) * As + 0 ... uncommon; (Cs - Cd) * Ad + Cd
		gs.Write(GS_REG_ALPHA_1, 0 | (1 << 2) | (1 << 4) | (1 << 6));
		Sprite(gs, 350, 250, 600, 420, Rgbaq(0xFF, 0xFF, 0xFF, 0x80));
		gs.Present();
	}

	void SceneTexturedClut(IDriver& gs)
	{
		Setup(gs);
		Clear(gs, Rgbaq(0, 0, 0, 0x80));
		const uint32 texPtr = 0x200000;
		const uint32 clutPtr = 0x300000;
		for(uint32 y = 0; y < 16; y++)
			for(uint32 x = 0; x < 16; x++)
				GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMT4, texPtr, 1, x, y, (x + y) & 15);
		for(uint32 i = 0; i < 16; i++)
			GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMCT32, clutPtr, 1, i & 7, i >> 3, 0x80000000 | (i * 16) | ((255 - i * 16) << 8) | (0x40 << 16));

		gs.Write(GS_REG_TEX0_1, Tex0(texPtr, 1, CGSHandler::PSMT4, 4, 4, true, CGSHandler::TEX0_FUNCTION_DECAL, clutPtr, true));
		gs.Write(GS_REG_CLAMP_1, 0);
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE | (1 << 4) | (1 << 8)); //TME, FST
		gs.Write(GS_REG_RGBAQ, Rgbaq(0x80, 0x80, 0x80, 0x80));
		gs.Write(GS_REG_UV, Uv(0, 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(0), OffX(0), 0));
		gs.Write(GS_REG_UV, Uv(16 << 4, 16 << 4));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(128), OffX(128), 0));

		// Modulate with a vertex color, repeat wrap.
		gs.Write(GS_REG_TEX0_1, Tex0(texPtr, 1, CGSHandler::PSMT4, 4, 4, true, CGSHandler::TEX0_FUNCTION_MODULATE, clutPtr, false));
		gs.Write(GS_REG_RGBAQ, Rgbaq(0x40, 0x80, 0xC0, 0x80));
		gs.Write(GS_REG_UV, Uv(0, 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(200), OffX(0), 0));
		gs.Write(GS_REG_UV, Uv(64 << 4, 32 << 4));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(456), OffX(128), 0));
		gs.Present();
	}

	void ScenePerspective(IDriver& gs)
	{
		Setup(gs);
		Clear(gs, Rgbaq(0x20, 0x20, 0x20, 0x80));
		const uint32 texPtr = 0x200000;
		WriteTexture32(gs, texPtr);
		gs.Write(GS_REG_TEX0_1, Tex0(texPtr, 1, CGSHandler::PSMCT32, 6, 6, true, CGSHandler::TEX0_FUNCTION_MODULATE));
		gs.Write(GS_REG_TEX1_1, (1ULL << 5) | (1ULL << 6)); //bilinear
		gs.Write(GS_REG_CLAMP_1, 0);
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_TRIANGLESTRIP | (1 << 4)); //TME, STQ
		// A floor plane receding into the distance.
		struct
		{
			uint32 x, y;
			float s, t, q;
		} corners[4] = {
		    {40, 440, 0.0f, 1.0f, 1.0f},
		    {600, 440, 1.0f, 1.0f, 1.0f},
		    {250, 150, 0.0f, 0.0f, 0.25f},
		    {390, 150, 1.0f, 0.0f, 0.25f},
		};
		for(auto& c : corners)
		{
			gs.Write(GS_REG_ST, St(c.s * 4 * c.q, c.t * 4 * c.q));
			gs.Write(GS_REG_RGBAQ, RgbaqQ(0x80, 0x80, 0x80, 0x80, c.q));
			gs.Write(GS_REG_XYZ2, Xyz(OffX(c.x), OffX(c.y), 0));
		}
		gs.Present();
	}

	// Render into an off-screen buffer, then use it as a texture.
	void SceneRenderToTexture(IDriver& gs)
	{
		Setup(gs);
		Clear(gs, Rgbaq(0, 0, 0x40, 0x80));
		const uint32 rtPtr = 0x1C0000; //page 224
		const uint32 rtPages = rtPtr / 8192;
		gs.Write(GS_REG_FRAME_1, static_cast<uint64>(rtPages) | (4ULL << 16)); //256 wide
		gs.Write(GS_REG_SCISSOR_1, Scissor(0, 255, 0, 255));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);
		Sprite(gs, 0, 0, 256, 256, Rgbaq(0x80, 0x00, 0x00, 0x80));
		Sprite(gs, 64, 64, 192, 192, Rgbaq(0x00, 0xC0, 0x00, 0x80));

		gs.Write(GS_REG_FRAME_1, Frame(0));
		gs.Write(GS_REG_SCISSOR_1, Scissor(0, 639, 0, 447));
		gs.Write(GS_REG_TEX0_1, Tex0(rtPtr, 4, CGSHandler::PSMCT32, 8, 8, false, CGSHandler::TEX0_FUNCTION_MODULATE));
		gs.Write(GS_REG_TEX1_1, 0);
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE | (1 << 4) | (1 << 8));
		gs.Write(GS_REG_RGBAQ, Rgbaq(0x80, 0x80, 0x80, 0x80));
		gs.Write(GS_REG_UV, Uv(0, 0));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(100), OffX(100), 0));
		gs.Write(GS_REG_UV, Uv(256 << 4, 256 << 4));
		gs.Write(GS_REG_XYZ2, Xyz(OffX(356), OffX(356), 0));
		gs.Present();
	}

	// Render target sampled with its alpha (TCC) and used for blending.
	void SceneRenderTargetAlpha(IDriver& gs)
	{
		Setup(gs);
		Clear(gs, Rgbaq(0xC0, 0xC0, 0xC0, 0x80));
		const uint32 rtPtr = 0x1C0000;
		gs.Write(GS_REG_FRAME_1, static_cast<uint64>(rtPtr / 8192) | (4ULL << 16));
		gs.Write(GS_REG_SCISSOR_1, Scissor(0, 255, 0, 255));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);
		Sprite(gs, 0, 0, 128, 256, Rgbaq(0xFF, 0x00, 0x00, 0x20));
		Sprite(gs, 128, 0, 256, 256, Rgbaq(0x00, 0x00, 0xFF, 0x70));

		gs.Write(GS_REG_FRAME_1, Frame(0));
		gs.Write(GS_REG_SCISSOR_1, Scissor(0, 639, 0, 447));
		gs.Write(GS_REG_ALPHA_1, 0 | (1 << 2) | (0 << 4) | (1 << 6));
		gs.Write(GS_REG_TEX1_1, 0);
		for(uint32 tfx : {CGSHandler::TEX0_FUNCTION_MODULATE, CGSHandler::TEX0_FUNCTION_DECAL})
		{
			uint32 x = (tfx == CGSHandler::TEX0_FUNCTION_DECAL) ? 320 : 20;
			gs.Write(GS_REG_TEX0_1, Tex0(rtPtr, 4, CGSHandler::PSMCT32, 8, 8, true, tfx));
			gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE | (1 << 4) | (1 << 6) | (1 << 8));
			gs.Write(GS_REG_RGBAQ, Rgbaq(0x80, 0x80, 0x80, 0x80));
			gs.Write(GS_REG_UV, Uv(0, 0));
			gs.Write(GS_REG_XYZ2, Xyz(OffX(x), OffX(50), 0));
			gs.Write(GS_REG_UV, Uv(256 << 4, 256 << 4));
			gs.Write(GS_REG_XYZ2, Xyz(OffX(x + 256), OffX(306), 0));
		}
		gs.Present();
	}

	// Typical game memory layout: a 640x448 frame immediately followed by
	// textures that are re-uploaded and sampled while the frame is drawn.
	void SceneTexturesAfterFrame(IDriver& gs)
	{
		Setup(gs);
		Clear(gs, Rgbaq(0x10, 0x10, 0x30, 0x80));
		const uint32 texPtr = 140 * 8192; //first page after 448 lines of a 640 wide CT32 frame
		for(uint32 round = 0; round < 3; round++)
		{
			std::vector<uint32> image(64 * 64);
			for(uint32 i = 0; i < image.size(); i++) image[i] = 0x80000000 | ((i * (round + 3)) & 0xFF) | ((round * 0x50) << 8);
			uint64 bitbltbuf = (static_cast<uint64>(texPtr / 256) << 32) | (1ULL << 48) | (static_cast<uint64>(CGSHandler::PSMCT32) << 56);
			gs.Write(GS_REG_BITBLTBUF, bitbltbuf);
			gs.Write(GS_REG_TRXPOS, 0);
			gs.Write(GS_REG_TRXREG, 64 | (64ULL << 32));
			gs.Write(GS_REG_TRXDIR, 0);
			gs.Transfer(reinterpret_cast<const uint8*>(image.data()), static_cast<uint32>(image.size() * 4));

			gs.Write(GS_REG_TEX0_1, Tex0(texPtr, 1, CGSHandler::PSMCT32, 6, 6, true, CGSHandler::TEX0_FUNCTION_DECAL));
			gs.Write(GS_REG_TEX1_1, 0);
			gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE | (1 << 4) | (1 << 8));
			gs.Write(GS_REG_UV, Uv(0, 0));
			gs.Write(GS_REG_XYZ2, Xyz(OffX(50 + round * 150), OffX(50), 0));
			gs.Write(GS_REG_UV, Uv(64 << 4, 64 << 4));
			gs.Write(GS_REG_XYZ2, Xyz(OffX(178 + round * 150), OffX(178), 0));
		}
		gs.Present();
	}

	// Rendering, then a host->local upload over part of the frame, then more
	// rendering on top: GPU and memory copies must stay coherent.
	void SceneTransfer(IDriver& gs)
	{
		Setup(gs);
		Clear(gs, Rgbaq(0x30, 0x60, 0x90, 0x80));
		std::vector<uint32> image(64 * 32);
		for(uint32 i = 0; i < image.size(); i++) image[i] = 0x80000000 | ((i * 7) & 0xFF) | (((i * 3) & 0xFF) << 8);
		uint64 bitbltbuf = (static_cast<uint64>(0) << 32) | (static_cast<uint64>(FBW) << 48) | (static_cast<uint64>(CGSHandler::PSMCT32) << 56);
		gs.Write(GS_REG_BITBLTBUF, bitbltbuf);
		gs.Write(GS_REG_TRXPOS, (static_cast<uint64>(100) << 32) | (static_cast<uint64>(60) << 48));
		gs.Write(GS_REG_TRXREG, 64 | (32ULL << 32));
		gs.Write(GS_REG_TRXDIR, 0);
		gs.Transfer(reinterpret_cast<const uint8*>(image.data()), static_cast<uint32>(image.size() * 4));
		gs.Write(GS_REG_PRIM, CGSHandler::PRIM_SPRITE);
		Sprite(gs, 140, 80, 200, 120, Rgbaq(0xFF, 0xFF, 0x00, 0x80));
		gs.Present();
	}

	// Random states and primitives within what the GPU renderer supports
	// (no fog, no region clamp, alpha test fail mode KEEP).
	void SceneRandom(IDriver& gs, uint32 seed)
	{
		std::mt19937 rng(seed);
		auto rand = [&](uint32 n) { return rng() % n; };
		Setup(gs);
		gs.Write(GS_REG_ZBUF_1, 150 | (0ULL << 24) | (0ULL << 32));
		gs.Write(GS_REG_TEST_1, (1ULL << 16) | (1ULL << 17));
		Clear(gs, Rgbaq(rand(256), rand(256), rand(256), 0x80));

		// Above the Z buffer (page 150, 640x448x32 bits ends at 0x244000).
		const uint32 texPtr = 0x280000, clutPtr = 0x300000;
		WriteTexture32(gs, texPtr);
		for(uint32 y = 0; y < 64; y++)
			for(uint32 x = 0; x < 64; x++)
				GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMT8, 0x2C0000, 1, x, y, (x * 3 + y * 5) & 0xFF);
		for(uint32 i = 0; i < 256; i++)
			GsMemory::WriteRaw(gs.Ram(), CGSHandler::PSMCT32, clutPtr, 1, i & 15, i >> 4, ((i * 0x9E3779B1) & 0x00FFFFFF) | ((i & 0x7F) << 24));

		// HW_RANDOM_NO=<letters> disables features to isolate differences:
		// b=blending a=alpha test z=depth test (ALWAYS) t=texturing p=perspective f=fog r=region wrap modes
		const char* disabled = std::getenv("HW_RANDOM_NO");
		auto off = [&](char c) { return disabled && std::strchr(disabled, c); };
		const char* env = std::getenv("HW_RANDOM_BATCHES");
		uint32 batches = env ? std::atoi(env) : 12;
		for(uint32 batch = 0; batch < batches; batch++)
		{
			uint32 ztst = 1 + rand(3);
			if(off('z')) ztst = 1;
			uint64 test = (1ULL << 16) | (static_cast<uint64>(ztst) << 17);
			if((rand(3) == 0) && !off('a')) test |= 1 | (static_cast<uint64>(rand(8)) << 1) | (static_cast<uint64>(rand(256)) << 4); //ATE, ATST, AREF, AFAIL=KEEP
			gs.Write(GS_REG_TEST_1, test);
			uint32 blendA = rand(3), blendB = rand(3), blendC = rand(3), blendD = rand(3);
			if((test & 1) && (blendC == 2)) blendC = rand(2); //FIX + alpha test: unsupported combination
			while(!CGSH_Hardware::IsBlendExact(blendA, blendB, blendD)) blendD = rand(3);
			if(std::getenv("HW_NO_FIX") && (blendC == 2)) blendC = rand(2);
			gs.Write(GS_REG_ALPHA_1, blendA | (blendB << 2) | (blendC << 4) | (blendD << 6) | (static_cast<uint64>(rand(129)) << 32));
			bool textured = (rand(3) != 0) && !off('t');
			bool indexed = rand(2);
			uint32 tfx = rand(2) ? CGSHandler::TEX0_FUNCTION_MODULATE : CGSHandler::TEX0_FUNCTION_DECAL;
			bool tcc = rand(2);
			bool bilinear = rand(2);
			gs.Write(GS_REG_TEX0_1, indexed ? Tex0(0x2C0000, 1, CGSHandler::PSMT8, 6, 6, tcc, tfx, clutPtr, true)
			                                : Tex0(texPtr, 1, CGSHandler::PSMCT32, 6, 6, tcc, tfx));
			gs.Write(GS_REG_TEX1_1, bilinear ? ((1ULL << 5) | (1ULL << 6)) : 0);
			// Wrap modes: repeat, clamp, region clamp, region repeat (power
			// of two mask with an aligned offset, the form games use).
			auto wrapAxis = [&](uint64& mode, uint64& minValue, uint64& maxValue) {
				mode = off('r') ? rand(2) : rand(4);
				minValue = maxValue = 0;
				if(mode == 2)
				{
					minValue = rand(48);
					maxValue = minValue + rand(16);
				}
				else if(mode == 3)
				{
					uint32 k = 2 + rand(4);
					minValue = (1u << k) - 1;
					maxValue = rand(64 >> k) << k;
				}
			};
			uint64 wms, minu, maxu, wmt, minv, maxv;
			wrapAxis(wms, minu, maxu);
			wrapAxis(wmt, minv, maxv);
			gs.Write(GS_REG_CLAMP_1, wms | (wmt << 2) | (minu << 4) | (maxu << 14) | (minv << 24) | (maxv << 34));
			bool sprite = rand(3) == 0;
			bool fst = sprite || rand(2) || off('p');
			bool abe = rand(2) && !off('b');
			bool fog = (rand(3) == 0) && !off('f');
			gs.Write(GS_REG_FOGCOL, rng() & 0xFFFFFF);
			uint64 prim = (sprite ? CGSHandler::PRIM_SPRITE : CGSHandler::PRIM_TRIANGLE) | (rand(2) << 3) | (textured << 4) |
			              (fog << 5) | (abe << 6) | (fst << 8);
			gs.Write(GS_REG_PRIM, prim);
			if(std::getenv("HW_RANDOM_VERBOSE") && (dynamic_cast<CHardwareDriver*>(&gs) != nullptr))
				std::printf("    seed %u: %s tex=%d idx=%d tfx=%u tcc=%d bil=%d fst=%d abe=%d ABCD=%u%u%u%u ate=%d atst=%u aref=%u ztst=%u iip=%d\n",
				            seed, sprite ? "sprite" : "tri", textured, indexed, tfx, int(tcc), int(bilinear), fst, int((prim >> 6) & 1),
				            blendA, blendB, blendC, blendD, int(test & 1), unsigned((test >> 1) & 7), unsigned((test >> 4) & 0xFF), ztst, int((prim >> 3) & 1));
			uint32 count = sprite ? 2 * (1 + rand(3)) : 3 * (1 + rand(3));
			for(uint32 v = 0; v < count; v++)
			{
				// Gouraud colors are interpolated perspective correctly on the GPU
				// (the GS interpolates them affinely): keep Q ratios realistic.
				bool gouraud = (prim >> 3) & 1;
				float q = fst ? 1.0f : (gouraud || fog) ? 0.96f + rand(100) / 1250.0f : 0.5f + rand(100) / 100.0f;
				gs.Write(GS_REG_RGBAQ, RgbaqQ(rand(256), rand(256), rand(256), rand(0x81), q));
				uint32 u = rand(64 * 16), vv = rand(64 * 16);
				float sCoord = rand(100) / 100.0f, tCoord = rand(100) / 100.0f;
				if(fst) gs.Write(GS_REG_UV, Uv(u, vv));
				else gs.Write(GS_REG_ST, St(sCoord * q, tCoord * q));
				// Distinct at 24-bit depth precision.
				uint32 x = rand(640), y = rand(448);
				gs.Write(GS_REG_FOG, static_cast<uint64>(rand(256)) << 56);
				gs.Write(GS_REG_XYZ2, Xyz(OffX(x), OffX(y), rand(1 << 16) << 16));
				if(std::getenv("HW_RANDOM_VERTICES") && (dynamic_cast<CHardwareDriver*>(&gs) != nullptr))
					std::printf("      v%u: xy %u,%u uv %.2f,%.2f st %.2f,%.2f q %.2f\n", v, x, y, u / 16.0f, vv / 16.0f, sCoord, tCoord, q);
			}
		}
		gs.Present();
	}

	//-------------------------------------------------------------------------

	struct SCENE
	{
		const char* name;
		std::function<void(IDriver&)> run;
		double maxMismatchPercent;
	};

	bool CompareFrames(const char* name, const FRAME& reference, const FRAME& gpu, double maxMismatchPercent)
	{
		if((reference.width != gpu.width) || (reference.height != gpu.height) || reference.pixels.empty())
		{
			std::printf("  FAIL %s: frame size %ux%u vs %ux%u\n", name, reference.width, reference.height, gpu.width, gpu.height);
			return false;
		}
		const uint32 tolerance = 6;
		uint32 mismatches = 0;
		uint32 firstX = 0, firstY = 0, firstRef = 0, firstGpu = 0;
		for(uint32 i = 0; i < reference.pixels.size(); i++)
		{
			uint32 a = reference.pixels[i], b = gpu.pixels[i];
			for(uint32 shift = 0; shift < 24; shift += 8)
			{
				int32 d = static_cast<int32>((a >> shift) & 0xFF) - static_cast<int32>((b >> shift) & 0xFF);
				if(static_cast<uint32>(std::abs(d)) > tolerance)
				{
					if(mismatches == 0)
					{
						firstX = i % reference.width;
						firstY = i / reference.width;
						firstRef = a;
						firstGpu = b;
					}
					mismatches++;
					break;
				}
			}
		}
		double percent = 100.0 * mismatches / reference.pixels.size();
		bool ok = percent <= maxMismatchPercent;
		std::printf("  %s %-20s %6u pixels differ (%.3f%%, limit %.2f%%)", ok ? "ok  " : "FAIL", name, mismatches, percent, maxMismatchPercent);
		if(mismatches) std::printf(", first at (%u,%u): %08X vs %08X", firstX, firstY, firstRef, firstGpu);
		std::printf("\n");
		return ok;
	}

	void WritePpm(const std::string& path, const FRAME& frame)
	{
		FILE* file = std::fopen(path.c_str(), "wb");
		if(!file) return;
		std::fprintf(file, "P6\n%u %u\n255\n", frame.width, frame.height);
		for(uint32 pixel : frame.pixels)
		{
			uint8 rgb[3] = {static_cast<uint8>(pixel), static_cast<uint8>(pixel >> 8), static_cast<uint8>(pixel >> 16)};
			std::fwrite(rgb, 1, 3, file);
		}
		std::fclose(file);
	}
}

int main(int argc, char** argv)
{
	CEmuSession::SetDataPaths("vitaps2_test_data", ".");
	const char* dumpDir = (argc > 1) ? argv[1] : nullptr;

	const int attribs[] = {OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_PROFILE, OSMESA_COMPAT_PROFILE, 0};
	OSMesaContext context = OSMesaCreateContextAttribs(attribs, nullptr);
	if(!context)
	{
		std::printf("could not create an OSMesa context\n");
		return 1;
	}
	std::vector<uint32> windowBuffer(16 * 16);
	if(!OSMesaMakeCurrent(context, windowBuffer.data(), GL_UNSIGNED_BYTE, 16, 16))
	{
		std::printf("OSMesaMakeCurrent failed\n");
		return 1;
	}
	std::printf("GL renderer: %s\n", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

	const SCENE scenes[] = {
	    {"flat/gouraud", SceneFlat, 0.5},
	    {"depth", SceneDepth, 0.5},
	    {"blend", SceneBlend, 0.5},
	    {"textured clut", SceneTexturedClut, 0.5},
	    {"perspective", ScenePerspective, 2.0},
	    {"render to texture", SceneRenderToTexture, 0.5},
	    {"render target alpha", SceneRenderTargetAlpha, 0.5},
	    {"transfer", SceneTransfer, 0.5},
	    {"textures after frame", SceneTexturesAfterFrame, 0.5},
	};
	std::vector<SCENE> allScenes(std::begin(scenes), std::end(scenes));
	static std::vector<std::string> randomNames;
	const uint32 randomCount = (argc > 2) ? std::atoi(argv[2]) : 20;
	randomNames.reserve(randomCount);
	for(uint32 i = 0; i < randomCount; i++)
	{
		randomNames.push_back("random " + std::to_string(i));
		allScenes.push_back({randomNames.back().c_str(), [i](IDriver& gs) { SceneRandom(gs, 1000 + i); }, 3.0});
	}

	int failures = 0;
	const char* only = std::getenv("HW_ONLY");
	for(const auto& scene : allScenes)
	{
		if(only && std::strcmp(only, scene.name)) continue;
		CSoftwareDriver software;
		scene.run(software);
		CHardwareDriver hardware;
		scene.run(hardware);
		if(glGetError() != GL_NO_ERROR)
		{
			std::printf("  FAIL %s: GL error\n", scene.name);
			failures++;
		}
		if(!hardware.PresentedRenderTarget())
		{
			std::printf("  FAIL %s: display was not served from a render target\n", scene.name);
			failures++;
		}
		if(!CompareFrames(scene.name, software.frame, hardware.frame, scene.maxMismatchPercent)) failures++;
		if(!std::strcmp(scene.name, "textures after frame"))
		{
			// The frame must never round-trip through GS memory, and texture
			// uploads must not re-upload it.
			auto stats = hardware.Stats();
			std::printf("       readback %u px, upload %u px\n", stats.downloadedPixels, stats.uploadedPixels);
			if((stats.downloadedPixels != 0) || (stats.uploadedPixels > 640 * 448))
			{
				std::printf("  FAIL %s: unexpected GS memory synchronization\n", scene.name);
				failures++;
			}
		}
		if(dumpDir)
		{
			std::string name = scene.name;
			for(auto& c : name)
				if(c == ' ' || c == '/') c = '_';
			std::string base = std::string(dumpDir) + "/" + name;
			WritePpm(base + "_sw.ppm", software.frame);
			WritePpm(base + "_hw.ppm", hardware.frame);
		}
	}

	OSMesaDestroyContext(context);
	std::printf("%s (%d failing scenes)\n", failures ? "FAILED" : "PASSED", failures);
	return failures ? 1 : 0;
}
