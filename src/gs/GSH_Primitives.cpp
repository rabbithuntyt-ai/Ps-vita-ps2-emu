#include "GSH_Primitives.h"
#include <cstring>
#include <vector>

void CGSH_Primitives::WriteRegisterImpl(uint8 registerId, uint64 data)
{
	CGSHandler::WriteRegisterImpl(registerId, data);

	switch(registerId)
	{
	case GS_REG_RGBAQ:
	case GS_REG_ST:
	case GS_REG_UV:
	case GS_REG_FOG:
	case GS_REG_HWREG:
		//Per-vertex data: no effect on rasterizer state.
		break;
	default:
		m_stateDirty = true;
		break;
	}

	switch(registerId)
	{
	case GS_REG_PRIM:
		m_pendingPrim = true;
		m_pendingPrimValue = data;
		break;
	case GS_REG_XYZ2:
	case GS_REG_XYZ3:
	case GS_REG_XYZF2:
	case GS_REG_XYZF3:
		VertexKick(registerId, data);
		break;
	}
}

void CGSH_Primitives::BeginPrimitive(uint64 data)
{
	m_primitiveType = static_cast<uint32>(data & 0x07);
	switch(m_primitiveType)
	{
	case PRIM_POINT:
		m_vtxCount = 1;
		break;
	case PRIM_LINE:
	case PRIM_LINESTRIP:
	case PRIM_SPRITE:
		m_vtxCount = 2;
		break;
	case PRIM_TRIANGLE:
	case PRIM_TRIANGLESTRIP:
	case PRIM_TRIANGLEFAN:
		m_vtxCount = 3;
		break;
	default:
		m_vtxCount = 0;
		break;
	}
}

void CGSH_Primitives::VertexKick(uint8 registerId, uint64 data)
{
	if(m_pendingPrim)
	{
		m_pendingPrim = false;
		BeginPrimitive(m_pendingPrimValue);
	}

	if(m_vtxCount == 0) return;

	bool drawingKick = ((registerId == GS_REG_XYZ2) || (registerId == GS_REG_XYZF2)) && m_drawEnabled && !m_skipDrawing;
	bool fog = (registerId == GS_REG_XYZF2) || (registerId == GS_REG_XYZF3);

	auto& vertex = m_vtxBuffer[m_vtxCount - 1];
	vertex.position = fog ? (data & 0x00FFFFFFFFFFFFFFULL) : data;
	vertex.rgbaq = m_nReg[GS_REG_RGBAQ];
	vertex.uv = m_nReg[GS_REG_UV];
	vertex.st = m_nReg[GS_REG_ST];
	vertex.fog = fog ? static_cast<uint8>(data >> 56) : static_cast<uint8>(m_nReg[GS_REG_FOG] >> 56);

	m_vtxCount--;
	if(m_vtxCount != 0) return;

	if((m_nReg[GS_REG_PRMODECONT] & 1) != 0)
	{
		m_primitiveMode <<= m_nReg[GS_REG_PRIM];
	}
	else
	{
		m_primitiveMode <<= m_nReg[GS_REG_PRMODE];
	}

	if(drawingKick)
	{
		uint64 primitiveMode = m_primitiveMode;
		if(m_stateDirty || (primitiveMode != m_lastPrimitiveMode))
		{
			OnStateChanged();
			m_stateDirty = false;
			m_lastPrimitiveMode = primitiveMode;
		}
		m_primitiveCount++;
	}

	// m_vtxBuffer is filled from the end: [2] is the oldest vertex, [0] the newest.
	switch(m_primitiveType)
	{
	case PRIM_POINT:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[1] = {ConvertVertex(m_vtxBuffer[0])}; OnDraw(CSoftwareRasterizer::PRIMITIVE_POINT, v); }
		m_vtxCount = 1;
		break;
	case PRIM_LINE:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[2] = {ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; OnDraw(CSoftwareRasterizer::PRIMITIVE_LINE, v); }
		m_vtxCount = 2;
		break;
	case PRIM_LINESTRIP:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[2] = {ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; OnDraw(CSoftwareRasterizer::PRIMITIVE_LINE, v); }
		m_vtxBuffer[1] = m_vtxBuffer[0];
		m_vtxCount = 1;
		break;
	case PRIM_TRIANGLE:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[3] = {ConvertVertex(m_vtxBuffer[2]), ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; OnDraw(CSoftwareRasterizer::PRIMITIVE_TRIANGLE, v); }
		m_vtxCount = 3;
		break;
	case PRIM_TRIANGLESTRIP:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[3] = {ConvertVertex(m_vtxBuffer[2]), ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; OnDraw(CSoftwareRasterizer::PRIMITIVE_TRIANGLE, v); }
		m_vtxBuffer[2] = m_vtxBuffer[1];
		m_vtxBuffer[1] = m_vtxBuffer[0];
		m_vtxCount = 1;
		break;
	case PRIM_TRIANGLEFAN:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[3] = {ConvertVertex(m_vtxBuffer[2]), ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; OnDraw(CSoftwareRasterizer::PRIMITIVE_TRIANGLE, v); }
		m_vtxBuffer[1] = m_vtxBuffer[0];
		m_vtxCount = 1;
		break;
	case PRIM_SPRITE:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[2] = {ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; OnDraw(CSoftwareRasterizer::PRIMITIVE_SPRITE, v); }
		m_vtxCount = 2;
		break;
	}
}

CSoftwareRasterizer::VERTEX CGSH_Primitives::ConvertBatchVertex(const Re4Vu1::GS_VERTEX& in) const
{
	unsigned int context = m_primitiveMode.nContext;
	auto offset = make_convertible<XYOFFSET>(m_nReg[GS_REG_XYOFFSET_1 + context]);
	CSoftwareRasterizer::VERTEX out;
	out.x = in.x - static_cast<int32>(offset.nOffsetX);
	out.y = in.y - static_cast<int32>(offset.nOffsetY);
	out.z = in.z;
	out.r = in.rgba[0];
	out.g = in.rgba[1];
	out.b = in.rgba[2];
	out.a = in.rgba[3];
	out.fog = static_cast<uint8>(m_nReg[GS_REG_FOG] >> 56);
	out.s = in.s;
	out.t = in.t;
	out.q = in.q;
	auto uv = make_convertible<UV>(m_nReg[GS_REG_UV]);
	out.u = uv.GetU();
	out.v = uv.GetV();
	return out;
}

bool CGSH_Primitives::BeginBatchStrip()
{
	if(m_pendingPrim)
	{
		m_pendingPrim = false;
		BeginPrimitive(m_pendingPrimValue);
	}
	if((m_nReg[GS_REG_PRMODECONT] & 1) != 0)
	{
		m_primitiveMode <<= m_nReg[GS_REG_PRIM];
	}
	else
	{
		m_primitiveMode <<= m_nReg[GS_REG_PRMODE];
	}
	if(!m_drawEnabled || m_skipDrawing || (m_primitiveType != PRIM_TRIANGLESTRIP)) return false;
	uint64 primitiveMode = m_primitiveMode;
	if(m_stateDirty || (primitiveMode != m_lastPrimitiveMode))
	{
		OnStateChanged();
		m_stateDirty = false;
		m_lastPrimitiveMode = primitiveMode;
	}
	return true;
}

void CGSH_Primitives::EndBatchStrip(const Re4Vu1::GS_VERTEX* secondLast, const Re4Vu1::GS_VERTEX& last)
{
	auto floatBits = [](float value) {
		uint32 bits;
		std::memcpy(&bits, &value, 4);
		return static_cast<uint64>(bits);
	};
	auto kick = [&](const Re4Vu1::GS_VERTEX& g) {
		m_nReg[GS_REG_ST] = floatBits(g.s) | (floatBits(g.t) << 32);
		m_nReg[GS_REG_RGBAQ] = static_cast<uint64>(g.rgba[0]) | (static_cast<uint64>(g.rgba[1]) << 8) | (static_cast<uint64>(g.rgba[2]) << 16) |
		                       (static_cast<uint64>(g.rgba[3]) << 24) | (floatBits(g.q) << 32);
		uint64 xyz = static_cast<uint64>(static_cast<uint16>(g.x)) | (static_cast<uint64>(static_cast<uint16>(g.y)) << 16) | (static_cast<uint64>(g.z) << 32);
		VertexKick(GS_REG_XYZ3, xyz); //no drawing
	};
	if(secondLast) kick(*secondLast);
	kick(last);
	m_primitiveCount += m_batchPrimitives;
	m_batchPrimitives = 0;
}

void CGSH_Primitives::DrawDirectTriangle(const CSoftwareRasterizer::VERTEX* vertices)
{
	if(!m_drawEnabled || m_skipDrawing) return;
	uint64 primitiveMode = m_primitiveMode;
	if(m_stateDirty || (primitiveMode != m_lastPrimitiveMode))
	{
		OnStateChanged();
		m_stateDirty = false;
		m_lastPrimitiveMode = primitiveMode;
	}
	m_primitiveCount++;
	OnDraw(CSoftwareRasterizer::PRIMITIVE_TRIANGLE, vertices);
}

void CGSH_Primitives::DrawWorldBatch(const Re4Vu1::WORLD_BATCH& batch)
{
	g_worldBatches++;
	g_worldVertices += batch.count;
	struct TIMER
	{
		uint64 start = g_clockUs ? g_clockUs() : 0;
		~TIMER()
		{
			if(g_clockUs) g_worldUs += g_clockUs() - start;
		}
	} timer;
	if(batch.count == 0) return;
	std::vector<Re4Vu1::WORLD_VERTEX> vertices(batch.count);
	std::vector<Re4Vu1::GS_VERTEX> projected(batch.count);
	for(uint32 i = 0; i < batch.count; i++)
	{
		vertices[i] = Re4Vu1::ReadWorldVertex(batch, i);
		projected[i] = Re4Vu1::Project(vertices[i]);
	}
	auto floatBits = [](float value) {
		uint32 bits;
		std::memcpy(&bits, &value, 4);
		return static_cast<uint64>(bits);
	};
	for(uint32 i = 0; i < batch.count; i++)
	{
		const auto& v = vertices[i];
		const auto& g = projected[i];
		bool clipped = false, back = false;
		if(i >= 2)
		{
			clipped = (vertices[i - 2].clipFlags | vertices[i - 1].clipFlags | v.clipFlags) != 0;
			if(!clipped)
			{
				const auto &g0 = projected[i - 2], &g1 = projected[i - 1];
				float ax = g1.screenX - g0.screenX, ay = g1.screenY - g0.screenY;
				float bx = g.screenX - g0.screenX, by = g.screenY - g0.screenY;
				back = (2.0f * (bx * ay - by * ax) * v.winding) < 0;
			}
		}
		bool adc = v.restart || back || clipped;
		// The registers as the program's packet leaves them, and the kick (XYZ3: no drawing)
		m_nReg[GS_REG_ST] = floatBits(g.s) | (floatBits(g.t) << 32);
		m_nReg[GS_REG_RGBAQ] = static_cast<uint64>(g.rgba[0]) | (static_cast<uint64>(g.rgba[1]) << 8) | (static_cast<uint64>(g.rgba[2]) << 16) |
		                       (static_cast<uint64>(g.rgba[3]) << 24) | (floatBits(g.q) << 32);
		uint64 xyz = static_cast<uint64>(static_cast<uint16>(g.x)) | (static_cast<uint64>(static_cast<uint16>(g.y)) << 16) | (static_cast<uint64>(g.z) << 32);
		VertexKick(adc ? GS_REG_XYZ3 : GS_REG_XYZ2, xyz);
		if(clipped && !v.restart) DrawClippedWorldTriangle(&vertices[i - 2]);
	}
}

void CGSH_Primitives::DrawClippedWorldTriangle(const Re4Vu1::WORLD_VERTEX* last3)
{
	// The program's clipping routine: the triangle clipped to the clip volume, culled like
	// the others, drawn as a fan.
	Re4Vu1::WORLD_VERTEX polygon[12];
	uint32 count = Re4Vu1::ClipTriangle(last3, polygon);
	if(count < 3) return;
	g_worldClipped++;
	Re4Vu1::GS_VERTEX out[12];
	for(uint32 k = 0; k < count; k++) out[k] = Re4Vu1::Project(polygon[k]);
	float ax = out[1].screenX - out[0].screenX, ay = out[1].screenY - out[0].screenY;
	float bx = out[2].screenX - out[0].screenX, by = out[2].screenY - out[0].screenY;
	if((2.0f * (bx * ay - by * ax) * last3[2].winding) < 0) return;
	for(uint32 k = 1; k + 1 < count; k++)
	{
		CSoftwareRasterizer::VERTEX triangle[3] = {ConvertBatchVertex(out[0]), ConvertBatchVertex(out[k]), ConvertBatchVertex(out[k + 1])};
		DrawDirectTriangle(triangle);
	}
}

CSoftwareRasterizer::VERTEX CGSH_Primitives::ConvertVertex(const VERTEX& in) const
{
	unsigned int context = m_primitiveMode.nContext;
	auto offset = make_convertible<XYOFFSET>(m_nReg[GS_REG_XYOFFSET_1 + context]);
	auto xyz = make_convertible<XYZ>(in.position);
	auto rgbaq = make_convertible<RGBAQ>(in.rgbaq);
	auto uv = make_convertible<UV>(in.uv);
	auto st = make_convertible<ST>(in.st);

	CSoftwareRasterizer::VERTEX out;
	out.x = static_cast<int32>(xyz.nX) - static_cast<int32>(offset.nOffsetX);
	out.y = static_cast<int32>(xyz.nY) - static_cast<int32>(offset.nOffsetY);
	// XYZF kicks had their fog byte stripped in VertexKick, leaving a 24-bit Z.
	out.z = static_cast<uint32>(in.position >> 32);
	out.r = rgbaq.nR;
	out.g = rgbaq.nG;
	out.b = rgbaq.nB;
	out.a = rgbaq.nA;
	out.fog = in.fog;
	out.s = st.nS;
	out.t = st.nT;
	out.q = rgbaq.nQ;
	out.u = uv.GetU();
	out.v = uv.GetV();
	return out;
}

CSoftwareRasterizer::STATE CGSH_Primitives::DecodeState() const
{
	unsigned int context = m_primitiveMode.nContext;

	auto frame = make_convertible<FRAME>(m_nReg[GS_REG_FRAME_1 + context]);
	auto zbuf = make_convertible<ZBUF>(m_nReg[GS_REG_ZBUF_1 + context]);
	auto scissor = make_convertible<SCISSOR>(m_nReg[GS_REG_SCISSOR_1 + context]);
	auto test = make_convertible<TEST>(m_nReg[GS_REG_TEST_1 + context]);
	auto alpha = make_convertible<ALPHA>(m_nReg[GS_REG_ALPHA_1 + context]);
	auto tex0 = make_convertible<TEX0>(m_nReg[GS_REG_TEX0_1 + context]);
	auto tex1 = make_convertible<TEX1>(m_nReg[GS_REG_TEX1_1 + context]);
	auto clamp = make_convertible<CLAMP>(m_nReg[GS_REG_CLAMP_1 + context]);
	auto texa = make_convertible<TEXA>(m_nReg[GS_REG_TEXA]);
	auto fogcol = make_convertible<FOGCOL>(m_nReg[GS_REG_FOGCOL]);

	CSoftwareRasterizer::STATE state;
	state.gouraud = m_primitiveMode.nShading != 0;
	state.textured = m_primitiveMode.nTexture != 0;
	state.fog = m_primitiveMode.nFog != 0;
	state.alphaBlend = m_primitiveMode.nAlpha != 0;
	state.fst = m_primitiveMode.nUseUV != 0;

	state.fbp = frame.GetBasePtr();
	state.fbw = frame.nWidth;
	state.fpsm = frame.nPsm;
	state.fbmsk = frame.nMask;
	state.zbp = zbuf.GetBasePtr();
	state.zpsm = zbuf.nPsm;
	state.zmsk = zbuf.nMask != 0;

	state.scax0 = scissor.scax0;
	state.scax1 = scissor.scax1;
	state.scay0 = scissor.scay0;
	state.scay1 = scissor.scay1;

	state.ate = test.nAlphaEnabled != 0;
	state.atst = test.nAlphaMethod;
	state.aref = test.nAlphaRef;
	state.afail = test.nAlphaFail;
	state.date = test.nDestAlphaEnabled != 0;
	state.datm = test.nDestAlphaMode != 0;
	state.zte = test.nDepthEnabled != 0;
	state.ztst = test.nDepthMethod;

	state.blendA = alpha.nA;
	state.blendB = alpha.nB;
	state.blendC = alpha.nC;
	state.blendD = alpha.nD;
	state.blendFix = alpha.nFix;
	state.pabe = (m_nReg[GS_REG_PABE] & 1) != 0;
	state.fba = (m_nReg[GS_REG_FBA_1 + context] & 1) != 0;
	state.colClamp = (m_nReg[GS_REG_COLCLAMP] & 1) != 0;
	state.fogColor = fogcol.nFCR | (fogcol.nFCG << 8) | (fogcol.nFCB << 16);

	state.tbp = tex0.GetBufPtr();
	state.tbw = tex0.nBufWidth;
	state.tpsm = tex0.nPsm;
	state.tw = std::min<uint32>(tex0.GetWidth(), TEX0_MAX_TEXTURE_SIZE);
	state.th = std::min<uint32>(tex0.GetHeight(), TEX0_MAX_TEXTURE_SIZE);
	state.tcc = tex0.nColorComp != 0;
	state.tfx = tex0.nFunction;
	state.cpsm = tex0.nCPSM;
	state.csa = tex0.nCSA;
	state.bilinear = tex1.nMagFilter == MAG_FILTER_LINEAR;
	state.wms = clamp.nWMS;
	state.wmt = clamp.nWMT;
	state.minu = clamp.GetMinU();
	state.maxu = clamp.GetMaxU();
	state.minv = clamp.GetMinV();
	state.maxv = clamp.GetMaxV();
	state.ta0 = texa.nTA0;
	state.ta1 = texa.nTA1;
	state.aem = texa.nAEM != 0;

	return state;
}

