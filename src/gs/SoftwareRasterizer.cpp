#include "SoftwareRasterizer.h"
#include "GsMemory.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace
{
	inline uint32 Clamp255(int32 value)
	{
		return static_cast<uint32>(std::clamp<int32>(value, 0, 255));
	}

	inline int64 EdgeFunction(int32 ax, int32 ay, int32 bx, int32 by, int32 px, int32 py)
	{
		return static_cast<int64>(bx - ax) * static_cast<int64>(py - ay) -
		       static_cast<int64>(by - ay) * static_cast<int64>(px - ax);
	}

	// Top-left fill convention for an edge a->b whose interior is where E >= 0.
	inline bool IsTopLeft(int32 ax, int32 ay, int32 bx, int32 by)
	{
		int32 dy = by - ay;
		int32 dx = bx - ax;
		return (dy < 0) || ((dy == 0) && (dx > 0));
	}

	// First covered pixel for a 12.4 fixed point coordinate.
	inline int32 CeilFixed(int32 value)
	{
		return (value + 15) >> 4;
	}

	inline int64 FloorDiv(int64 n, int64 d) //d > 0
	{
		return (n >= 0) ? (n / d) : -((-n + d - 1) / d);
	}

	inline int64 CeilDiv(int64 n, int64 d) //d > 0
	{
		return -FloorDiv(-n, d);
	}

	inline uint32 ZMaxForPsm(uint32 zpsm)
	{
		switch(zpsm & 0xF)
		{
		case 0x0: //Z32
			return 0xFFFFFFFF;
		case 0x1: //Z24
			return 0x00FFFFFF;
		default: //Z16, Z16S
			return 0x0000FFFF;
		}
	}

	inline int32 ToFixed(double value)
	{
		return static_cast<int32>(std::floor(value * 65536.0));
	}

	inline int32 FloatToFixedTexel(float value)
	{
		value = std::clamp(value, -32767.0f, 32767.0f);
		return static_cast<int32>(value * 65536.0f);
	}

	inline uint32 ClampZ(int64 zFixed)
	{
		if(zFixed <= 0) return 0;
		int64 z = zFixed >> 16;
		return (z >= 0xFFFFFFFFLL) ? 0xFFFFFFFF : static_cast<uint32>(z);
	}

	inline int32 WrapCoord(int32 coord, uint32 size, uint32 mode, uint32 minc, uint32 maxc)
	{
		switch(mode)
		{
		default:
		case CGSHandler::CLAMP_MODE_REPEAT:
			return coord & static_cast<int32>(size - 1);
		case CGSHandler::CLAMP_MODE_CLAMP:
			return std::clamp<int32>(coord, 0, static_cast<int32>(size) - 1);
		case CGSHandler::CLAMP_MODE_REGION_CLAMP:
			return std::clamp<int32>(coord, static_cast<int32>(minc), static_cast<int32>(maxc));
		case CGSHandler::CLAMP_MODE_REGION_REPEAT:
			return (coord & static_cast<int32>(minc)) | static_cast<int32>(maxc);
		}
	}

	// Linear interpolation of two RGBA8888 colors, two channels per operation.
	inline uint32 Lerp2(uint32 a, uint32 b, uint32 w)
	{
		uint32 ag = (a >> 8) & 0x00FF00FF, rb = a & 0x00FF00FF;
		uint32 bg = (b >> 8) & 0x00FF00FF, bb = b & 0x00FF00FF;
		uint32 lo = ((rb * (256 - w) + bb * w) >> 8) & 0x00FF00FF;
		uint32 hi = ((ag * (256 - w) + bg * w) >> 8) & 0x00FF00FF;
		return lo | (hi << 8);
	}

	// Edge inequality A*x + K >= 0 restricts x to [lo, hi].
	inline void ClipSpanToEdge(int64 a, int64 k, int64& lo, int64& hi)
	{
		if(a > 0)
		{
			lo = std::max(lo, CeilDiv(-k, a));
		}
		else if(a < 0)
		{
			hi = std::min(hi, FloorDiv(k, -a));
		}
		else if(k < 0)
		{
			lo = 1;
			hi = 0;
		}
	}
}

CSoftwareRasterizer::CSoftwareRasterizer()
{
	GS_SURFACE::InitTables();
	SelectSpanFunction();
}

void CSoftwareRasterizer::SetMemory(uint8* ram, const uint16* clut)
{
	m_ram = ram;
	m_clut = clut;
	m_textureCache.SetMemory(ram, clut);
	m_texture = nullptr;
	m_texels = nullptr;
}

void CSoftwareRasterizer::SetClutHash(uint64 hash)
{
	if(hash == m_clutHash) return;
	m_clutHash = hash;
	if(CGsPixelFormats::IsPsmIDTEX(m_state.tpsm))
	{
		m_textureKey.clutHash = hash;
		m_texture = nullptr;
		m_texels = nullptr;
	}
}

void CSoftwareRasterizer::NotifyMemoryWrite(uint32 start, uint32 size)
{
	m_textureCache.MarkBytesWritten(start, size);
}

void CSoftwareRasterizer::NotifyAllMemoryWritten()
{
	m_textureCache.MarkAllWritten();
}

void CSoftwareRasterizer::SetRowFilter(uint32 mask, uint32 value)
{
	m_rowMask = mask;
	m_rowValue = value & mask;
}

void CSoftwareRasterizer::SetState(const STATE& state)
{
	m_state = state;
	const auto& s = m_state;

	m_frameFast = m_frame.Init(s.fpsm, s.fbp, s.fbw);
	m_zpsmFull = 0x30 | (s.zpsm & 0xF);
	m_depthFast = m_depth.Init(m_zpsmFull, s.zbp, s.fbw);
	m_depth16 = GsMemory::IsPsm16(m_zpsmFull);
	m_zMax = ZMaxForPsm(s.zpsm);
	m_drawNothing = s.zte && (s.ztst == CGSHandler::DEPTH_TEST_NEVER);

	if(s.textured)
	{
		CTextureCache::KEY key;
		key.tbp = s.tbp;
		key.tbw = s.tbw;
		key.tpsm = s.tpsm;
		key.tw = s.tw;
		key.th = s.th;
		bool indexed = CGsPixelFormats::IsPsmIDTEX(s.tpsm);
		key.cpsm = indexed ? s.cpsm : 0;
		key.csa = indexed ? s.csa : 0;
		key.clutHash = indexed ? m_clutHash : 0;
		key.texa = s.ta0 | (s.ta1 << 8) | ((s.aem ? 1 : 0) << 16);
		if(!m_texture || !(key == m_textureKey))
		{
			m_textureKey = key;
			m_texture = nullptr;
			m_texels = nullptr;
		}
	}

	SelectSpanFunction();
}

bool CSoftwareRasterizer::EnsureTexture()
{
	if(m_laneMode) return true;
	if(!m_state.textured) return true;
	if(!m_texture || (m_texture->GetDecodeStamp() != m_textureCache.GetStamp()))
	{
		auto texture = m_textureCache.Get(m_textureKey);
		if((texture != m_texture) || (texture->GetGeneration() != m_texelsGeneration))
		{
			m_texels = nullptr;
		}
		m_texture = texture;
	}
	if(!m_texture) return false;
	// Small textures are decoded whole so spans can index texels directly.
	if(!m_texels && (m_state.tw * m_state.th <= 256 * 256))
	{
		m_texture->DecodeAll();
		m_texels = m_texture->Texels();
		m_texelsGeneration = m_texture->GetGeneration();
	}
	return true;
}

void CSoftwareRasterizer::MarkWritten(int32 x0, int32 y0, int32 x1, int32 y1)
{
	if(m_laneMode) return;
	const auto& s = m_state;
	auto markSurface = [&](const GS_SURFACE& surface, bool fast) {
		if(!fast)
		{
			m_textureCache.MarkAllWritten();
			return;
		}
		uint32 rowFirst = static_cast<uint32>(y0) >> surface.phShift;
		uint32 rowLast = static_cast<uint32>(y1) >> surface.phShift;
		uint32 colFirst = static_cast<uint32>(x0) >> surface.pwShift;
		uint32 colLast = static_cast<uint32>(x1) >> surface.pwShift;
		for(uint32 row = rowFirst; row <= rowLast; row++)
		{
			uint32 first = surface.FirstPage() + row * surface.pagesPerRow + colFirst;
			uint32 count = colLast - colFirst + 1;
			m_textureCache.MarkPagesWritten(first, count);
			for(uint32 i = 0; i < count && i < 512; i++)
			{
				uint32 page = (first + i) & 511;
				m_pendingPages[page >> 5] |= 1u << (page & 31);
			}
		}
	};
	markSurface(m_frame, m_frameFast);
	if(s.zte && !s.zmsk) markSurface(m_depth, m_depthFast);
}

uint32 CSoftwareRasterizer::ReadColor32(uint8* ram, uint32 psm, uint32 bufPtr, uint32 bufWidth, uint32 x, uint32 y)
{
	uint32 raw = GsMemory::ReadRaw(ram, psm, bufPtr, bufWidth, x, y);
	if(GsMemory::IsPsm16(psm))
	{
		return GsMemory::Color16To32(raw);
	}
	if(GsMemory::IsPsm24(psm))
	{
		return raw | 0xFF000000;
	}
	return raw;
}

//-----------------------------------------------------------------------------
// Texturing
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Span pipeline
//-----------------------------------------------------------------------------

// ZMODE: 0 = depth test disabled, 1 = ALWAYS, 2 = GEQUAL/GREATER
// FMT:   0 = CT32, 1 = CT24, 2 = CT16/CT16S, 3 = anything else (generic access)
template <bool TEXTURED, bool BILINEAR, int ZMODE, int FMT, bool BLEND, bool STQ>
void CSoftwareRasterizer::DrawSpan(int32 y, int32 x0, int32 x1, INTERP& it)
{
	// Local copies: stores into GS RAM go through a uint8 pointer, which may
	// alias anything, so members would be reloaded after every pixel write.
	const STATE s = m_state;
	const GS_SURFACE frame = m_frame;
	const GS_SURFACE depth = m_depth;
	const bool depthFast = m_depthFast;
	const bool depth16 = m_depth16;
	const uint32 zMax = m_zMax;
	const uint32 zpsmFull = m_zpsmFull;
	CTextureCache::CTexture* const texture = m_texture;
	const uint32* const texels = m_texels;
	uint8* const ram = m_ram;
	// REPEAT/CLAMP keep coordinates inside TW x TH, so decoded texels can be
	// indexed directly.
	const bool simpleWrap = (s.wms <= CGSHandler::CLAMP_MODE_CLAMP) && (s.wmt <= CGSHandler::CLAMP_MODE_CLAMP);
	const bool direct = TEXTURED && (texels != nullptr) && simpleWrap;
	const uint32 texWidth = s.tw;
	auto fetchTexel = [&](int32 iu, int32 iv) -> uint32 {
		if(direct)
		{
			uint32 uu = (s.wms == CGSHandler::CLAMP_MODE_REPEAT) ? (iu & (s.tw - 1)) : std::clamp<int32>(iu, 0, s.tw - 1);
			uint32 vv = (s.wmt == CGSHandler::CLAMP_MODE_REPEAT) ? (iv & (s.th - 1)) : std::clamp<int32>(iv, 0, s.th - 1);
			return texels[vv * texWidth + uu];
		}
		return texture->FetchAny(WrapCoord(iu, s.tw, s.wms, s.minu, s.maxu), WrapCoord(iv, s.th, s.wmt, s.minv, s.maxv));
	};
	auto sampleBilinear = [&](int32 u, int32 v) -> uint32 {
		int32 iu = u >> 16, iv = v >> 16;
		uint32 wu = (static_cast<uint32>(u) >> 8) & 0xFF;
		uint32 wv = (static_cast<uint32>(v) >> 8) & 0xFF;
		uint32 t00 = fetchTexel(iu, iv);
		uint32 t10 = fetchTexel(iu + 1, iv);
		uint32 t01 = fetchTexel(iu, iv + 1);
		uint32 t11 = fetchTexel(iu + 1, iv + 1);
		return Lerp2(Lerp2(t00, t10, wu), Lerp2(t01, t11, wu), wv);
	};
	constexpr bool fb16 = (FMT == 2);
	constexpr bool fb24 = (FMT == 1);
	const bool generic16 = (FMT == 3) && GsMemory::IsPsm16(s.fpsm);
	const bool generic24 = (FMT == 3) && GsMemory::IsPsm24(s.fpsm);
	const bool isFb16 = fb16 || generic16;
	const bool isFb24 = fb24 || generic24;

	uint32 frameRow = 0, depthRow = 0;
	const uint32* frameTable = nullptr;
	const uint32* depthTable = nullptr;
	if(FMT != 3)
	{
		frameRow = frame.RowBase(y);
		frameTable = frame.RowTable(y);
	}
	if((ZMODE != 0) && depthFast)
	{
		depthRow = depth.RowBase(y);
		depthTable = depth.RowTable(y);
	}

	// Plain fills (clears, flat UI rectangles): nothing but a store per pixel.
	if(!TEXTURED && (ZMODE == 0) && !BLEND && (FMT == 0 || FMT == 2) && !s.fog && !s.ate && !s.date &&
	   (s.fbmsk == 0) && (it.dr == 0) && (it.dg == 0) && (it.db == 0) && (it.da == 0))
	{
		uint32 color = Clamp255(it.r >> 16) | (Clamp255(it.g >> 16) << 8) | (Clamp255(it.b >> 16) << 16) |
		               ((Clamp255(it.a >> 16) | (s.fba ? 0x80 : 0)) << 24);
		for(int32 x = x0; x <= x1; x++)
		{
			uint32 offset = frame.Offset(frameRow, frameTable, x);
			if(FMT == 0)
				*reinterpret_cast<uint32*>(ram + offset) = color;
			else
				*reinterpret_cast<uint16*>(ram + offset) = static_cast<uint16>(GsMemory::Color32To16(color));
		}
		return;
	}

	const float tw = static_cast<float>(s.tw);
	const float th = static_cast<float>(s.th);

	for(int32 x = x0; x <= x1; x++)
	{
		uint32 r = Clamp255(it.r >> 16);
		uint32 g = Clamp255(it.g >> 16);
		uint32 b = Clamp255(it.b >> 16);
		uint32 a = Clamp255(it.a >> 16);

		if(TEXTURED)
		{
			int32 tu, tv;
			if(STQ)
			{
				float q = (it.q != 0) ? it.q : 1.0f;
				float invQ = 1.0f / q;
				tu = FloatToFixedTexel(it.s * invQ * tw);
				tv = FloatToFixedTexel(it.t * invQ * th);
			}
			else
			{
				tu = it.u;
				tv = it.v;
			}
			uint32 texel = BILINEAR ? sampleBilinear(tu - 0x8000, tv - 0x8000) : fetchTexel(tu >> 16, tv >> 16);
			uint32 tr = texel & 0xFF;
			uint32 tg = (texel >> 8) & 0xFF;
			uint32 tb = (texel >> 16) & 0xFF;
			uint32 ta = texel >> 24;

			switch(s.tfx)
			{
			case CGSHandler::TEX0_FUNCTION_MODULATE:
				r = std::min<uint32>((tr * r) >> 7, 255);
				g = std::min<uint32>((tg * g) >> 7, 255);
				b = std::min<uint32>((tb * b) >> 7, 255);
				if(s.tcc) a = std::min<uint32>((ta * a) >> 7, 255);
				break;
			case CGSHandler::TEX0_FUNCTION_DECAL:
				r = tr;
				g = tg;
				b = tb;
				if(s.tcc) a = ta;
				break;
			case CGSHandler::TEX0_FUNCTION_HIGHLIGHT:
				r = std::min<uint32>(((tr * r) >> 7) + a, 255);
				g = std::min<uint32>(((tg * g) >> 7) + a, 255);
				b = std::min<uint32>(((tb * b) >> 7) + a, 255);
				if(s.tcc) a = std::min<uint32>(ta + a, 255);
				break;
			case CGSHandler::TEX0_FUNCTION_HIGHLIGHT2:
				r = std::min<uint32>(((tr * r) >> 7) + a, 255);
				g = std::min<uint32>(((tg * g) >> 7) + a, 255);
				b = std::min<uint32>(((tb * b) >> 7) + a, 255);
				if(s.tcc) a = ta;
				break;
			}
		}

		uint32 z = 0;
		if(ZMODE != 0)
		{
			z = std::min(ClampZ(it.z), zMax);
		}

		//Step now so that every early-out below can just 'continue'.
		uint32 fog = Clamp255(it.f >> 16);
		it.r += it.dr;
		it.g += it.dg;
		it.b += it.db;
		it.a += it.da;
		it.f += it.df;
		if(ZMODE != 0) it.z += it.dz;
		if(TEXTURED)
		{
			if(STQ)
			{
				it.s += it.ds;
				it.t += it.dt;
				it.q += it.dq;
			}
			else
			{
				it.u += it.du;
				it.v += it.dv;
			}
		}

		if(s.fog)
		{
			r = (fog * r + (255 - fog) * (s.fogColor & 0xFF)) >> 8;
			g = (fog * g + (255 - fog) * ((s.fogColor >> 8) & 0xFF)) >> 8;
			b = (fog * b + (255 - fog) * ((s.fogColor >> 16) & 0xFF)) >> 8;
		}

		bool writeColor = true;
		bool writeAlpha = true;
		bool writeDepth = !s.zmsk;
		if(s.ate)
		{
			bool pass = true;
			switch(s.atst)
			{
			case CGSHandler::ALPHA_TEST_NEVER: pass = false; break;
			case CGSHandler::ALPHA_TEST_ALWAYS: pass = true; break;
			case CGSHandler::ALPHA_TEST_LESS: pass = a < s.aref; break;
			case CGSHandler::ALPHA_TEST_LEQUAL: pass = a <= s.aref; break;
			case CGSHandler::ALPHA_TEST_EQUAL: pass = a == s.aref; break;
			case CGSHandler::ALPHA_TEST_GEQUAL: pass = a >= s.aref; break;
			case CGSHandler::ALPHA_TEST_GREATER: pass = a > s.aref; break;
			case CGSHandler::ALPHA_TEST_NOTEQUAL: pass = a != s.aref; break;
			}
			if(!pass)
			{
				switch(s.afail)
				{
				case CGSHandler::ALPHA_TEST_FAIL_KEEP:
					continue;
				case CGSHandler::ALPHA_TEST_FAIL_FBONLY:
					writeDepth = false;
					break;
				case CGSHandler::ALPHA_TEST_FAIL_ZBONLY:
					writeColor = false;
					writeAlpha = false;
					break;
				case CGSHandler::ALPHA_TEST_FAIL_RGBONLY:
					writeAlpha = false;
					writeDepth = false;
					break;
				}
			}
		}

		uint32 frameOffset = 0;
		if(FMT != 3) frameOffset = frame.Offset(frameRow, frameTable, x);

		uint32 dstRaw = 0;
		bool dstLoaded = false;
		auto loadDst = [&]() {
			if(dstLoaded) return;
			if(FMT == 0 || FMT == 1)
				dstRaw = *reinterpret_cast<const uint32*>(ram + frameOffset);
			else if(FMT == 2)
				dstRaw = *reinterpret_cast<const uint16*>(ram + frameOffset);
			else
				dstRaw = GsMemory::ReadRaw(ram, s.fpsm, s.fbp, s.fbw, x, y);
			dstLoaded = true;
		};

		if(s.date && !isFb24)
		{
			loadDst();
			bool dstAlphaBit = isFb16 ? ((dstRaw & 0x8000) != 0) : ((dstRaw & 0x80000000) != 0);
			if(dstAlphaBit != s.datm) continue;
		}

		uint32 depthOffset = 0;
		if(ZMODE != 0)
		{
			if(depthFast) depthOffset = depth.Offset(depthRow, depthTable, x);
			if(ZMODE == 2)
			{
				uint32 dstZ;
				if(depthFast)
					dstZ = depth16 ? *reinterpret_cast<const uint16*>(ram + depthOffset) : *reinterpret_cast<const uint32*>(ram + depthOffset);
				else
					dstZ = GsMemory::ReadRaw(ram, zpsmFull, s.zbp, s.fbw, x, y);
				dstZ &= zMax;
				bool pass = (s.ztst == CGSHandler::DEPTH_TEST_GEQUAL) ? (z >= dstZ) : (z > dstZ);
				if(!pass) continue;
			}
		}

		if(writeColor || writeAlpha)
		{
			uint32 dst32 = 0;
			bool needDst = BLEND || (s.fbmsk != 0) || !writeAlpha;
			if(needDst)
			{
				loadDst();
				dst32 = isFb16 ? GsMemory::Color16To32(dstRaw) : dstRaw;
				if(isFb24) dst32 = (dst32 & 0x00FFFFFF) | 0x80000000;
			}

			if(BLEND && (!s.pabe || (a & 0x80)))
			{
				int32 cs[3] = {static_cast<int32>(r), static_cast<int32>(g), static_cast<int32>(b)};
				int32 cd[3] = {static_cast<int32>(dst32 & 0xFF), static_cast<int32>((dst32 >> 8) & 0xFF), static_cast<int32>((dst32 >> 16) & 0xFF)};
				int32 ad = static_cast<int32>(dst32 >> 24);
				int32 c = (s.blendC == CGSHandler::ALPHABLEND_C_AS) ? static_cast<int32>(a) : (s.blendC == CGSHandler::ALPHABLEND_C_AD) ? ad : static_cast<int32>(s.blendFix);
				uint32 out[3];
				for(int i = 0; i < 3; i++)
				{
					int32 va = (s.blendA == CGSHandler::ALPHABLEND_ABD_CS) ? cs[i] : (s.blendA == CGSHandler::ALPHABLEND_ABD_CD) ? cd[i] : 0;
					int32 vb = (s.blendB == CGSHandler::ALPHABLEND_ABD_CS) ? cs[i] : (s.blendB == CGSHandler::ALPHABLEND_ABD_CD) ? cd[i] : 0;
					int32 vd = (s.blendD == CGSHandler::ALPHABLEND_ABD_CS) ? cs[i] : (s.blendD == CGSHandler::ALPHABLEND_ABD_CD) ? cd[i] : 0;
					int32 result = (((va - vb) * c) >> 7) + vd;
					out[i] = s.colClamp ? Clamp255(result) : (static_cast<uint32>(result) & 0xFF);
				}
				r = out[0];
				g = out[1];
				b = out[2];
			}

			uint32 outA = a | (s.fba ? 0x80 : 0);
			uint32 src32 = (r & 0xFF) | ((g & 0xFF) << 8) | ((b & 0xFF) << 16) | ((outA & 0xFF) << 24);

			uint32 mask = s.fbmsk;
			if(!writeAlpha) mask |= 0xFF000000;
			uint32 final32 = (dst32 & mask) | (src32 & ~mask);

			if(FMT == 0)
			{
				*reinterpret_cast<uint32*>(ram + frameOffset) = final32;
			}
			else if(FMT == 1)
			{
				auto pixel = reinterpret_cast<uint32*>(ram + frameOffset);
				*pixel = (*pixel & 0xFF000000) | (final32 & 0x00FFFFFF);
			}
			else if(FMT == 2)
			{
				*reinterpret_cast<uint16*>(ram + frameOffset) = static_cast<uint16>(GsMemory::Color32To16(final32));
			}
			else
			{
				GsMemory::WriteRaw(ram, s.fpsm, s.fbp, s.fbw, x, y, isFb16 ? GsMemory::Color32To16(final32) : final32);
			}
		}

		if((ZMODE != 0) && writeDepth)
		{
			if(depthFast)
			{
				if(depth16)
				{
					*reinterpret_cast<uint16*>(ram + depthOffset) = static_cast<uint16>(z);
				}
				else if(zMax == 0x00FFFFFF)
				{
					auto pixel = reinterpret_cast<uint32*>(ram + depthOffset);
					*pixel = (*pixel & 0xFF000000) | z;
				}
				else
				{
					*reinterpret_cast<uint32*>(ram + depthOffset) = z;
				}
			}
			else
			{
				GsMemory::WriteRaw(ram, zpsmFull, s.zbp, s.fbw, x, y, z);
			}
		}
	}
}

template <bool T, bool BL, bool STQ, int Z, int F>
CSoftwareRasterizer::SpanFunction CSoftwareRasterizer::SelectBlend(bool blend)
{
	return blend ? &CSoftwareRasterizer::DrawSpan<T, BL, Z, F, true, STQ> : &CSoftwareRasterizer::DrawSpan<T, BL, Z, F, false, STQ>;
}

template <bool T, bool BL, bool STQ, int Z>
CSoftwareRasterizer::SpanFunction CSoftwareRasterizer::SelectFormat(int fmt, bool blend)
{
	switch(fmt)
	{
	case 0: return SelectBlend<T, BL, STQ, Z, 0>(blend);
	case 1: return SelectBlend<T, BL, STQ, Z, 1>(blend);
	case 2: return SelectBlend<T, BL, STQ, Z, 2>(blend);
	default: return SelectBlend<T, BL, STQ, Z, 3>(blend);
	}
}

template <bool T, bool BL, bool STQ>
CSoftwareRasterizer::SpanFunction CSoftwareRasterizer::SelectDepth(int zmode, int fmt, bool blend)
{
	switch(zmode)
	{
	case 0: return SelectFormat<T, BL, STQ, 0>(fmt, blend);
	case 1: return SelectFormat<T, BL, STQ, 1>(fmt, blend);
	default: return SelectFormat<T, BL, STQ, 2>(fmt, blend);
	}
}

void CSoftwareRasterizer::SelectSpanFunction()
{
	const auto& s = m_state;
	int fmt = 3;
	if(m_frameFast)
	{
		switch(s.fpsm)
		{
		case CGSHandler::PSMCT32:
		case CGSHandler::PSMCT32_UNK:
			fmt = 0;
			break;
		case CGSHandler::PSMCT24:
		case CGSHandler::PSMCT24_UNK:
			fmt = 1;
			break;
		case CGSHandler::PSMCT16:
		case CGSHandler::PSMCT16S:
			fmt = 2;
			break;
		}
	}
	int zmode = !s.zte ? 0 : (s.ztst == CGSHandler::DEPTH_TEST_ALWAYS) ? 1 : 2;
	bool blend = s.alphaBlend;

	if(!s.textured)
	{
		m_span = SelectDepth<false, false, false>(zmode, fmt, blend);
		m_spanLinear = m_span;
	}
	else if(s.bilinear)
	{
		m_span = s.fst ? SelectDepth<true, true, false>(zmode, fmt, blend) : SelectDepth<true, true, true>(zmode, fmt, blend);
		m_spanLinear = SelectDepth<true, true, false>(zmode, fmt, blend);
	}
	else
	{
		m_span = s.fst ? SelectDepth<true, false, false>(zmode, fmt, blend) : SelectDepth<true, false, true>(zmode, fmt, blend);
		m_spanLinear = SelectDepth<true, false, false>(zmode, fmt, blend);
	}
}

//-----------------------------------------------------------------------------
// Primitives
//-----------------------------------------------------------------------------

void CSoftwareRasterizer::DrawPixel(int32 x, int32 y, INTERP& it, bool linear)
{
	const auto& s = m_state;
	if((x < s.scax0) || (x > s.scax1) || (y < s.scay0) || (y > s.scay1)) return;
	if(!RowEnabled(y)) return;
	if(!EnsureTexture()) return;
	MarkWritten(x, y, x, y);
	(this->*(linear ? m_spanLinear : m_span))(y, x, x, it);
}

CSoftwareRasterizer::INTERP CSoftwareRasterizer::MakeConstantInterp(uint32 r, uint32 g, uint32 b, uint32 a, uint32 fog, uint32 z) const
{
	INTERP it = {};
	it.r = static_cast<int32>(r) << 16;
	it.g = static_cast<int32>(g) << 16;
	it.b = static_cast<int32>(b) << 16;
	it.a = static_cast<int32>(a) << 16;
	it.f = static_cast<int32>(fog) << 16;
	it.z = static_cast<int64>(z) << 16;
	it.q = 1.0f;
	return it;
}

void CSoftwareRasterizer::DrawPoint(const VERTEX& v)
{
	if(m_drawNothing) return;
	INTERP it = MakeConstantInterp(v.r, v.g, v.b, v.a, v.fog, v.z);
	if(m_state.fst)
	{
		it.u = static_cast<int32>(std::floor(v.u * 65536.0f));
		it.v = static_cast<int32>(std::floor(v.v * 65536.0f));
	}
	else
	{
		float q = (v.q != 0) ? v.q : 1.0f;
		it.u = FloatToFixedTexel((v.s / q) * m_state.tw);
		it.v = FloatToFixedTexel((v.t / q) * m_state.th);
	}
	DrawPixel((v.x + 8) >> 4, (v.y + 8) >> 4, it, true);
}

void CSoftwareRasterizer::DrawLine(const VERTEX& v0, const VERTEX& v1)
{
	if(m_drawNothing) return;
	int32 x0 = (v0.x + 8) >> 4, y0 = (v0.y + 8) >> 4;
	int32 x1 = (v1.x + 8) >> 4, y1 = (v1.y + 8) >> 4;
	int32 steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
	if(steps == 0)
	{
		DrawPoint(v1);
		return;
	}

	for(int32 i = 0; i < steps; i++)
	{
		float t = static_cast<float>(i) / static_cast<float>(steps);
		auto lerp = [t](float a, float b) { return a + (b - a) * t; };
		const VERTEX& colorSrc = m_state.gouraud ? v0 : v1;
		uint32 r = colorSrc.r, g = colorSrc.g, b = colorSrc.b, a = colorSrc.a;
		if(m_state.gouraud)
		{
			r = static_cast<uint32>(lerp(v0.r, v1.r));
			g = static_cast<uint32>(lerp(v0.g, v1.g));
			b = static_cast<uint32>(lerp(v0.b, v1.b));
			a = static_cast<uint32>(lerp(v0.a, v1.a));
		}
		uint32 z = static_cast<uint32>(static_cast<double>(v0.z) + (static_cast<double>(v1.z) - static_cast<double>(v0.z)) * t);
		INTERP it = MakeConstantInterp(r, g, b, a, static_cast<uint32>(lerp(v0.fog, v1.fog)), z);
		if(m_state.fst)
		{
			it.u = static_cast<int32>(std::floor(lerp(v0.u, v1.u) * 65536.0f));
			it.v = static_cast<int32>(std::floor(lerp(v0.v, v1.v) * 65536.0f));
		}
		else
		{
			float q = lerp(v0.q, v1.q);
			if(q == 0) q = 1.0f;
			it.u = FloatToFixedTexel((lerp(v0.s, v1.s) / q) * m_state.tw);
			it.v = FloatToFixedTexel((lerp(v0.t, v1.t) / q) * m_state.th);
		}
		int32 px = x0 + static_cast<int32>(std::lround(static_cast<float>(x1 - x0) * t));
		int32 py = y0 + static_cast<int32>(std::lround(static_cast<float>(y1 - y0) * t));
		DrawPixel(px, py, it, true);
	}
}

void CSoftwareRasterizer::DrawTriangle(const VERTEX& va, const VERTEX& vb, const VERTEX& vc)
{
	if(m_drawNothing) return;

	const VERTEX* v0 = &va;
	const VERTEX* v1 = &vb;
	const VERTEX* v2 = &vc;

	int64 area = EdgeFunction(v0->x, v0->y, v1->x, v1->y, v2->x, v2->y);
	if(area == 0) return;
	if(area < 0)
	{
		std::swap(v1, v2);
		area = -area;
	}

	const auto& s = m_state;

	int32 minX = std::max(CeilFixed(std::min({v0->x, v1->x, v2->x})), s.scax0);
	int32 maxX = std::min((std::max({v0->x, v1->x, v2->x})) >> 4, s.scax1);
	int32 minY = std::max(CeilFixed(std::min({v0->y, v1->y, v2->y})), s.scay0);
	int32 maxY = std::min((std::max({v0->y, v1->y, v2->y})) >> 4, s.scay1);
	if((minX > maxX) || (minY > maxY)) return;

	if(!EnsureTexture()) return;
	MarkWritten(minX, minY, maxX, maxY);

	// Everything below is set up once per triangle; the per-row work is
	// incremental integer math only. (The Vita's Cortex-A9 has no integer
	// divide and slow double precision, so per-row divisions are expensive.)

	// Edges as A*x + K(y) >= 0 over pixel x (sampled at x*16); K steps by
	// stepK per row.
	struct EDGE
	{
		int64 A;
		int64 K;
		int64 stepK;
		float invA;
	};
	auto makeEdge = [&](const VERTEX* a, const VERTEX* b) {
		EDGE edge;
		int64 bias = IsTopLeft(a->x, a->y, b->x, b->y) ? 0 : -1;
		edge.A = -static_cast<int64>(b->y - a->y) * 16;
		int32 py = minY * 16;
		edge.K = static_cast<int64>(b->x - a->x) * static_cast<int64>(py - a->y) +
		         static_cast<int64>(b->y - a->y) * static_cast<int64>(a->x) + bias;
		edge.stepK = static_cast<int64>(b->x - a->x) * 16;
		edge.invA = (edge.A != 0) ? 1.0f / static_cast<float>(edge.A) : 0.0f;
		return edge;
	};
	EDGE edges[3] = {makeEdge(v1, v2), makeEdge(v2, v0), makeEdge(v0, v1)};

	// Barycentric weights at the reference pixel (minX, minY) and their
	// derivatives per pixel in x and y.
	double invArea = 1.0 / static_cast<double>(area);
	int32 refPx = minX * 16, refPy = minY * 16;
	double l0 = static_cast<double>(EdgeFunction(v1->x, v1->y, v2->x, v2->y, refPx, refPy)) * invArea;
	double l1 = static_cast<double>(EdgeFunction(v2->x, v2->y, v0->x, v0->y, refPx, refPy)) * invArea;
	double dl0dx = static_cast<double>(edges[0].A) * invArea;
	double dl1dx = static_cast<double>(edges[1].A) * invArea;
	double dl0dy = static_cast<double>(edges[0].stepK) * invArea;
	double dl1dy = static_cast<double>(edges[1].stepK) * invArea;

	struct PLANE
	{
		double value, dx, dy;
	};
	auto plane = [&](double a0, double a1, double a2) {
		PLANE p;
		p.value = a2 + (a0 - a2) * l0 + (a1 - a2) * l1;
		p.dx = (a0 - a2) * dl0dx + (a1 - a2) * dl1dx;
		p.dy = (a0 - a2) * dl0dy + (a1 - a2) * dl1dy;
		return p;
	};
	// 16.16 fixed point planes, stepped in int64.
	struct FIXED_PLANE
	{
		int64 value, dy;
		int32 dx;
	};
	auto fixedPlane = [&](const PLANE& p, double bias) {
		FIXED_PLANE f;
		f.value = static_cast<int64>(std::floor((p.value + bias) * 65536.0));
		f.dx = ToFixed(p.dx);
		f.dy = static_cast<int64>(std::floor(p.dy * 65536.0));
		return f;
	};

	// Flat shading uses the color of the last vertex that was kicked.
	const VERTEX& flat = vc;
	const bool gouraud = s.gouraud;
	const bool textured = s.textured;
	const bool fst = s.fst;

	FIXED_PLANE pr = {}, pg = {}, pb = {}, pa = {};
	if(gouraud)
	{
		pr = fixedPlane(plane(v0->r, v1->r, v2->r), 0.5);
		pg = fixedPlane(plane(v0->g, v1->g, v2->g), 0.5);
		pb = fixedPlane(plane(v0->b, v1->b, v2->b), 0.5);
		pa = fixedPlane(plane(v0->a, v1->a, v2->a), 0.5);
	}
	FIXED_PLANE pf = fixedPlane(plane(v0->fog, v1->fog, v2->fog), 0.5);
	PLANE zPlane = plane(v0->z, v1->z, v2->z);
	FIXED_PLANE pz;
	pz.value = static_cast<int64>(std::floor(zPlane.value * 65536.0));
	int64 dzdx = static_cast<int64>(std::floor(zPlane.dx * 65536.0));
	pz.dy = static_cast<int64>(std::floor(zPlane.dy * 65536.0));

	FIXED_PLANE pu = {}, pv = {};
	PLANE ps = {}, pt = {}, pq = {};
	if(textured)
	{
		if(fst)
		{
			pu = fixedPlane(plane(v0->u, v1->u, v2->u), 0);
			pv = fixedPlane(plane(v0->v, v1->v, v2->v), 0);
		}
		else
		{
			float q0 = (v0->q != 0) ? v0->q : 1.0f;
			float q1 = (v1->q != 0) ? v1->q : 1.0f;
			float q2 = (v2->q != 0) ? v2->q : 1.0f;
			ps = plane(v0->s, v1->s, v2->s);
			pt = plane(v0->t, v1->t, v2->t);
			pq = plane(q0, q1, q2);
		}
	}

	INTERP step = {};
	step.dr = pr.dx;
	step.dg = pg.dx;
	step.db = pb.dx;
	step.da = pa.dx;
	step.df = pf.dx;
	step.dz = dzdx;
	step.du = pu.dx;
	step.dv = pv.dx;
	step.ds = static_cast<float>(ps.dx);
	step.dt = static_cast<float>(pt.dx);
	step.dq = static_cast<float>(pq.dx);

	for(int32 y = minY; y <= maxY; y++)
	{
		int32 rowIndex = y - minY;
		if(rowIndex != 0)
		{
			for(auto& edge : edges) edge.K += edge.stepK;
		}
		if(!RowEnabled(y)) continue;

		// Span extents: float estimate, then exact integer correction.
		int32 lo = minX, hi = maxX;
		bool empty = false;
		for(const auto& edge : edges)
		{
			if(edge.A > 0)
			{
				//x >= -K/A
				float estimate = -static_cast<float>(edge.K) * edge.invA;
				int32 x = static_cast<int32>(std::clamp(std::ceil(estimate), static_cast<float>(minX - 1), static_cast<float>(maxX + 1)));
				while((x <= maxX) && (edge.A * x + edge.K < 0)) x++;
				while((x > minX) && (edge.A * (x - 1) + edge.K >= 0)) x--;
				lo = std::max(lo, x);
			}
			else if(edge.A < 0)
			{
				//x <= K/-A
				float estimate = -static_cast<float>(edge.K) * edge.invA;
				int32 x = static_cast<int32>(std::clamp(std::floor(estimate), static_cast<float>(minX - 1), static_cast<float>(maxX + 1)));
				while((x >= minX) && (edge.A * x + edge.K < 0)) x--;
				while((x < maxX) && (edge.A * (x + 1) + edge.K >= 0)) x++;
				hi = std::min(hi, x);
			}
			else if(edge.K < 0)
			{
				empty = true;
			}
		}
		if(empty || (lo > hi)) continue;

		int32 dx = lo - minX;
		INTERP it = step;
		if(gouraud)
		{
			it.r = static_cast<int32>(pr.value + pr.dy * rowIndex + static_cast<int64>(pr.dx) * dx);
			it.g = static_cast<int32>(pg.value + pg.dy * rowIndex + static_cast<int64>(pg.dx) * dx);
			it.b = static_cast<int32>(pb.value + pb.dy * rowIndex + static_cast<int64>(pb.dx) * dx);
			it.a = static_cast<int32>(pa.value + pa.dy * rowIndex + static_cast<int64>(pa.dx) * dx);
		}
		else
		{
			it.r = flat.r << 16;
			it.g = flat.g << 16;
			it.b = flat.b << 16;
			it.a = flat.a << 16;
		}
		it.f = static_cast<int32>(pf.value + pf.dy * rowIndex + static_cast<int64>(pf.dx) * dx);
		it.z = pz.value + pz.dy * rowIndex + dzdx * dx;
		if(textured)
		{
			if(fst)
			{
				it.u = static_cast<int32>(pu.value + pu.dy * rowIndex + static_cast<int64>(pu.dx) * dx);
				it.v = static_cast<int32>(pv.value + pv.dy * rowIndex + static_cast<int64>(pv.dx) * dx);
			}
			else
			{
				float fy = static_cast<float>(rowIndex), fx = static_cast<float>(dx);
				it.s = static_cast<float>(ps.value + ps.dy * fy + ps.dx * fx);
				it.t = static_cast<float>(pt.value + pt.dy * fy + pt.dx * fx);
				it.q = static_cast<float>(pq.value + pq.dy * fy + pq.dx * fx);
			}
		}

		(this->*m_span)(y, lo, hi, it);
	}
}

void CSoftwareRasterizer::DrawSprite(const VERTEX& va, const VERTEX& vb)
{
	if(m_drawNothing) return;
	const auto& s = m_state;

	int32 x0 = va.x, x1 = vb.x, y0 = va.y, y1 = vb.y;
	float u0 = 0, u1 = 0, v0 = 0, v1 = 0;
	if(s.fst)
	{
		u0 = va.u;
		u1 = vb.u;
		v0 = va.v;
		v1 = vb.v;
	}
	else
	{
		float qa = (va.q != 0) ? va.q : 1.0f;
		float qb = (vb.q != 0) ? vb.q : 1.0f;
		u0 = (va.s / qa) * s.tw;
		u1 = (vb.s / qb) * s.tw;
		v0 = (va.t / qa) * s.th;
		v1 = (vb.t / qb) * s.th;
	}
	if(x0 > x1)
	{
		std::swap(x0, x1);
		std::swap(u0, u1);
	}
	if(y0 > y1)
	{
		std::swap(y0, y1);
		std::swap(v0, v1);
	}
	if((x0 == x1) || (y0 == y1)) return;

	int32 startX = std::max(CeilFixed(x0), s.scax0);
	int32 endX = std::min(CeilFixed(x1) - 1, s.scax1);
	int32 startY = std::max(CeilFixed(y0), s.scay0);
	int32 endY = std::min(CeilFixed(y1) - 1, s.scay1);
	if((startX > endX) || (startY > endY)) return;

	if(!EnsureTexture()) return;
	MarkWritten(startX, startY, endX, endY);

	float dudx = (u1 - u0) / static_cast<float>(x1 - x0);
	float dvdy = (v1 - v0) / static_cast<float>(y1 - y0);

	INTERP base = MakeConstantInterp(vb.r, vb.g, vb.b, vb.a, vb.fog, vb.z);
	if(s.textured)
	{
		base.u = FloatToFixedTexel(u0 + dudx * static_cast<float>(startX * 16 - x0));
		base.du = FloatToFixedTexel(dudx * 16.0f);
	}

	for(int32 y = startY; y <= endY; y++)
	{
		if(!RowEnabled(y)) continue;
		INTERP it = base;
		if(s.textured)
		{
			it.v = FloatToFixedTexel(v0 + dvdy * static_cast<float>(y * 16 - y0));
		}
		(this->*m_spanLinear)(y, startX, endX, it);
	}
}

//-----------------------------------------------------------------------------
// Multi-threaded rendering support
//-----------------------------------------------------------------------------

bool CSoftwareRasterizer::ComputeBounds(PRIMITIVE_KIND kind, const VERTEX* v, int32& x0, int32& y0, int32& x1, int32& y1) const
{
	const auto& s = m_state;
	switch(kind)
	{
	case PRIMITIVE_POINT:
		x0 = x1 = (v[0].x + 8) >> 4;
		y0 = y1 = (v[0].y + 8) >> 4;
		break;
	case PRIMITIVE_LINE:
		x0 = std::min((v[0].x + 8) >> 4, (v[1].x + 8) >> 4);
		x1 = std::max((v[0].x + 8) >> 4, (v[1].x + 8) >> 4);
		y0 = std::min((v[0].y + 8) >> 4, (v[1].y + 8) >> 4);
		y1 = std::max((v[0].y + 8) >> 4, (v[1].y + 8) >> 4);
		break;
	case PRIMITIVE_TRIANGLE:
		x0 = CeilFixed(std::min({v[0].x, v[1].x, v[2].x}));
		x1 = std::max({v[0].x, v[1].x, v[2].x}) >> 4;
		y0 = CeilFixed(std::min({v[0].y, v[1].y, v[2].y}));
		y1 = std::max({v[0].y, v[1].y, v[2].y}) >> 4;
		break;
	case PRIMITIVE_SPRITE:
		x0 = CeilFixed(std::min(v[0].x, v[1].x));
		x1 = CeilFixed(std::max(v[0].x, v[1].x)) - 1;
		y0 = CeilFixed(std::min(v[0].y, v[1].y));
		y1 = CeilFixed(std::max(v[0].y, v[1].y)) - 1;
		break;
	}
	x0 = std::max(x0, s.scax0);
	y0 = std::max(y0, s.scay0);
	x1 = std::min(x1, s.scax1);
	y1 = std::min(y1, s.scay1);
	return (x0 <= x1) && (y0 <= y1);
}

bool CSoftwareRasterizer::Prepare(PRIMITIVE_KIND kind, const VERTEX* vertices)
{
	if(m_drawNothing) return false;
	int32 x0, y0, x1, y1;
	if(!ComputeBounds(kind, vertices, x0, y0, x1, y1)) return false;
	// Texture first: a primitive sampling its own render target reads memory
	// as it was before the primitive; the write stamp then invalidates the
	// texture for the primitives that follow.
	if(!EnsureTexture()) return false;
	MarkWritten(x0, y0, x1, y1);
	return true;
}

CSoftwareRasterizer::PREPARED_STATE CSoftwareRasterizer::GetPreparedState() const
{
	PREPARED_STATE prepared;
	prepared.state = m_state;
	prepared.texture = m_texture;
	prepared.texels = m_texels;
	prepared.texelsGeneration = m_texelsGeneration;
	return prepared;
}

void CSoftwareRasterizer::ApplyPrepared(const PREPARED_STATE& prepared)
{
	SetState(prepared.state);
	m_texture = prepared.texture;
	m_texels = prepared.texels;
	m_texelsGeneration = prepared.texelsGeneration;
}

void CSoftwareRasterizer::SetLane(uint32 index, uint32 count)
{
	m_laneMode = true;
	m_laneIndex = index;
	m_laneCount = std::max<uint32>(count, 1);
}

bool CSoftwareRasterizer::PendingWritesOverlapTexture() const
{
	if(!m_state.textured) return false;
	uint32 first = 0, count = 0;
	CTextureCache::GetPageRange(m_textureKey, first, count);
	for(uint32 i = 0; i < count; i++)
	{
		uint32 page = (first + i) & 511;
		if(m_pendingPages[page >> 5] & (1u << (page & 31))) return true;
	}
	return false;
}

uint64 CSoftwareRasterizer::GetLayoutKey() const
{
	const auto& s = m_state;
	bool depthUsed = s.zte;
	uint64 key = static_cast<uint64>(s.fbp >> 13) | (static_cast<uint64>(s.fbw) << 9) | (static_cast<uint64>(s.fpsm) << 15);
	if(depthUsed) key |= (static_cast<uint64>(s.zbp >> 13) << 21) | (static_cast<uint64>(s.zpsm & 0xF) << 30) | (1ULL << 34);
	return key;
}

bool CSoftwareRasterizer::IsLayoutAliased() const
{
	const auto& s = m_state;
	if(!s.zte) return false;
	if(!m_frameFast || !m_depthFast) return true;
	auto range = [&](const GS_SURFACE& surface, uint32& first, uint32& count) {
		uint32 rows = (static_cast<uint32>(std::max(s.scay1, 0)) >> surface.phShift) + 1;
		first = surface.FirstPage();
		count = std::min<uint32>(rows * std::max<uint32>(surface.pagesPerRow, 1) + 1, 512);
	};
	uint32 f0, fc, z0, zc;
	range(m_frame, f0, fc);
	range(m_depth, z0, zc);
	//Wrapping intervals on a 512 page ring
	auto contains = [](uint32 start, uint32 count, uint32 page) { return ((page - start) & 511) < count; };
	return contains(f0, fc, z0) || contains(z0, zc, f0);
}

void CSoftwareRasterizer::ClearPendingWrites()
{
	m_pendingPages.fill(0);
}
