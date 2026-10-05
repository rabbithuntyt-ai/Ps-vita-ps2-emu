#include "SoftwareRasterizer.h"
#include "GsMemory.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

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

	// Floor division of a 12.4 fixed point coordinate to the first covered pixel.
	inline int32 CeilFixed(int32 value)
	{
		return (value + 15) >> 4;
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

	inline uint32 ZPsmToFull(uint32 zpsm)
	{
		return 0x30 | (zpsm & 0xF);
	}
}

void CSoftwareRasterizer::SetMemory(uint8* ram, const uint16* clut)
{
	m_ram = ram;
	m_clut = clut;
}

void CSoftwareRasterizer::SetState(const STATE& state)
{
	m_state = state;
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

uint32 CSoftwareRasterizer::ExpandAlpha16(uint32 color16) const
{
	uint32 rgb = GsMemory::Color16To32(color16) & 0x00FFFFFF;
	uint32 alpha = 0;
	if(color16 & 0x8000)
	{
		alpha = m_state.ta1;
	}
	else if(!(m_state.aem && ((color16 & 0x7FFF) == 0)))
	{
		alpha = m_state.ta0;
	}
	return rgb | (alpha << 24);
}

uint32 CSoftwareRasterizer::LookupClut(uint32 index) const
{
	bool clut32 = !GsMemory::IsPsm16(m_state.cpsm);
	bool idtex4 = CGsPixelFormats::IsPsmIDTEX4(m_state.tpsm);
	if(clut32)
	{
		uint32 offset = idtex4 ? (((m_state.csa & 0xF) * 16) + index) : (((m_state.csa * 16) + index) & 0xFF);
		return static_cast<uint32>(m_clut[offset]) | (static_cast<uint32>(m_clut[offset + 0x100]) << 16);
	}
	else
	{
		uint32 offset = ((m_state.csa * 16) + index) & 0x1FF;
		return ExpandAlpha16(m_clut[offset]);
	}
}

int32 CSoftwareRasterizer::WrapCoord(int32 coord, uint32 size, uint32 mode, uint32 minc, uint32 maxc) const
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

uint32 CSoftwareRasterizer::FetchTexel(int32 u, int32 v)
{
	const auto& s = m_state;
	u = WrapCoord(u, s.tw, s.wms, s.minu, s.maxu);
	v = WrapCoord(v, s.th, s.wmt, s.minv, s.maxv);

	uint32 raw = GsMemory::ReadRaw(m_ram, s.tpsm, s.tbp, s.tbw, u, v);
	switch(s.tpsm)
	{
	default:
	case CGSHandler::PSMCT32:
	case CGSHandler::PSMCT32_UNK:
	case CGSHandler::PSMZ32:
		return raw;
	case CGSHandler::PSMCT24:
	case CGSHandler::PSMCT24_UNK:
	case CGSHandler::PSMZ24:
	{
		uint32 rgb = raw & 0x00FFFFFF;
		uint32 alpha = (s.aem && (rgb == 0)) ? 0 : s.ta0;
		return rgb | (alpha << 24);
	}
	case CGSHandler::PSMCT16:
	case CGSHandler::PSMCT16S:
	case CGSHandler::PSMZ16:
	case CGSHandler::PSMZ16S:
		return ExpandAlpha16(raw);
	case CGSHandler::PSMT8:
		return LookupClut(raw & 0xFF);
	case CGSHandler::PSMT4:
		return LookupClut(raw & 0xF);
	case CGSHandler::PSMT8H:
		return LookupClut(raw >> 24);
	case CGSHandler::PSMT4HL:
		return LookupClut((raw >> 24) & 0xF);
	case CGSHandler::PSMT4HH:
		return LookupClut(raw >> 28);
	}
}

uint32 CSoftwareRasterizer::SampleTexture(float u, float v)
{
	if(!m_state.bilinear)
	{
		return FetchTexel(static_cast<int32>(std::floor(u)), static_cast<int32>(std::floor(v)));
	}

	u -= 0.5f;
	v -= 0.5f;
	float fu = std::floor(u);
	float fv = std::floor(v);
	int32 iu = static_cast<int32>(fu);
	int32 iv = static_cast<int32>(fv);
	uint32 wu = static_cast<uint32>((u - fu) * 256.0f);
	uint32 wv = static_cast<uint32>((v - fv) * 256.0f);

	uint32 t00 = FetchTexel(iu, iv);
	uint32 t10 = FetchTexel(iu + 1, iv);
	uint32 t01 = FetchTexel(iu, iv + 1);
	uint32 t11 = FetchTexel(iu + 1, iv + 1);

	uint32 result = 0;
	for(uint32 shift = 0; shift < 32; shift += 8)
	{
		uint32 c00 = (t00 >> shift) & 0xFF;
		uint32 c10 = (t10 >> shift) & 0xFF;
		uint32 c01 = (t01 >> shift) & 0xFF;
		uint32 c11 = (t11 >> shift) & 0xFF;
		uint32 top = (c00 * (256 - wu) + c10 * wu) >> 8;
		uint32 bottom = (c01 * (256 - wu) + c11 * wu) >> 8;
		uint32 c = (top * (256 - wv) + bottom * wv) >> 8;
		result |= (c & 0xFF) << shift;
	}
	return result;
}

//-----------------------------------------------------------------------------
// Per pixel pipeline
//-----------------------------------------------------------------------------

void CSoftwareRasterizer::ShadePixel(int32 x, int32 y, FRAGMENT& frag)
{
	const auto& s = m_state;

	if((x < s.scax0) || (x > s.scax1) || (y < s.scay0) || (y > s.scay1)) return;

	uint32 r = frag.r, g = frag.g, b = frag.b, a = frag.a;

	if(s.textured)
	{
		uint32 texel = SampleTexture(frag.u, frag.v);
		uint32 tr = texel & 0xFF;
		uint32 tg = (texel >> 8) & 0xFF;
		uint32 tb = (texel >> 16) & 0xFF;
		uint32 ta = texel >> 24;

		switch(s.tfx)
		{
		case CGSHandler::TEX0_FUNCTION_MODULATE:
			r = Clamp255((tr * r) >> 7);
			g = Clamp255((tg * g) >> 7);
			b = Clamp255((tb * b) >> 7);
			if(s.tcc) a = Clamp255((ta * a) >> 7);
			break;
		case CGSHandler::TEX0_FUNCTION_DECAL:
			r = tr;
			g = tg;
			b = tb;
			if(s.tcc) a = ta;
			break;
		case CGSHandler::TEX0_FUNCTION_HIGHLIGHT:
			r = Clamp255(((tr * r) >> 7) + a);
			g = Clamp255(((tg * g) >> 7) + a);
			b = Clamp255(((tb * b) >> 7) + a);
			if(s.tcc) a = Clamp255(ta + a);
			break;
		case CGSHandler::TEX0_FUNCTION_HIGHLIGHT2:
			r = Clamp255(((tr * r) >> 7) + a);
			g = Clamp255(((tg * g) >> 7) + a);
			b = Clamp255(((tb * b) >> 7) + a);
			if(s.tcc) a = ta;
			break;
		}
	}

	if(s.fog)
	{
		uint32 f = frag.fog;
		r = (f * r + (255 - f) * (s.fogColor & 0xFF)) >> 8;
		g = (f * g + (255 - f) * ((s.fogColor >> 8) & 0xFF)) >> 8;
		b = (f * b + (255 - f) * ((s.fogColor >> 16) & 0xFF)) >> 8;
	}

	//Alpha test
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
				return;
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

	bool fb16 = GsMemory::IsPsm16(s.fpsm);
	bool fb24 = GsMemory::IsPsm24(s.fpsm);

	//Destination alpha test
	uint32 dstRaw = 0;
	bool dstLoaded = false;
	auto loadDst = [&]() {
		if(!dstLoaded)
		{
			dstRaw = GsMemory::ReadRaw(m_ram, s.fpsm, s.fbp, s.fbw, x, y);
			dstLoaded = true;
		}
	};
	if(s.date && !fb24)
	{
		loadDst();
		bool dstAlphaBit = fb16 ? ((dstRaw & 0x8000) != 0) : ((dstRaw & 0x80000000) != 0);
		if(dstAlphaBit != s.datm) return;
	}

	//Depth test
	uint32 zpsm = ZPsmToFull(s.zpsm);
	uint32 z = std::min(frag.z, ZMaxForPsm(s.zpsm));
	if(s.zte)
	{
		switch(s.ztst)
		{
		case CGSHandler::DEPTH_TEST_NEVER:
			return;
		case CGSHandler::DEPTH_TEST_ALWAYS:
			break;
		case CGSHandler::DEPTH_TEST_GEQUAL:
		case CGSHandler::DEPTH_TEST_GREATER:
		{
			uint32 dstZ = GsMemory::ReadRaw(m_ram, zpsm, s.zbp, s.fbw, x, y) & ZMaxForPsm(s.zpsm);
			bool pass = (s.ztst == CGSHandler::DEPTH_TEST_GEQUAL) ? (z >= dstZ) : (z > dstZ);
			if(!pass) return;
		}
		break;
		}
	}

	if(writeColor || writeAlpha)
	{
		uint32 dst32 = 0;
		bool needDst = s.alphaBlend || (s.fbmsk != 0) || !writeAlpha;
		if(needDst)
		{
			loadDst();
			dst32 = fb16 ? GsMemory::Color16To32(dstRaw) : dstRaw;
			if(fb24) dst32 = (dst32 & 0x00FFFFFF) | 0x80000000;
		}

		if(s.alphaBlend && (!s.pabe || (a & 0x80)))
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

		if(fb16)
		{
			GsMemory::WriteRaw(m_ram, s.fpsm, s.fbp, s.fbw, x, y, GsMemory::Color32To16(final32));
		}
		else
		{
			GsMemory::WriteRaw(m_ram, s.fpsm, s.fbp, s.fbw, x, y, final32);
		}
		m_pixelsWritten++;
	}

	if(writeDepth && s.zte)
	{
		GsMemory::WriteRaw(m_ram, zpsm, s.zbp, s.fbw, x, y, z);
	}
}

//-----------------------------------------------------------------------------
// Primitives
//-----------------------------------------------------------------------------

void CSoftwareRasterizer::DrawPoint(const VERTEX& v)
{
	FRAGMENT frag;
	frag.r = v.r;
	frag.g = v.g;
	frag.b = v.b;
	frag.a = v.a;
	frag.z = v.z;
	frag.fog = v.fog;
	if(m_state.fst)
	{
		frag.u = v.u;
		frag.v = v.v;
	}
	else
	{
		float q = (v.q != 0) ? v.q : 1.0f;
		frag.u = (v.s / q) * m_state.tw;
		frag.v = (v.t / q) * m_state.th;
	}
	ShadePixel((v.x + 8) >> 4, (v.y + 8) >> 4, frag);
}

void CSoftwareRasterizer::DrawLine(const VERTEX& v0, const VERTEX& v1)
{
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
		FRAGMENT frag;
		if(m_state.gouraud)
		{
			frag.r = static_cast<uint32>(lerp(v0.r, v1.r));
			frag.g = static_cast<uint32>(lerp(v0.g, v1.g));
			frag.b = static_cast<uint32>(lerp(v0.b, v1.b));
			frag.a = static_cast<uint32>(lerp(v0.a, v1.a));
		}
		else
		{
			frag.r = colorSrc.r;
			frag.g = colorSrc.g;
			frag.b = colorSrc.b;
			frag.a = colorSrc.a;
		}
		frag.z = static_cast<uint32>(static_cast<double>(v0.z) + (static_cast<double>(v1.z) - static_cast<double>(v0.z)) * t);
		frag.fog = static_cast<uint32>(lerp(v0.fog, v1.fog));
		if(m_state.fst)
		{
			frag.u = lerp(v0.u, v1.u);
			frag.v = lerp(v0.v, v1.v);
		}
		else
		{
			float q = lerp(v0.q, v1.q);
			if(q == 0) q = 1.0f;
			frag.u = (lerp(v0.s, v1.s) / q) * m_state.tw;
			frag.v = (lerp(v0.t, v1.t) / q) * m_state.th;
		}
		int32 px = x0 + static_cast<int32>(std::lround(static_cast<float>(x1 - x0) * t));
		int32 py = y0 + static_cast<int32>(std::lround(static_cast<float>(y1 - y0) * t));
		ShadePixel(px, py, frag);
	}
}

void CSoftwareRasterizer::DrawTriangle(const VERTEX& va, const VERTEX& vb, const VERTEX& vc)
{
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

	int64 bias0 = IsTopLeft(v1->x, v1->y, v2->x, v2->y) ? 0 : -1;
	int64 bias1 = IsTopLeft(v2->x, v2->y, v0->x, v0->y) ? 0 : -1;
	int64 bias2 = IsTopLeft(v0->x, v0->y, v1->x, v1->y) ? 0 : -1;

	// Edge increments for one pixel (16 subpixels) step along x.
	int64 step0 = -static_cast<int64>(v2->y - v1->y) * 16;
	int64 step1 = -static_cast<int64>(v0->y - v2->y) * 16;
	int64 step2 = -static_cast<int64>(v1->y - v0->y) * 16;

	double invArea = 1.0 / static_cast<double>(area);

	// Flat shading uses the color of the last vertex that was kicked.
	const VERTEX& flat = vc;

	float q0 = (v0->q != 0) ? v0->q : 1.0f;
	float q1 = (v1->q != 0) ? v1->q : 1.0f;
	float q2 = (v2->q != 0) ? v2->q : 1.0f;

	for(int32 y = minY; y <= maxY; y++)
	{
		int32 py = y * 16;
		int32 px = minX * 16;
		int64 e0 = EdgeFunction(v1->x, v1->y, v2->x, v2->y, px, py);
		int64 e1 = EdgeFunction(v2->x, v2->y, v0->x, v0->y, px, py);
		int64 e2 = EdgeFunction(v0->x, v0->y, v1->x, v1->y, px, py);

		for(int32 x = minX; x <= maxX; x++, e0 += step0, e1 += step1, e2 += step2)
		{
			if(((e0 + bias0) < 0) || ((e1 + bias1) < 0) || ((e2 + bias2) < 0)) continue;

			double l0 = static_cast<double>(e0) * invArea;
			double l1 = static_cast<double>(e1) * invArea;
			double l2 = 1.0 - l0 - l1;
			float f0 = static_cast<float>(l0), f1 = static_cast<float>(l1), f2 = static_cast<float>(l2);

			FRAGMENT frag;
			if(s.gouraud)
			{
				frag.r = Clamp255(static_cast<int32>(v0->r * f0 + v1->r * f1 + v2->r * f2 + 0.5f));
				frag.g = Clamp255(static_cast<int32>(v0->g * f0 + v1->g * f1 + v2->g * f2 + 0.5f));
				frag.b = Clamp255(static_cast<int32>(v0->b * f0 + v1->b * f1 + v2->b * f2 + 0.5f));
				frag.a = Clamp255(static_cast<int32>(v0->a * f0 + v1->a * f1 + v2->a * f2 + 0.5f));
			}
			else
			{
				frag.r = flat.r;
				frag.g = flat.g;
				frag.b = flat.b;
				frag.a = flat.a;
			}
			double z = v0->z * l0 + v1->z * l1 + v2->z * l2;
			frag.z = (z <= 0) ? 0 : (z >= 4294967295.0) ? 0xFFFFFFFF : static_cast<uint32>(z);
			frag.fog = Clamp255(static_cast<int32>(v0->fog * f0 + v1->fog * f1 + v2->fog * f2 + 0.5f));

			if(s.textured)
			{
				if(s.fst)
				{
					frag.u = v0->u * f0 + v1->u * f1 + v2->u * f2;
					frag.v = v0->v * f0 + v1->v * f1 + v2->v * f2;
				}
				else
				{
					float q = q0 * f0 + q1 * f1 + q2 * f2;
					if(q == 0) q = 1.0f;
					float ss = v0->s * f0 + v1->s * f1 + v2->s * f2;
					float tt = v0->t * f0 + v1->t * f1 + v2->t * f2;
					frag.u = (ss / q) * s.tw;
					frag.v = (tt / q) * s.th;
				}
			}
			else
			{
				frag.u = frag.v = 0;
			}

			ShadePixel(x, y, frag);
		}
	}
}

void CSoftwareRasterizer::DrawSprite(const VERTEX& va, const VERTEX& vb)
{
	const auto& s = m_state;

	int32 x0 = va.x, x1 = vb.x, y0 = va.y, y1 = vb.y;
	float u0, u1, v0, v1;
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

	float dudx = (u1 - u0) / static_cast<float>(x1 - x0);
	float dvdy = (v1 - v0) / static_cast<float>(y1 - y0);

	FRAGMENT frag;
	frag.r = vb.r;
	frag.g = vb.g;
	frag.b = vb.b;
	frag.a = vb.a;
	frag.z = vb.z;
	frag.fog = vb.fog;

	for(int32 y = startY; y <= endY; y++)
	{
		float v = v0 + dvdy * static_cast<float>(y * 16 - y0);
		for(int32 x = startX; x <= endX; x++)
		{
			FRAGMENT pixel = frag;
			pixel.u = u0 + dudx * static_cast<float>(x * 16 - x0);
			pixel.v = v;
			ShadePixel(x, y, pixel);
		}
	}
}
