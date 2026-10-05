#include "GSH_Software.h"
#include "GsMemory.h"
#include "gs/GsTransferRange.h"
#include "xxhash.h"
#include "ThreadProfiler.h"
#include <chrono>
#include <cstring>
#include <cstdlib>

CGSH_Software::CGSH_Software(bool gsThreaded)
    : CGSH_Primitives(gsThreaded)
{
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
	if(m_gsThreaded) ThreadProfiler::RegisterCurrentThread("GS");
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

namespace
{
	inline uint64 NowMicros()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}
}

void CGSH_Software::FlushRendering()
{
	if(!m_parallel) return;
	uint64 start = NowMicros();
	m_parallel->Flush();
	m_frameRasterMicros += NowMicros() - start;
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
	m_lastFrameRasterMicros = static_cast<uint32>(std::min<uint64>(m_frameRasterMicros, 0xFFFFFFFF));
	m_frameRasterMicros = 0;
	m_frameCounter++;
	m_skipDrawing = (m_frameSkip != 0) && ((m_frameCounter % (m_frameSkip + 1)) != 0);
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
	ResetPrimitiveState();
	FlushRendering();
	m_rasterizer.NotifyAllMemoryWritten();
}

void CGSH_Software::OnStateChanged()
{
	m_rasterizer.SetState(DecodeState());
	if(m_parallel) m_parallel->OnStateChanged();
}

void CGSH_Software::OnDraw(CSoftwareRasterizer::PRIMITIVE_KIND kind, const CSoftwareRasterizer::VERTEX* vertices)
{
	Submit(kind, vertices);
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
		GS_SURFACE surface;
		bool fast = surface.Init(layer.psm, layer.bufPtr, bufWidth) && !surface.nibbles;
		bool is16 = GsMemory::IsPsm16(layer.psm);
		bool is32 = (layer.psm == PSMCT32) || (layer.psm == PSMCT24) || (layer.psm == PSMCT24_UNK) || (layer.psm == PSMCT32_UNK);
		for(uint32 y = 0; y < height; y++)
		{
			uint32* dst = m_frameBuffer.data() + y * width;
			uint32 sy = (layer.offsetY + y) & 2047;
			if(fast && (is16 || is32))
			{
				uint32 rowBase = surface.RowBase(sy);
				const uint32* rowTable = surface.RowTable(sy);
				for(uint32 x = 0; x < width; x++)
				{
					uint32 offset = surface.Offset(rowBase, rowTable, (layer.offsetX + x) & 2047);
					uint32 color = is16 ? GsMemory::Color16To32(*reinterpret_cast<const uint16*>(m_pRAM + offset))
					                    : *reinterpret_cast<const uint32*>(m_pRAM + offset);
					dst[x] = color | 0xFF000000;
				}
			}
			else
			{
				for(uint32 x = 0; x < width; x++)
				{
					uint32 color = CSoftwareRasterizer::ReadColor32(m_pRAM, layer.psm, layer.bufPtr, bufWidth,
					                                                layer.offsetX + x, sy);
					dst[x] = color | 0xFF000000;
				}
			}
		}
		m_frameSink(m_frameBuffer, width, height);
	}
	CGSHandler::FlipImpl(dispInfo);
}
