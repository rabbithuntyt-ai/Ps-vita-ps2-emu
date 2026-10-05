#include "GSH_Software.h"
#include "GsMemory.h"
#include "gs/GsTransferRange.h"
#include "xxhash.h"
#include <cstring>
#include <cstdlib>

CGSH_Software::CGSH_Software(bool gsThreaded)
    : CGSHandler(gsThreaded)
{
	m_primitiveMode <<= 0;
}

CGSHandler::FactoryFunction CGSH_Software::GetFactoryFunction(FrameSink sink, const OPTIONS& options)
{
	return [sink, options]() {
		auto handler = new CGSH_Software(options.gsThreaded);
		handler->SetFrameSink(sink);
		handler->SetRasterizerThreads(options.rasterizerThreads);
		handler->SetInterlacedRendering(options.interlacedRendering);
		handler->SetFrameSkip(options.frameSkip);
		return handler;
	};
}

void CGSH_Software::SetFrameSink(FrameSink sink)
{
	m_frameSink = std::move(sink);
}

void CGSH_Software::InitializeImpl()
{
	m_rasterizer.SetMemory(m_pRAM, m_pCLUT);
	m_rasterizer.SetClutHash(XXH3_64bits(m_pCLUT, CLUTSIZE));
	if(m_rasterizerThreads > 1)
	{
		m_parallel = std::make_unique<CParallelRasterizer>(m_rasterizer, m_pRAM, m_pCLUT, m_rasterizerThreads);
	}
}

void CGSH_Software::SetRasterizerThreads(uint32 threads)
{
	m_rasterizerThreads = std::max<uint32>(threads, 1);
}

void CGSH_Software::FlushRendering()
{
	if(m_parallel) m_parallel->Flush();
}

void CGSH_Software::Submit(CSoftwareRasterizer::PRIMITIVE_KIND kind, const CSoftwareRasterizer::VERTEX* vertices)
{
	if(m_parallel)
	{
		m_parallel->Submit(kind, vertices);
		return;
	}
	switch(kind)
	{
	case CSoftwareRasterizer::PRIMITIVE_POINT:
		m_rasterizer.DrawPoint(vertices[0]);
		break;
	case CSoftwareRasterizer::PRIMITIVE_LINE:
		m_rasterizer.DrawLine(vertices[0], vertices[1]);
		break;
	case CSoftwareRasterizer::PRIMITIVE_TRIANGLE:
		m_rasterizer.DrawTriangle(vertices[0], vertices[1], vertices[2]);
		break;
	case CSoftwareRasterizer::PRIMITIVE_SPRITE:
		m_rasterizer.DrawSprite(vertices[0], vertices[1]);
		break;
	}
}

void CGSH_Software::SetInterlacedRendering(bool enabled)
{
	m_interlaced = enabled;
	UpdateRowFilter();
}

void CGSH_Software::SetFrameSkip(uint32 frameSkip)
{
	m_frameSkip = frameSkip;
}

void CGSH_Software::UpdateRowFilter()
{
	FlushRendering();
	m_rasterizer.SetRowFilter(m_interlaced ? 1 : 0, m_frameCounter & 1);
}

void CGSH_Software::MarkNewFrame()
{
	FlushRendering();
	m_frameCounter++;
	m_skipThisFrame = (m_frameSkip != 0) && ((m_frameCounter % (m_frameSkip + 1)) != 0);
	UpdateRowFilter();
	CGSHandler::MarkNewFrame();
}

void CGSH_Software::TransferWrite(const uint8* data, uint32 length)
{
	FlushRendering();
	CGSHandler::TransferWrite(data, length);
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	auto [start, size] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);
	m_rasterizer.NotifyMemoryWrite(start, size);
}

void CGSH_Software::SyncCLUT(const TEX0& tex0)
{
	// A CLUT load reads GS memory, which pending primitives may still write.
	if(tex0.nCLD != 0) FlushRendering();
	CGSHandler::SyncCLUT(tex0);
	m_rasterizer.SetClutHash(XXH3_64bits(m_pCLUT, CLUTSIZE));
	if(m_parallel) m_parallel->OnStateChanged();
}

void CGSH_Software::ReleaseImpl()
{
	FlushRendering();
	m_parallel.reset();
}

void CGSH_Software::ResetImpl()
{
	m_vtxCount = 0;
	m_primitiveType = PRIM_INVALID;
	m_pendingPrim = false;
	m_pendingPrimValue = 0;
	m_primitiveCount = 0;
	m_stateDirty = true;
	FlushRendering();
	m_rasterizer.NotifyAllMemoryWritten();
}

void CGSH_Software::WriteRegisterImpl(uint8 registerId, uint64 data)
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

void CGSH_Software::BeginPrimitive(uint64 data)
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

void CGSH_Software::VertexKick(uint8 registerId, uint64 data)
{
	if(m_pendingPrim)
	{
		m_pendingPrim = false;
		BeginPrimitive(m_pendingPrimValue);
	}

	if(m_vtxCount == 0) return;

	bool drawingKick = ((registerId == GS_REG_XYZ2) || (registerId == GS_REG_XYZF2)) && m_drawEnabled && !m_skipThisFrame;
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
			BuildState();
			m_stateDirty = false;
			m_lastPrimitiveMode = primitiveMode;
		}
		m_primitiveCount++;
	}

	// m_vtxBuffer is filled from the end: [2] is the oldest vertex, [0] the newest.
	switch(m_primitiveType)
	{
	case PRIM_POINT:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[1] = {ConvertVertex(m_vtxBuffer[0])}; Submit(CSoftwareRasterizer::PRIMITIVE_POINT, v); }
		m_vtxCount = 1;
		break;
	case PRIM_LINE:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[2] = {ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; Submit(CSoftwareRasterizer::PRIMITIVE_LINE, v); }
		m_vtxCount = 2;
		break;
	case PRIM_LINESTRIP:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[2] = {ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; Submit(CSoftwareRasterizer::PRIMITIVE_LINE, v); }
		m_vtxBuffer[1] = m_vtxBuffer[0];
		m_vtxCount = 1;
		break;
	case PRIM_TRIANGLE:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[3] = {ConvertVertex(m_vtxBuffer[2]), ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; Submit(CSoftwareRasterizer::PRIMITIVE_TRIANGLE, v); }
		m_vtxCount = 3;
		break;
	case PRIM_TRIANGLESTRIP:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[3] = {ConvertVertex(m_vtxBuffer[2]), ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; Submit(CSoftwareRasterizer::PRIMITIVE_TRIANGLE, v); }
		m_vtxBuffer[2] = m_vtxBuffer[1];
		m_vtxBuffer[1] = m_vtxBuffer[0];
		m_vtxCount = 1;
		break;
	case PRIM_TRIANGLEFAN:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[3] = {ConvertVertex(m_vtxBuffer[2]), ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; Submit(CSoftwareRasterizer::PRIMITIVE_TRIANGLE, v); }
		m_vtxBuffer[1] = m_vtxBuffer[0];
		m_vtxCount = 1;
		break;
	case PRIM_SPRITE:
		if(drawingKick) { CSoftwareRasterizer::VERTEX v[2] = {ConvertVertex(m_vtxBuffer[1]), ConvertVertex(m_vtxBuffer[0])}; Submit(CSoftwareRasterizer::PRIMITIVE_SPRITE, v); }
		m_vtxCount = 2;
		break;
	}
}

CSoftwareRasterizer::VERTEX CGSH_Software::ConvertVertex(const VERTEX& in) const
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

void CGSH_Software::BuildState()
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

	m_rasterizer.SetState(state);
	if(m_parallel) m_parallel->OnStateChanged();
}

void CGSH_Software::ProcessHostToLocalTransfer()
{
	// CGSHandler already wrote the image data into GS RAM, which is all this
	// renderer reads from.
}

void CGSH_Software::ProcessLocalToHostTransfer()
{
	FlushRendering();
	// Reads are served by CGSHandler straight from GS RAM.
}

void CGSH_Software::ProcessLocalToLocalTransfer()
{
	FlushRendering();
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);

	uint32 width = trxReg.nRRW;
	uint32 height = trxReg.nRRH;
	if((width == 0) || (height == 0)) return;

	auto [dstStart, dstSize] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);
	m_rasterizer.NotifyMemoryWrite(dstStart, dstSize);

	// Gather first so that overlapping source/destination areas behave.
	std::vector<uint32> pixels(width * height);
	for(uint32 y = 0; y < height; y++)
	{
		for(uint32 x = 0; x < width; x++)
		{
			pixels[x + y * width] = GsMemory::ReadRaw(m_pRAM, bltBuf.nSrcPsm, bltBuf.GetSrcPtr(), bltBuf.nSrcWidth,
			                                          trxPos.nSSAX + x, trxPos.nSSAY + y);
		}
	}
	for(uint32 y = 0; y < height; y++)
	{
		for(uint32 x = 0; x < width; x++)
		{
			GsMemory::WriteRaw(m_pRAM, bltBuf.nDstPsm, bltBuf.GetDstPtr(), bltBuf.nDstWidth,
			                   trxPos.nDSAX + x, trxPos.nDSAY + y, pixels[x + y * width]);
		}
	}
}

void CGSH_Software::ProcessClutTransfer(uint32, uint32)
{
}

void CGSH_Software::FlipImpl(const DISPLAY_INFO& dispInfo)
{
	FlushRendering();
	const auto& layer = dispInfo.layers[0];
	if(m_frameSink && layer.enabled && (dispInfo.width != 0) && (dispInfo.height != 0))
	{
		uint32 width = std::min<uint32>(dispInfo.width, 1024);
		uint32 height = std::min<uint32>(dispInfo.height, 1024);
		m_frameBuffer.resize(width * height);
		uint32 bufWidth = layer.bufWidth / 64;
		for(uint32 y = 0; y < height; y++)
		{
			for(uint32 x = 0; x < width; x++)
			{
				uint32 color = CSoftwareRasterizer::ReadColor32(m_pRAM, layer.psm, layer.bufPtr, bufWidth,
				                                                layer.offsetX + x, layer.offsetY + y);
				m_frameBuffer[x + y * width] = color | 0xFF000000;
			}
		}
		m_frameSink(m_frameBuffer.data(), width, height);
	}
	CGSHandler::FlipImpl(dispInfo);
}
