#include "GSH_Hardware.h"
#include <algorithm>
#include <cstring>
#include "GsMemory.h"
#include "GsSurface.h"
#include "gs/GsTransferRange.h"
#include "xxhash.h"

namespace
{
	bool IsPsm16(uint32 psm)
	{
		return (psm == CGSHandler::PSMCT16) || (psm == CGSHandler::PSMCT16S);
	}

	bool IsColorPsm(uint32 psm)
	{
		switch(psm)
		{
		case CGSHandler::PSMCT32:
		case CGSHandler::PSMCT24:
		case CGSHandler::PSMCT16:
		case CGSHandler::PSMCT16S:
		case CGSHandler::PSMCT32_UNK:
		case CGSHandler::PSMCT24_UNK:
			return true;
		default:
			return false;
		}
	}

	// Formats that share memory layout can share a render target.
	uint32 PsmClass(uint32 psm)
	{
		return IsPsm16(psm) ? 16 : 32;
	}

	uint32 ZMaxForPsm(uint32 zpsm)
	{
		switch(zpsm & 0xF)
		{
		case 0x0:
			return 0xFFFFFFFF;
		case 0x1:
			return 0x00FFFFFF;
		default:
			return 0x0000FFFF;
		}
	}

	// GS alpha (0x80 = 1.0) <-> render target alpha (1.0 stored as 255).
	inline uint8 GsAlphaToTarget(uint32 alpha)
	{
		return static_cast<uint8>(std::min<uint32>(alpha * 2, 255));
	}

	inline uint32 TargetAlphaToGs(uint32 alpha)
	{
		return (alpha + 1) / 2;
	}

	uint32 RoundUpTargetHeight(uint32 height)
	{
		if(height <= 256) return 256;
		if(height <= 512) return 512;
		return 1024;
	}
}

CGSH_Hardware::CGSH_Hardware(const OPTIONS& options)
    : CGSH_Primitives(false)
    , m_options(options)
{
}

CGSH_Hardware::~CGSH_Hardware()
{
}

CGSHandler::FactoryFunction CGSH_Hardware::GetFactoryFunction(const OPTIONS& options)
{
	return [options]() { return new CGSH_Hardware(options); };
}

//-----------------------------------------------------------------------------
// Lifetime
//-----------------------------------------------------------------------------

bool CGSH_Hardware::Pump(uint32 timeoutMs)
{
	m_mailBox.WaitForCall(timeoutMs);
	// The frontend draws on the same context between pumps.
	m_stateApplied = false;
	while(m_mailBox.IsPending() && !m_flipped)
	{
		m_mailBox.ReceiveCall();
	}
	FlushBatch();
	bool flipped = m_flipped;
	m_flipped = false;
	return flipped;
}

void CGSH_Hardware::ReleaseGpu()
{
	ReleaseImpl();
}

void CGSH_Hardware::InitializeImpl()
{
	if(m_gpuInitialized) return;
	m_gpuInitialized = true;
	m_textureCache.SetMemory(m_pRAM, m_pCLUT);
	m_clutHash = XXH3_64bits(m_pCLUT, CLUTSIZE);

	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glMatrixMode(GL_TEXTURE);
	glLoadIdentity();
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);

	glGenTextures(1, &m_displayUploadTexture);
	glBindTexture(GL_TEXTURE_2D, m_displayUploadTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	std::vector<uint32> empty(1024 * 1024, 0xFF000000);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, empty.data());

	// Untextured draws still go through the texture combiner (for the alpha
	// scale), sampling a 1x1 white texture.
	CACHED_TEXTURE white;
	glGenTextures(1, &white.texture);
	glBindTexture(GL_TEXTURE_2D, white.texture);
	uint32 whiteTexel = 0xFFFFFFFF;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &whiteTexel);
	m_glTextures[0] = white;
}

void CGSH_Hardware::ReleaseImpl()
{
	if(!m_gpuInitialized) return;
	m_gpuInitialized = false;
	FlushBatch();
	for(auto& target : m_targets)
	{
		DeleteTarget(*target);
	}
	m_targets.clear();
	for(auto& texture : m_glTextures)
	{
		glDeleteTextures(1, &texture.second.texture);
	}
	m_glTextures.clear();
	if(m_displayUploadTexture) glDeleteTextures(1, &m_displayUploadTexture);
	m_displayUploadTexture = 0;
}

void CGSH_Hardware::ResetImpl()
{
	FlushBatch();
	ResetPrimitiveState();
	for(auto& target : m_targets)
	{
		DeleteTarget(*target);
	}
	m_targets.clear();
	m_currentTarget = nullptr;
	m_stateApplied = false;
	m_textureCache.MarkAllWritten();
}

void CGSH_Hardware::MarkNewFrame()
{
	FlushBatch();
	m_lastFrameStats = m_stats;
	m_stats = STATS();
	CGSH_Primitives::MarkNewFrame();
}

//-----------------------------------------------------------------------------
// Render targets
//-----------------------------------------------------------------------------

uint32 CGSH_Hardware::TargetBytes(const TARGET& target) const
{
	GS_SURFACE surface;
	surface.Init(target.psm, target.fbp, target.fbw);
	uint32 rows = (target.height + surface.phMask) >> surface.phShift;
	return rows * std::max<uint32>(surface.pagesPerRow, 1) * 8192;
}

bool CGSH_Hardware::RangesOverlap(const TARGET& target, uint32 start, uint32 size) const
{
	uint32 targetStart = target.fbp;
	uint32 targetEnd = target.fbp + TargetBytes(target);
	uint32 end = start + size;
	return (start < targetEnd) && (targetStart < end);
}

void CGSH_Hardware::DeleteTarget(TARGET& target)
{
	if(m_currentTarget == &target) m_currentTarget = nullptr;
	if(target.framebuffer) glDeleteFramebuffers(1, &target.framebuffer);
	if(target.colorTexture) glDeleteTextures(1, &target.colorTexture);
	if(target.depthBuffer) glDeleteRenderbuffers(1, &target.depthBuffer);
	target.framebuffer = target.colorTexture = target.depthBuffer = 0;
}

CGSH_Hardware::TARGET* CGSH_Hardware::FindTarget(uint32 fbp, uint32 fbw, uint32 psm, bool create, uint32 minHeight)
{
	if(!IsColorPsm(psm) || (fbw == 0)) return nullptr;
	minHeight = std::min<uint32>(minHeight, 1024);

	for(auto& target : m_targets)
	{
		if((target->fbp == fbp) && (target->fbw == fbw) && (PsmClass(target->psm) == PsmClass(psm)))
		{
			if(target->height >= minHeight)
			{
				target->psm = psm;
				target->lastUse = ++m_useCounter;
				return target.get();
			}
			break; //too small: recreated below
		}
	}
	if(!create) return nullptr;

	FlushBatch();

	auto target = std::make_unique<TARGET>();
	target->fbp = fbp;
	target->fbw = fbw;
	target->psm = psm;
	target->width = std::min<uint32>(fbw * 64, 1024);
	target->height = RoundUpTargetHeight(std::max<uint32>(minHeight, 448));
	uint32 size = TargetBytes(*target);

	// Anything already overlapping this memory goes back to GS memory first,
	// then the new target starts from GS memory contents.
	for(auto it = m_targets.begin(); it != m_targets.end();)
	{
		if(RangesOverlap(**it, fbp, size))
		{
			if((*it)->gpuDirty) DownloadTarget(**it);
			DeleteTarget(**it);
			it = m_targets.erase(it);
		}
		else
		{
			++it;
		}
	}

	glGenTextures(1, &target->colorTexture);
	glBindTexture(GL_TEXTURE_2D, target->colorTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target->width, target->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	glGenRenderbuffers(1, &target->depthBuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, target->depthBuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, target->width, target->height);

	glGenFramebuffers(1, &target->framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target->colorTexture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target->depthBuffer);
	glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_TRUE);
	glClearDepth(0.0);
	glClear(GL_DEPTH_BUFFER_BIT);

	target->lastUse = ++m_useCounter;
	m_targets.push_back(std::move(target));
	TARGET* result = m_targets.back().get();
	UploadTarget(*result, 0, 0, result->width, result->height);
	m_stateApplied = false;
	return result;
}

void CGSH_Hardware::UploadTarget(TARGET& target, uint32 x0, uint32 y0, uint32 width, uint32 height)
{
	x0 = std::min(x0, target.width);
	y0 = std::min(y0, target.height);
	width = std::min(width, target.width - x0);
	height = std::min(height, target.height - y0);
	if((width == 0) || (height == 0)) return;

	GS_SURFACE surface;
	surface.Init(target.psm, target.fbp, target.fbw);
	bool is16 = IsPsm16(target.psm);
	bool is24 = GsMemory::IsPsm24(target.psm);
	std::vector<uint32> pixels(width * height);
	for(uint32 y = 0; y < height; y++)
	{
		uint32 rowBase = surface.RowBase(y0 + y);
		const uint32* rowTable = surface.RowTable(y0 + y);
		for(uint32 x = 0; x < width; x++)
		{
			uint32 offset = surface.Offset(rowBase, rowTable, x0 + x);
			uint32 color = is16 ? GsMemory::Color16To32(*reinterpret_cast<const uint16*>(m_pRAM + offset))
			                    : *reinterpret_cast<const uint32*>(m_pRAM + offset);
			uint32 alpha = is24 ? 0x80 : (color >> 24);
			pixels[x + y * width] = (color & 0x00FFFFFF) | (static_cast<uint32>(GsAlphaToTarget(alpha)) << 24);
		}
	}
	glBindTexture(GL_TEXTURE_2D, target.colorTexture);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	m_stats.targetUploads++;
	m_stateApplied = false;
}

void CGSH_Hardware::DownloadTarget(TARGET& target)
{
	FlushBatch();
	std::vector<uint32> pixels(target.width * target.height);
	glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
	glReadPixels(0, 0, target.width, target.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

	GS_SURFACE surface;
	surface.Init(target.psm, target.fbp, target.fbw);
	bool is16 = IsPsm16(target.psm);
	bool is24 = GsMemory::IsPsm24(target.psm);
	for(uint32 y = 0; y < target.height; y++)
	{
		uint32 rowBase = surface.RowBase(y);
		const uint32* rowTable = surface.RowTable(y);
		for(uint32 x = 0; x < target.width; x++)
		{
			uint32 color = pixels[x + y * target.width];
			uint32 gsColor = (color & 0x00FFFFFF) | (TargetAlphaToGs(color >> 24) << 24);
			uint32 offset = surface.Offset(rowBase, rowTable, x);
			if(is16)
			{
				*reinterpret_cast<uint16*>(m_pRAM + offset) = static_cast<uint16>(GsMemory::Color32To16(gsColor));
			}
			else if(is24)
			{
				auto pixel = reinterpret_cast<uint32*>(m_pRAM + offset);
				*pixel = (*pixel & 0xFF000000) | (gsColor & 0x00FFFFFF);
			}
			else
			{
				*reinterpret_cast<uint32*>(m_pRAM + offset) = gsColor;
			}
		}
	}
	m_textureCache.MarkBytesWritten(target.fbp, TargetBytes(target));
	target.gpuDirty = false;
	m_stats.targetDownloads++;
	m_stateApplied = false;
}

void CGSH_Hardware::DownloadTargetsOverlapping(uint32 start, uint32 size)
{
	for(auto& target : m_targets)
	{
		if(target->gpuDirty && RangesOverlap(*target, start, size)) DownloadTarget(*target);
	}
}

void CGSH_Hardware::UploadTargetsOverlapping(uint32 start, uint32 size)
{
	for(auto& target : m_targets)
	{
		if(RangesOverlap(*target, start, size)) UploadTarget(*target, 0, 0, target->width, target->height);
	}
}

//-----------------------------------------------------------------------------
// GS memory coherency
//-----------------------------------------------------------------------------

void CGSH_Hardware::BeginTransferWrite()
{
	FlushBatch();
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	auto [start, size] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);
	// Keep pixels the GPU rendered around the transferred rectangle.
	DownloadTargetsOverlapping(start, size);
	CGSH_Primitives::BeginTransferWrite();
}

void CGSH_Hardware::ProcessHostToLocalTransfer()
{
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	auto [start, size] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);
	m_textureCache.MarkBytesWritten(start, size);

	for(auto& target : m_targets)
	{
		if(!RangesOverlap(*target, start, size)) continue;
		bool sameLayout = (target->fbp == bltBuf.GetDstPtr()) && (target->fbw == bltBuf.nDstWidth) &&
		                  (PsmClass(target->psm) == PsmClass(bltBuf.nDstPsm)) && IsColorPsm(bltBuf.nDstPsm);
		if(sameLayout)
		{
			UploadTarget(*target, trxPos.nDSAX, trxPos.nDSAY, trxReg.nRRW, trxReg.nRRH);
		}
		else
		{
			UploadTarget(*target, 0, 0, target->width, target->height);
		}
	}
	m_stateApplied = false;
}

void CGSH_Hardware::ProcessLocalToHostTransfer()
{
	FlushBatch();
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	auto [start, size] = GsTransfer::GetSrcRange(bltBuf, trxReg, trxPos);
	DownloadTargetsOverlapping(start, size);
}

void CGSH_Hardware::ProcessLocalToLocalTransfer()
{
	FlushBatch();
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	auto [srcStart, srcSize] = GsTransfer::GetSrcRange(bltBuf, trxReg, trxPos);
	auto [dstStart, dstSize] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);
	DownloadTargetsOverlapping(srcStart, srcSize);
	DownloadTargetsOverlapping(dstStart, dstSize);

	uint32 width = trxReg.nRRW;
	uint32 height = trxReg.nRRH;
	if((width != 0) && (height != 0))
	{
		std::vector<uint32> pixels(width * height);
		for(uint32 y = 0; y < height; y++)
			for(uint32 x = 0; x < width; x++)
				pixels[x + y * width] = GsMemory::ReadRaw(m_pRAM, bltBuf.nSrcPsm, bltBuf.GetSrcPtr(), bltBuf.nSrcWidth, trxPos.nSSAX + x, trxPos.nSSAY + y);
		for(uint32 y = 0; y < height; y++)
			for(uint32 x = 0; x < width; x++)
				GsMemory::WriteRaw(m_pRAM, bltBuf.nDstPsm, bltBuf.GetDstPtr(), bltBuf.nDstWidth, trxPos.nDSAX + x, trxPos.nDSAY + y, pixels[x + y * width]);
	}
	m_textureCache.MarkBytesWritten(dstStart, dstSize);
	UploadTargetsOverlapping(dstStart, dstSize);
}

void CGSH_Hardware::ProcessClutTransfer(uint32, uint32)
{
}

void CGSH_Hardware::SyncCLUT(const TEX0& tex0)
{
	if(tex0.nCLD != 0)
	{
		FlushBatch();
		DownloadTargetsOverlapping(tex0.GetCLUTPtr(), 0x400);
	}
	CGSH_Primitives::SyncCLUT(tex0);
	uint64 hash = XXH3_64bits(m_pCLUT, CLUTSIZE);
	if(hash != m_clutHash)
	{
		m_clutHash = hash;
		MarkStateDirty();
	}
}

//-----------------------------------------------------------------------------
// Drawing
//-----------------------------------------------------------------------------

void CGSH_Hardware::OnStateChanged()
{
	FlushBatch();
	m_state = DecodeState();
	m_stateApplied = false;
}

void CGSH_Hardware::BindTexture()
{
	const auto& s = m_state;
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	m_texScaleS = m_texScaleT = 1.0f;
	m_texOffsetS = m_texOffsetT = 0.0f;
	m_textureAlphaDoubled = false;

	if(!s.textured)
	{
		glBindTexture(GL_TEXTURE_2D, m_glTextures[0].texture);
		return;
	}

	// 1. The texture is (part of) a render target with the same layout:
	//    sample the GPU copy directly.
	for(auto& target : m_targets)
	{
		if((target->fbw != s.tbw) || (PsmClass(target->psm) != PsmClass(s.tpsm)) || !IsColorPsm(s.tpsm)) continue;
		if((s.tbp < target->fbp) || ((s.tbp - target->fbp) % 8192) != 0) continue;
		uint32 page = (s.tbp - target->fbp) / 8192;
		GS_SURFACE surface;
		surface.Init(target->psm, target->fbp, target->fbw);
		uint32 pagesPerRow = std::max<uint32>(surface.pagesPerRow, 1);
		uint32 offsetX = (page % pagesPerRow) << surface.pwShift;
		uint32 offsetY = (page / pagesPerRow) << surface.phShift;
		if(offsetY >= target->height) continue;

		GLuint texture = target->colorTexture;
		if(target.get() == m_currentTarget)
		{
			// Sampling the target being drawn: work on a copy.
			static GLuint feedbackCopy = 0;
			if(!feedbackCopy) glGenTextures(1, &feedbackCopy);
			glBindTexture(GL_TEXTURE_2D, feedbackCopy);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target->width, target->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
			glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, target->width, target->height);
			texture = feedbackCopy;
		}
		glBindTexture(GL_TEXTURE_2D, texture);
		m_textureAlphaDoubled = true;
		m_texScaleS = static_cast<float>(s.tw) / static_cast<float>(target->width);
		m_texScaleT = static_cast<float>(s.th) / static_cast<float>(target->height);
		m_texOffsetS = static_cast<float>(offsetX) / static_cast<float>(target->width);
		m_texOffsetT = static_cast<float>(offsetY) / static_cast<float>(target->height);
		GLint filter = s.bilinear ? GL_LINEAR : GL_NEAREST;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		return;
	}

	// 2. Decode from GS memory, after pulling back anything the GPU rendered there.
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

	uint32 firstPage = 0, pageCount = 0;
	CTextureCache::GetPageRange(key, firstPage, pageCount);
	DownloadTargetsOverlapping(firstPage * 8192, pageCount * 8192);

	auto texture = m_textureCache.Get(key);
	auto& glTexture = m_glTextures[texture->GetUniqueId()];
	if(!glTexture.texture || (glTexture.generation != texture->GetGeneration()))
	{
		texture->DecodeAll();
		if(!glTexture.texture) glGenTextures(1, &glTexture.texture);
		glBindTexture(GL_TEXTURE_2D, glTexture.texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s.tw, s.th, 0, GL_RGBA, GL_UNSIGNED_BYTE, texture->Texels());
		glTexture.generation = texture->GetGeneration();
		m_stats.textureUploads++;
	}
	glTexture.lastUse = ++m_useCounter;
	glBindTexture(GL_TEXTURE_2D, glTexture.texture);
	GLint filter = s.bilinear ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (s.wms == CLAMP_MODE_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (s.wmt == CLAMP_MODE_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE);

	// Evict GL textures that have not been used for a while.
	if(m_glTextures.size() > 96)
	{
		for(auto it = m_glTextures.begin(); it != m_glTextures.end();)
		{
			if((it->first != 0) && (m_useCounter - it->second.lastUse > 64))
			{
				glDeleteTextures(1, &it->second.texture);
				it = m_glTextures.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
}

void CGSH_Hardware::ApplyState()
{
	const auto& s = m_state;
	m_stateApplied = true;
	m_drawNothing = s.zte && (s.ztst == DEPTH_TEST_NEVER);
	if(m_drawNothing) return;

	TARGET* target = FindTarget(s.fbp, s.fbw, s.fpsm, true, static_cast<uint32>(std::max(s.scay1, 0)) + 1);
	if(!target)
	{
		m_drawNothing = true;
		return;
	}
	m_currentTarget = target;

	// Texture first: binding may download targets or copy the current one.
	BindTexture();
	m_currentTarget = target;
	target->gpuDirty = true;

	glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	glViewport(0, 0, target->width, target->height);
	glEnable(GL_SCISSOR_TEST);
	glScissor(s.scax0, s.scay0, std::max(s.scax1 - s.scax0 + 1, 0), std::max(s.scay1 - s.scay0 + 1, 0));

	//Depth
	if(s.zte)
	{
		glEnable(GL_DEPTH_TEST);
		GLenum func = (s.ztst == DEPTH_TEST_ALWAYS) ? GL_ALWAYS : (s.ztst == DEPTH_TEST_GEQUAL) ? GL_GEQUAL : GL_GREATER;
		glDepthFunc(func);
		glDepthMask(s.zmsk ? GL_FALSE : GL_TRUE);
		m_depthFunc = func;
		m_depthWrite = !s.zmsk;
	}
	else
	{
		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		m_depthFunc = GL_ALWAYS;
		m_depthWrite = false;
	}

	//Alpha test
	if(s.ate && (s.atst != ALPHA_TEST_ALWAYS) && (s.afail == ALPHA_TEST_FAIL_KEEP))
	{
		// Fragment alpha is the GS alpha doubled (a * 2 / 255). Thresholds sit
		// half a step between representable values so that comparisons
		// against AREF are exact.
		float lower = std::max(static_cast<float>(s.aref) * 2.0f - 1.0f, 0.0f) / 255.0f;
		float upper = std::min(static_cast<float>(s.aref) * 2.0f + 1.0f, 255.0f) / 255.0f;
		float exact = std::min(static_cast<float>(s.aref) * 2.0f, 255.0f) / 255.0f;
		GLenum func = GL_ALWAYS;
		float ref = 0;
		// Alphas above 0x80 saturate at 1.0: for AREF >= 0x80 assume the
		// common case of alphas <= 0x80.
		bool high = s.aref >= 0x80;
		switch(s.atst & 7)
		{
		case ALPHA_TEST_NEVER: func = GL_NEVER; break;
		case ALPHA_TEST_LESS: func = (s.aref > 0x80) ? GL_ALWAYS : GL_LESS, ref = lower; break;
		case ALPHA_TEST_LEQUAL: func = high ? GL_ALWAYS : GL_LESS, ref = upper; break;
		case ALPHA_TEST_EQUAL: func = (s.aref > 0x80) ? GL_NEVER : GL_EQUAL, ref = exact; break;
		case ALPHA_TEST_GEQUAL: func = (s.aref > 0x80) ? GL_NEVER : GL_GREATER, ref = lower; break;
		case ALPHA_TEST_GREATER: func = high ? GL_NEVER : GL_GREATER, ref = upper; break;
		case ALPHA_TEST_NOTEQUAL: func = (s.aref > 0x80) ? GL_ALWAYS : GL_NOTEQUAL, ref = exact; break;
		}
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(func, ref);
	}
	else
	{
		glDisable(GL_ALPHA_TEST);
		// Fail modes other than KEEP still write color; approximate by not
		// writing depth for the whole primitive.
		if(s.ate && (s.afail == ALPHA_TEST_FAIL_FBONLY || s.afail == ALPHA_TEST_FAIL_RGBONLY))
		{
			glDepthMask(GL_FALSE);
			m_depthWrite = false;
		}
	}

	//Color mask
	bool is24 = GsMemory::IsPsm24(s.fpsm);
	m_colorMask[0] = (s.fbmsk & 0x000000FF) != 0x000000FF;
	m_colorMask[1] = (s.fbmsk & 0x0000FF00) != 0x0000FF00;
	m_colorMask[2] = (s.fbmsk & 0x00FF0000) != 0x00FF0000;
	m_colorMask[3] = !is24 && ((s.fbmsk & 0xFF000000) != 0xFF000000);
	glColorMask(m_colorMask[0], m_colorMask[1], m_colorMask[2], m_colorMask[3]);

	//Blending: (A - B) * C + D
	m_fixAlpha = false;
	m_alphaPass = false;
	if(s.alphaBlend)
	{
		enum
		{
			CS = 0,
			CD = 1,
			ZERO = 2
		};
		uint32 a = s.blendA, b = s.blendB, d = s.blendD;
		bool dstAlpha = (s.blendC == ALPHABLEND_C_AD);
		// FIX is fed through the fragment alpha (vitaGL has no blend
		// color), which the alpha test needs when it is enabled: the test
		// wins and the blend uses the source alpha instead.
		bool alphaTestActive = s.ate && (s.atst != ALPHA_TEST_ALWAYS) && (s.afail == ALPHA_TEST_FAIL_KEEP);
		m_fixAlpha = (s.blendC == ALPHABLEND_C_FIX) && !alphaTestActive;
		// FIX travels in the fragment alpha, but the GS writes the source
		// alpha: a second, alpha-only pass writes it.
		m_alphaPass = m_fixAlpha && m_colorMask[3];
		GLenum c = dstAlpha ? GL_DST_ALPHA : GL_SRC_ALPHA;
		GLenum oneMinusC = dstAlpha ? GL_ONE_MINUS_DST_ALPHA : GL_ONE_MINUS_SRC_ALPHA;
		GLenum src = GL_ONE, dst = GL_ZERO, equation = GL_FUNC_ADD;
		if(a == b)
		{
			src = (d == CS) ? GL_ONE : GL_ZERO;
			dst = (d == CD) ? GL_ONE : GL_ZERO;
		}
		else if(a == CS && b == CD)
		{
			if(d == CD) src = c, dst = oneMinusC;
			else if(d == ZERO) src = c, dst = c, equation = GL_FUNC_SUBTRACT;
			else src = c, dst = oneMinusC; //approximation
		}
		else if(a == CS && b == ZERO)
		{
			src = c;
			dst = (d == CD) ? GL_ONE : GL_ZERO;
		}
		else if(a == CD && b == CS)
		{
			if(d == CS) src = oneMinusC, dst = c;
			else if(d == ZERO) src = c, dst = c, equation = GL_FUNC_REVERSE_SUBTRACT;
			else src = GL_ZERO, dst = GL_ONE; //approximation
		}
		else if(a == CD && b == ZERO)
		{
			if(d == CS) src = GL_ONE, dst = c;
			else if(d == ZERO) src = GL_ZERO, dst = c;
			else src = GL_ZERO, dst = GL_ONE; //approximation
		}
		else if(a == ZERO && b == CS)
		{
			if(d == CD) src = c, dst = GL_ONE, equation = GL_FUNC_REVERSE_SUBTRACT;
			else if(d == CS) src = oneMinusC, dst = GL_ZERO;
			else src = GL_ZERO, dst = GL_ZERO;
		}
		else //a == ZERO && b == CD
		{
			if(d == CS) src = GL_ONE, dst = c, equation = GL_FUNC_SUBTRACT;
			else if(d == CD) src = GL_ZERO, dst = oneMinusC;
			else src = GL_ZERO, dst = GL_ZERO;
		}
		if(!IsBlendExact(a, b, d)) m_stats.approximateBlends++;
		glEnable(GL_BLEND);
		// The GS blends color only; the source alpha is written as is.
		glBlendEquationSeparate(equation, GL_FUNC_ADD);
		glBlendFuncSeparate(src, dst, GL_ONE, GL_ZERO);
	}
	else
	{
		glDisable(GL_BLEND);
	}

	//Texture combiner: GS colors have 0x80 = 1.0.
	bool textured = s.textured;
	bool decal = textured && (s.tfx == TEX0_FUNCTION_DECAL);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
	if(textured && !decal)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PRIMARY_COLOR);
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 2.0f);
	}
	else
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, textured ? GL_TEXTURE : GL_PRIMARY_COLOR);
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
	}
	SetupAlphaCombiner(m_fixAlpha);
}

void CGSH_Hardware::SetupAlphaCombiner(bool fix)
{
	const auto& s = m_state;
	bool textured = s.textured;
	bool decal = textured && (s.tfx == TEX0_FUNCTION_DECAL);
	if(fix)
	{
		float fixValue = std::min(static_cast<float>(s.blendFix) / 128.0f, 1.0f);
		float envColor[4] = {0, 0, 0, fixValue};
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, envColor);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_CONSTANT);
		glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 1.0f);
	}
	else if(textured && s.tcc && !decal)
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_MODULATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_ALPHA, GL_PRIMARY_COLOR);
		glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, m_textureAlphaDoubled ? 2.0f : 4.0f);
	}
	else
	{
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
		bool textureAlpha = textured && s.tcc;
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, textureAlpha ? GL_TEXTURE : GL_PRIMARY_COLOR);
		glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, (textureAlpha && m_textureAlphaDoubled) ? 1.0f : 2.0f);
	}
}

void CGSH_Hardware::AddVertex(const CSoftwareRasterizer::VERTEX& v)
{
	const auto& s = m_state;
	const TARGET& target = *m_currentTarget;
	BATCH_VERTEX out;
	float px = static_cast<float>(v.x) / 16.0f + 0.5f;
	float py = static_cast<float>(v.y) / 16.0f + 0.5f;
	out.x = px / static_cast<float>(target.width) * 2.0f - 1.0f;
	out.y = py / static_cast<float>(target.height) * 2.0f - 1.0f;
	double z = static_cast<double>(std::min(v.z, ZMaxForPsm(s.zpsm))) / static_cast<double>(ZMaxForPsm(s.zpsm));
	out.z = static_cast<float>(z * 2.0 - 1.0);
	out.w = 1.0f;
	out.r = v.r;
	out.g = v.g;
	out.b = v.b;
	out.a = v.a;
	if(s.fst)
	{
		out.s = v.u / static_cast<float>(s.tw) * m_texScaleS + m_texOffsetS;
		out.t = v.v / static_cast<float>(s.th) * m_texScaleT + m_texOffsetT;
	}
	else
	{
		// Q <= 0 cannot be expressed as a clip space w; clamp.
		float q = std::max(v.q, 1.0e-12f);
		out.s = v.s / q * m_texScaleS + m_texOffsetS;
		out.t = v.t / q * m_texScaleT + m_texOffsetT;
		if(s.textured)
		{
			out.w = 1.0f / q;
			out.x *= out.w;
			out.y *= out.w;
			out.z *= out.w;
		}
	}
	m_batch.push_back(out);
}

void CGSH_Hardware::OnDraw(CSoftwareRasterizer::PRIMITIVE_KIND kind, const CSoftwareRasterizer::VERTEX* vertices)
{
	if(!m_stateApplied) ApplyState();
	if(m_drawNothing) return;

	GLenum mode = (kind == CSoftwareRasterizer::PRIMITIVE_POINT) ? GL_POINTS : (kind == CSoftwareRasterizer::PRIMITIVE_LINE) ? GL_LINES : GL_TRIANGLES;
	if(mode != m_batchMode)
	{
		FlushBatch();
		m_batchMode = mode;
	}

	switch(kind)
	{
	case CSoftwareRasterizer::PRIMITIVE_POINT:
		AddVertex(vertices[0]);
		break;
	case CSoftwareRasterizer::PRIMITIVE_LINE:
		AddVertex(vertices[0]);
		AddVertex(vertices[1]);
		break;
	case CSoftwareRasterizer::PRIMITIVE_TRIANGLE:
		if(m_state.gouraud)
		{
			AddVertex(vertices[0]);
			AddVertex(vertices[1]);
			AddVertex(vertices[2]);
		}
		else
		{
			// Flat shading: the last vertex's color for the whole triangle.
			for(int i = 0; i < 3; i++)
			{
				auto v = vertices[i];
				v.r = vertices[2].r;
				v.g = vertices[2].g;
				v.b = vertices[2].b;
				v.a = vertices[2].a;
				AddVertex(v);
			}
		}
		break;
	case CSoftwareRasterizer::PRIMITIVE_SPRITE:
	{
		// Two triangles; color and depth from the second vertex, texture
		// coordinates interpolated between the corners.
		const auto& a = vertices[0];
		const auto& b = vertices[1];
		auto corner = [&](bool right, bool bottom) {
			CSoftwareRasterizer::VERTEX v = b;
			v.x = right ? b.x : a.x;
			v.y = bottom ? b.y : a.y;
			v.u = right ? b.u : a.u;
			v.v = bottom ? b.v : a.v;
			v.s = right ? b.s : a.s;
			v.t = bottom ? b.t : a.t;
			v.q = b.q;
			if(!m_state.fst)
			{
				// Sprites interpolate texel coordinates linearly; use each
				// corner's own projected coordinate with q = 1.
				float qa = (a.q != 0) ? a.q : 1.0f;
				float qb = (b.q != 0) ? b.q : 1.0f;
				v.s = right ? (b.s / qb) : (a.s / qa);
				v.t = bottom ? (b.t / qb) : (a.t / qa);
				v.q = 1.0f;
			}
			return v;
		};
		auto tl = corner(false, false), tr = corner(true, false), bl = corner(false, true), br = corner(true, true);
		AddVertex(tl);
		AddVertex(tr);
		AddVertex(bl);
		AddVertex(tr);
		AddVertex(br);
		AddVertex(bl);
	}
	break;
	}
	if(m_batch.size() >= 3 * 4096) FlushBatch();
}

void CGSH_Hardware::FlushBatch()
{
	if(m_batch.empty()) return;
	const GLsizei stride = sizeof(BATCH_VERTEX);
	glVertexPointer(4, GL_FLOAT, stride, &m_batch[0].x);
	glColorPointer(4, GL_UNSIGNED_BYTE, stride, &m_batch[0].r);
	glTexCoordPointer(2, GL_FLOAT, stride, &m_batch[0].s);
	glDrawArrays(m_batchMode, 0, static_cast<GLsizei>(m_batch.size()));
	if(m_alphaPass)
	{
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
		glDisable(GL_BLEND);
		SetupAlphaCombiner(false);
		// Depth written by the first pass: the same fragments pass EQUAL.
		if(m_depthWrite)
		{
			glDepthFunc(GL_EQUAL);
			glDepthMask(GL_FALSE);
		}
		glDrawArrays(m_batchMode, 0, static_cast<GLsizei>(m_batch.size()));
		glColorMask(m_colorMask[0], m_colorMask[1], m_colorMask[2], m_colorMask[3]);
		glEnable(GL_BLEND);
		SetupAlphaCombiner(true);
		if(m_depthWrite)
		{
			glDepthFunc(m_depthFunc);
			glDepthMask(GL_TRUE);
		}
	}
	m_batch.clear();
	m_stats.drawCalls++;
}

//-----------------------------------------------------------------------------
// Display
//-----------------------------------------------------------------------------

void CGSH_Hardware::FlipImpl(const DISPLAY_INFO& dispInfo)
{
	FlushBatch();
	const auto& layer = dispInfo.layers[0];
	if(layer.enabled && (dispInfo.width != 0) && (dispInfo.height != 0))
	{
		uint32 width = std::min<uint32>(dispInfo.width, 1024);
		uint32 height = std::min<uint32>(dispInfo.height, 1024);
		uint32 bufWidth = layer.bufWidth / 64;
		TARGET* target = FindTarget(layer.bufPtr, bufWidth, layer.psm, false, 0);
		if(target)
		{
			m_display.texture = target->colorTexture;
			m_display.textureWidth = target->width;
			m_display.textureHeight = target->height;
			m_display.x = layer.offsetX;
			m_display.y = layer.offsetY;
			m_display.width = std::min(width, target->width - std::min(layer.offsetX, target->width));
			m_display.height = std::min(height, target->height - std::min(layer.offsetY, target->height));
			if(m_options.readbackFrames && m_options.frameSink)
			{
				m_frameBuffer.resize(m_display.width * m_display.height);
				glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
				glReadPixels(m_display.x, m_display.y, m_display.width, m_display.height, GL_RGBA, GL_UNSIGNED_BYTE, m_frameBuffer.data());
				for(auto& pixel : m_frameBuffer) pixel |= 0xFF000000;
				m_options.frameSink(m_frameBuffer, m_display.width, m_display.height);
			}
		}
		else
		{
			// Not rendered by the GPU (2D games, movies): display GS memory.
			m_frameBuffer.resize(width * height);
			for(uint32 y = 0; y < height; y++)
				for(uint32 x = 0; x < width; x++)
					m_frameBuffer[x + y * width] = CSoftwareRasterizer::ReadColor32(m_pRAM, layer.psm, layer.bufPtr, bufWidth, layer.offsetX + x, layer.offsetY + y) | 0xFF000000;
			glBindTexture(GL_TEXTURE_2D, m_displayUploadTexture);
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, m_frameBuffer.data());
			m_display.texture = m_displayUploadTexture;
			m_display.textureWidth = 1024;
			m_display.textureHeight = 1024;
			m_display.x = 0;
			m_display.y = 0;
			m_display.width = width;
			m_display.height = height;
			if(m_options.readbackFrames && m_options.frameSink) m_options.frameSink(m_frameBuffer, width, height);
		}
	}
	m_stateApplied = false;
	CGSH_Primitives::FlipImpl(dispInfo);
}
