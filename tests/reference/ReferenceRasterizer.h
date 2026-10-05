#pragma once

// Original straightforward per-pixel implementation of the GS pipeline. It is
// slow but simple, and serves as the oracle for the optimized rasterizer in
// the differential tests.

// Portable software implementation of the PS2 Graphics Synthesizer pixel
// pipeline. It renders straight into the emulated 4MB GS local memory, which
// makes it inherently accurate for render-to-texture, framebuffer feedback and
// GS memory readback. It has no dependency on any GPU API, so the exact same
// code runs on the Vita and in the host test harness.

#include <array>
#include "Types.h"

class CReferenceRasterizer
{
public:
	struct VERTEX
	{
		int32 x, y; //12.4 fixed point, XYOFFSET already applied
		uint32 z;
		uint8 r, g, b, a;
		uint8 fog;
		float s, t, q; //STQ coordinates (used when !fst)
		float u, v;    //Texel coordinates (used when fst)
	};

	// Snapshot of every register that influences rasterization of one primitive.
	struct STATE
	{
		//PRIM
		bool gouraud = false;
		bool textured = false;
		bool fog = false;
		bool alphaBlend = false;
		bool fst = false;

		//FRAME / ZBUF
		uint32 fbp = 0;  //bytes
		uint32 fbw = 0;  //units of 64 pixels
		uint32 fpsm = 0;
		uint32 fbmsk = 0;
		uint32 zbp = 0;
		uint32 zpsm = 0;
		bool zmsk = false;

		//SCISSOR (pixels, inclusive)
		int32 scax0 = 0, scax1 = 0, scay0 = 0, scay1 = 0;

		//TEST
		bool ate = false;
		uint32 atst = 0;
		uint32 aref = 0;
		uint32 afail = 0;
		bool date = false;
		bool datm = false;
		bool zte = false;
		uint32 ztst = 1;

		//ALPHA / PABE / FBA / COLCLAMP / FOGCOL
		uint32 blendA = 0, blendB = 0, blendC = 0, blendD = 0;
		uint32 blendFix = 0;
		bool pabe = false;
		bool fba = false;
		bool colClamp = true;
		uint32 fogColor = 0;

		//TEX0 / TEX1 / CLAMP / TEXA
		uint32 tbp = 0;
		uint32 tbw = 0;
		uint32 tpsm = 0;
		uint32 tw = 1, th = 1; //texture size (pixels)
		bool tcc = false;
		uint32 tfx = 0;
		uint32 cpsm = 0;
		uint32 csa = 0;
		bool bilinear = false;
		uint32 wms = 0, wmt = 0;
		uint32 minu = 0, maxu = 0, minv = 0, maxv = 0;
		uint32 ta0 = 0, ta1 = 0;
		bool aem = false;
	};

	void SetMemory(uint8* ram, const uint16* clut);
	void SetState(const STATE&);

	void DrawPoint(const VERTEX&);
	void DrawLine(const VERTEX&, const VERTEX&);
	void DrawTriangle(const VERTEX&, const VERTEX&, const VERTEX&);
	void DrawSprite(const VERTEX&, const VERTEX&);

	uint32 GetPixelsWritten() const
	{
		return m_pixelsWritten;
	}

	// Converts any color PSM to RGBA8888 for display/readback.
	static uint32 ReadColor32(uint8* ram, uint32 psm, uint32 bufPtr, uint32 bufWidth, uint32 x, uint32 y);

private:
	struct FRAGMENT
	{
		uint32 r, g, b, a;
		uint32 z;
		uint32 fog;
		float u, v; //texel space
	};

	void ShadePixel(int32 x, int32 y, FRAGMENT&);
	uint32 SampleTexture(float u, float v);
	uint32 FetchTexel(int32 u, int32 v);
	uint32 LookupClut(uint32 index) const;
	uint32 ExpandAlpha16(uint32 color16) const;
	int32 WrapCoord(int32 coord, uint32 size, uint32 mode, uint32 minc, uint32 maxc) const;

	uint8* m_ram = nullptr;
	const uint16* m_clut = nullptr;
	STATE m_state;
	uint32 m_pixelsWritten = 0;
};
