#pragma once

// Portable software implementation of the PS2 Graphics Synthesizer pixel
// pipeline. It renders straight into the emulated 4MB GS local memory, which
// makes it inherently accurate for render-to-texture, framebuffer feedback and
// GS memory readback. It has no dependency on any GPU API, so the exact same
// code runs on the Vita and in the host test harness.
//
// Performance design (the Vita's Cortex-A9 is the target):
//  - Triangles are walked as scanline spans whose extents are solved exactly
//    with integer edge equations (no per-pixel coverage tests).
//  - Attributes are stepped incrementally in fixed point.
//  - The span loop is a template specialized on the expensive state
//    (texturing, filtering, depth mode, framebuffer format, blending) and
//    selected once per state change.
//  - Swizzled addressing uses precomputed page tables (GS_SURFACE).
//  - Textures are sampled from a lazily decoded RGBA cache (CTextureCache).

#include <array>
#include "Types.h"
#include "GsSurface.h"
#include "TextureCache.h"

class CSoftwareRasterizer
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

	CSoftwareRasterizer();

	void SetMemory(uint8* ram, const uint16* clut);
	void SetState(const STATE&);

	// Must be called whenever the CLUT buffer contents may have changed.
	void SetClutHash(uint64 hash);
	// Must be called for writes to GS memory done outside of this class.
	void NotifyMemoryWrite(uint32 start, uint32 size);
	void NotifyAllMemoryWritten();

	// Interlace-style rendering: only rasterize rows where (y & mask) == value.
	// mask = 0 renders everything.
	void SetRowFilter(uint32 mask, uint32 value);

	void DrawPoint(const VERTEX&);
	void DrawLine(const VERTEX&, const VERTEX&);
	void DrawTriangle(const VERTEX&, const VERTEX&, const VERTEX&);
	void DrawSprite(const VERTEX&, const VERTEX&);

	// Converts any color PSM to RGBA8888 for display/readback.
	static uint32 ReadColor32(uint8* ram, uint32 psm, uint32 bufPtr, uint32 bufWidth, uint32 x, uint32 y);

	CTextureCache& GetTextureCache()
	{
		return m_textureCache;
	}

	struct INTERP
	{
		int32 r, g, b, a, f;      //16.16
		int32 dr, dg, db, da, df; //per pixel
		int64 z, dz;              //32.16
		int32 u, v, du, dv;       //16.16 texel coordinates (fst)
		float s, t, q, ds, dt, dq;
	};

private:
	typedef void (CSoftwareRasterizer::*SpanFunction)(int32, int32, int32, INTERP&);

	template <bool TEXTURED, bool BILINEAR, int ZMODE, int FMT, bool BLEND, bool STQ>
	void DrawSpan(int32 y, int32 x0, int32 x1, INTERP&);

	template <bool T, bool BL, bool STQ, int Z, int F>
	static SpanFunction SelectBlend(bool);
	template <bool T, bool BL, bool STQ, int Z>
	static SpanFunction SelectFormat(int, bool);
	template <bool T, bool BL, bool STQ>
	static SpanFunction SelectDepth(int, int, bool);
	void SelectSpanFunction();

	void DrawPixel(int32 x, int32 y, INTERP&, bool linear);
	INTERP MakeConstantInterp(uint32 r, uint32 g, uint32 b, uint32 a, uint32 fog, uint32 z) const;
	bool EnsureTexture();
	void MarkWritten(int32 x0, int32 y0, int32 x1, int32 y1);
	bool RowEnabled(int32 y) const
	{
		return (static_cast<uint32>(y) & m_rowMask) == m_rowValue;
	}


	uint8* m_ram = nullptr;
	const uint16* m_clut = nullptr;
	STATE m_state;
	GS_SURFACE m_frame;
	GS_SURFACE m_depth;
	bool m_frameFast = false;
	bool m_depthFast = false;
	bool m_depth16 = false;
	uint32 m_zMax = 0xFFFFFFFF;
	uint32 m_zpsmFull = 0x30;
	bool m_drawNothing = false;

	SpanFunction m_span = nullptr;       //triangles (perspective correct when STQ)
	SpanFunction m_spanLinear = nullptr; //sprites, lines, points (linear texel coordinates)

	CTextureCache m_textureCache;
	CTextureCache::KEY m_textureKey;
	CTextureCache::CTexture* m_texture = nullptr;
	const uint32* m_texels = nullptr; //set when the whole texture is decoded
	uint32 m_texelsGeneration = 0;
	uint64 m_clutHash = 0;

	uint32 m_rowMask = 0;
	uint32 m_rowValue = 0;
};
