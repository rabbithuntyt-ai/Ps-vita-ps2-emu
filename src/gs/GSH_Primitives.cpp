#include "GSH_Primitives.h"

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

