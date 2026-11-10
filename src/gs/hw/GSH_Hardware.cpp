#include "GSH_Hardware.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include "GsMemory.h"
#include "GsSurface.h"
#include "gs/GsTransferRange.h"
#include "xxhash.h"

#ifdef __vita__
#include <psp2/gxm.h>
#endif

namespace
{
#ifdef __vita__
	// vitaGL keeps a renderbuffer's GXM depth surface in the struct its handle points to
	// (vitaGL's renderbuffer: active flag, SceGxmDepthStencilSurface, surface pointer,
	// depth memory, stencil memory) but only ever stores depth, never loads it. With the
	// layout confirmed (the surface's depth pointer and the struct's depth memory agree),
	// the surface is set to load at each scene.
	bool LooksLikePointer(uint32 value)
	{
		return (value >= 0x60000000u) && (value < 0xF0000000u) && ((value & 3) == 0);
	}

	// Writes the first bytes of vitaGL's renderbuffer and framebuffer objects once (layout check).
	void DumpGlObjects(GLuint renderbuffer, GLuint framebuffer)
	{
		static bool dumped = false;
		if(dumped || !CGSH_Hardware::g_glObjectDumpPath) return;
		dumped = true;
		if(FILE* file = std::fopen(CGSH_Hardware::g_glObjectDumpPath, "w"))
		{
			std::fprintf(file, "renderbuffer %08X framebuffer %08X\n", renderbuffer, framebuffer);
			for(GLuint handle : {renderbuffer, framebuffer})
			{
				if(!LooksLikePointer(handle)) continue;
				auto words = reinterpret_cast<const uint32*>(static_cast<uintptr_t>(handle));
				for(int i = 0; i < 48; i++) std::fprintf(file, "%08X%s", words[i], ((i % 8) == 7) ? "\n" : " ");
				std::fprintf(file, "\n");
				// What its pointers into user memory point to
				for(int i = 0; i < 48; i++)
				{
					if((words[i] < 0x81000000u) || (words[i] >= 0xA0000000u) || (words[i] & 3)) continue;
					auto target = reinterpret_cast<const uint32*>(static_cast<uintptr_t>(words[i]));
					std::fprintf(file, " [%d] -> %08X:", i, words[i]);
					for(int j = 0; j < 12; j++) std::fprintf(file, " %08X", target[j]);
					std::fprintf(file, "\n");
				}
			}
			std::fclose(file);
		}
	}

	// vitaGL keeps a pointer to the bound depth surface (SceGxmDepthStencilSurface) in its
	// framebuffer object, pointing into the renderbuffer object; it stores depth but never
	// loads it. Found by that pointer, the surface is set to force load and store (the bits
	// sceGxmDepthStencilSurfaceSetForceLoadMode/SetForceStoreMode set).
	bool EnableDepthForceLoad(GLuint renderbuffer, GLuint framebuffer)
	{
		DumpGlObjects(renderbuffer, framebuffer);
		if(!LooksLikePointer(framebuffer)) return false;
		auto words = reinterpret_cast<const uint32*>(static_cast<uintptr_t>(framebuffer));
		for(int i = 0; i < 48; i++)
		{
			uint32 value = words[i];
			if((value < 0x81000000u) || (value >= 0xA0000000u) || (value & 3)) continue;
			// A depth surface: zlsControl, depth memory (GPU mapped), stencil memory or 0,
			// background depth in [0, 1].
			auto surface = reinterpret_cast<SceGxmDepthStencilSurface*>(static_cast<uintptr_t>(value));
			uint32 depthData = static_cast<uint32>(reinterpret_cast<uintptr_t>(surface->depthData));
			uint32 stencilData = static_cast<uint32>(reinterpret_cast<uintptr_t>(surface->stencilData));
			float background = surface->backgroundDepth;
			if(!LooksLikePointer(depthData) || ((stencilData != 0) && !LooksLikePointer(stencilData))) continue;
			if(!(background >= 0.0f) || !(background <= 1.0f)) continue;
			if(depthData == value) continue;
			surface->zlsControl |= SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED | SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED;
			return true;
		}
		return false;
	}
#endif

	uint64 NowUs()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	// Charges the time of the outermost stage only (stages call each other).
	class CStageTimer
	{
	public:
		explicit CStageTimer(int stage)
		    : m_outer(g_depth++ == 0)
		    , m_stage(stage)
		    , m_start(m_outer ? NowUs() : 0)
		{
		}
		~CStageTimer()
		{
			g_depth--;
			if(m_outer) CGSH_Hardware::GetTimes().stageUs[m_stage] += NowUs() - m_start;
		}

	private:
		static inline int g_depth = 0;
		bool m_outer;
		int m_stage;
		uint64 m_start;
	};

	// Adds the scope's time to one part (parts may nest; each counts its own time in full).
	class CPartTimer
	{
	public:
		explicit CPartTimer(int part)
		    : m_part(part)
		    , m_start(NowUs())
		{
		}
		~CPartTimer()
		{
			CGSH_Hardware::GetTimes().partUs[m_part] += NowUs() - m_start;
		}

	private:
		int m_part;
		uint64 m_start;
	};

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

	uint64 DiagKey(uint32 fbp, uint32 fbw, uint32 psm)
	{
		return (static_cast<uint64>(fbp) << 24) | (static_cast<uint64>(fbw) << 8) | psm;
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

	// Update SameState when fields are added to the drawing state.
	static_assert((sizeof(void*) != 8) || (sizeof(CSoftwareRasterizer::STATE) == 180), "STATE changed");

	bool SameState(const CSoftwareRasterizer::STATE& a, const CSoftwareRasterizer::STATE& b)
	{
		return (a.gouraud == b.gouraud) &&
		       (a.textured == b.textured) &&
		       (a.fog == b.fog) &&
		       (a.alphaBlend == b.alphaBlend) &&
		       (a.fst == b.fst) &&
		       (a.fbp == b.fbp) &&
		       (a.fbw == b.fbw) &&
		       (a.fpsm == b.fpsm) &&
		       (a.fbmsk == b.fbmsk) &&
		       (a.zbp == b.zbp) &&
		       (a.zpsm == b.zpsm) &&
		       (a.zmsk == b.zmsk) &&
		       (a.scax0 == b.scax0) &&
		       (a.scax1 == b.scax1) &&
		       (a.scay0 == b.scay0) &&
		       (a.scay1 == b.scay1) &&
		       (a.ate == b.ate) &&
		       (a.atst == b.atst) &&
		       (a.aref == b.aref) &&
		       (a.afail == b.afail) &&
		       (a.date == b.date) &&
		       (a.datm == b.datm) &&
		       (a.zte == b.zte) &&
		       (a.ztst == b.ztst) &&
		       (a.blendA == b.blendA) &&
		       (a.blendB == b.blendB) &&
		       (a.blendC == b.blendC) &&
		       (a.blendD == b.blendD) &&
		       (a.blendFix == b.blendFix) &&
		       (a.pabe == b.pabe) &&
		       (a.fba == b.fba) &&
		       (a.colClamp == b.colClamp) &&
		       (a.fogColor == b.fogColor) &&
		       (a.tbp == b.tbp) &&
		       (a.tbw == b.tbw) &&
		       (a.tpsm == b.tpsm) &&
		       (a.tw == b.tw) &&
		       (a.th == b.th) &&
		       (a.tcc == b.tcc) &&
		       (a.tfx == b.tfx) &&
		       (a.cpsm == b.cpsm) &&
		       (a.csa == b.csa) &&
		       (a.bilinear == b.bilinear) &&
		       (a.wms == b.wms) &&
		       (a.wmt == b.wmt) &&
		       (a.minu == b.minu) &&
		       (a.maxu == b.maxu) &&
		       (a.minv == b.minv) &&
		       (a.maxv == b.maxv) &&
		       (a.ta0 == b.ta0) &&
		       (a.ta1 == b.ta1) &&
		       (a.aem == b.aem);
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
	uint64 pumpStart = NowUs();
	// The frontend draws on the same context between pumps.
	m_stateApplied = false;
	while(m_mailBox.IsPending() && !m_flipped)
	{
		m_mailBox.ReceiveCall();
	}
	FlushBatch();
	GetTimes().pumpUs += NowUs() - pumpStart;
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

	// Fog: texture unit 1 interpolates towards the fog color by a factor
	// looked up in this ramp (alpha = fog value) from a texture coordinate.
	{
		std::vector<uint32> ramp(256);
		for(uint32 i = 0; i < 256; i++) ramp[i] = 0x00FFFFFF | (i << 24);
		glGenTextures(1, &m_fogRamp);
		glBindTexture(GL_TEXTURE_2D, m_fogRamp);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, ramp.data());
	}

	// Untextured draws still go through the texture combiner (for the alpha
	// scale), sampling a 1x1 white texture.
	CACHED_TEXTURE white;
	glGenTextures(1, &white.texture);
	glBindTexture(GL_TEXTURE_2D, white.texture);
	uint32 whiteTexel = 0xFFFFFFFF;
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &whiteTexel);
	m_glTextures[{0, 0}] = white;
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
	if(m_fogRamp) glDeleteTextures(1, &m_fogRamp);
	m_fogRamp = 0;
	if(m_displayUploadTexture) glDeleteTextures(1, &m_displayUploadTexture);
	for(auto& [size, ring] : m_alphaClutTargets)
	{
		for(auto& entry : ring)
		{
			glDeleteFramebuffers(1, &entry.framebuffer);
			glDeleteTextures(1, &entry.texture);
		}
	}
	m_alphaClutTargets.clear();
	if(m_alphaClutTexture) glDeleteTextures(1, &m_alphaClutTexture);
	m_alphaClutTexture = 0;
	if(m_alphaClutProgram) glDeleteProgram(m_alphaClutProgram);
	m_alphaClutProgram = 0;
	if(m_feedbackTexture) glDeleteTextures(1, &m_feedbackTexture);
	if(m_feedbackFramebuffer) glDeleteFramebuffers(1, &m_feedbackFramebuffer);
	m_feedbackFramebuffer = 0;
	m_feedbackTexture = 0;
	m_feedbackWidth = m_feedbackHeight = 0;
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
	m_frameNumber++;
	m_lastFrameStats = m_stats;
	m_stats = STATS();
	// Frame skip: the frame that just ended may have been skipped; decide
	// for the next one.
	m_lastFrameSkipped = m_skipDrawing;
	m_frameCounter++;
	m_skipDrawing = (m_frameSkip != 0) && ((m_frameCounter % (m_frameSkip + 1)) != 0);
	CGSH_Primitives::MarkNewFrame();
}

void CGSH_Hardware::SetFrameSkip(uint32 frameSkip)
{
	m_frameSkip = frameSkip;
}

//-----------------------------------------------------------------------------
// Render targets
//-----------------------------------------------------------------------------

void CGSH_Hardware::RowsToBytes(const TARGET& target, uint32 rowBegin, uint32 rowEnd, uint32& start, uint32& end) const
{
	GS_SURFACE surface;
	surface.Init(target.psm, target.fbp, target.fbw);
	uint32 rowBytes = std::max<uint32>(surface.pagesPerRow, 1) * 8192;
	start = target.fbp + (rowBegin >> surface.phShift) * rowBytes;
	end = target.fbp + ((rowEnd + surface.phMask) >> surface.phShift) * rowBytes;
}

bool CGSH_Hardware::BytesToRows(const TARGET& target, uint32 start, uint32 size, uint32& rowBegin, uint32& rowEnd) const
{
	GS_SURFACE surface;
	surface.Init(target.psm, target.fbp, target.fbw);
	uint32 pagesPerRow = std::max<uint32>(surface.pagesPerRow, 1);
	uint32 end = start + size;
	if(end <= target.fbp) return false;
	uint32 firstPage = (start > target.fbp) ? (start - target.fbp) / 8192 : 0;
	uint32 lastPage = (end - target.fbp + 8191) / 8192;
	rowBegin = std::min((firstPage / pagesPerRow) << surface.phShift, target.height);
	rowEnd = std::min(((lastPage + pagesPerRow - 1) / pagesPerRow) << surface.phShift, target.height);
	return rowBegin < rowEnd;
}

bool CGSH_Hardware::RowsOverlap(const TARGET& target, uint32 rowBegin, uint32 rowEnd, uint32 start, uint32 size) const
{
	if(rowBegin >= rowEnd) return false;
	uint32 targetStart = 0, targetEnd = 0;
	RowsToBytes(target, rowBegin, rowEnd, targetStart, targetEnd);
	uint32 end = start + size;
	return (start < targetEnd) && (targetStart < end);
}

void CGSH_Hardware::DeleteTarget(TARGET& target)
{
	if(m_pointTarget == &target)
	{
		if(!m_points.empty()) FlushBatch(); //drawn while it was the current target
		m_pointTarget = nullptr;
	}
	if(m_currentTarget == &target) m_currentTarget = nullptr;
	if(m_lastDisplayedTarget == &target) m_lastDisplayedTarget = nullptr;
	if(target.framebuffer) glDeleteFramebuffers(1, &target.framebuffer);
	if(target.colorTexture) glDeleteTextures(1, &target.colorTexture);
	if(target.depthBuffer) glDeleteRenderbuffers(1, &target.depthBuffer);
	target.framebuffer = target.colorTexture = target.depthBuffer = 0;
}

void CGSH_Hardware::RemoveTargetsOverlapping(const TARGET* except, uint32 start, uint32 size)
{
	for(auto it = m_targets.begin(); it != m_targets.end();)
	{
		TARGET& other = **it;
		if((&other != except) && RowsOverlap(other, 0, other.validRows, start, size))
		{
			if(other.gpuDirty) DownloadTarget(other);
			DeleteTarget(other);
			it = m_targets.erase(it);
		}
		else
		{
			++it;
		}
	}
}

CGSH_Hardware::TARGET* CGSH_Hardware::FindTarget(uint32 fbp, uint32 fbw, uint32 psm, bool create, uint32 minHeight)
{
	if(!IsColorPsm(psm) || (fbw == 0)) return nullptr;
	minHeight = std::min<uint32>(minHeight, 1024);
	// Rows up to the next target in use (drawn, sampled or displayed in the
	// last 60 frames: on the Vita a movie field takes ~15 frames to convert
	// while the other buffer is only shown): a scissor reaching past it does
	// not make the buffer that tall
	// (Silent Hill 2's movies draw 192 rows of 512x256 buffers at 0x200000 and
	// 0x280000 with a 448 row scissor).
	uint32 roomRows = ~0u;
	{
		GS_SURFACE surface;
		surface.Init(psm, fbp, fbw);
		uint32 rowBytes = std::max<uint32>(surface.pagesPerRow, 1) * 8192;
		for(const auto& other : m_targets)
		{
			if((other->fbp <= fbp) || (m_frameNumber - other->lastUseFrame > 60)) continue;
			roomRows = std::min(roomRows, ((other->fbp - fbp) / rowBytes) << surface.phShift);
		}
		if((roomRows >= 256) && (roomRows < minHeight)) minHeight = roomRows;
	}

	for(auto& target : m_targets)
	{
		if((target->fbp == fbp) && (target->fbw == fbw) && (PsmClass(target->psm) == PsmClass(psm)))
		{
			if(target->height >= minHeight)
			{
				target->psm = psm;
				target->lastUse = ++m_useCounter;
				target->lastUseFrame = m_frameNumber;
				return target.get();
			}
			break; //too small: recreated below
		}
	}
	if(!create) return nullptr;

	FlushBatch();

	auto target = std::make_unique<TARGET>();
	m_stats.targetsCreated++;
	target->fbp = fbp;
	target->fbw = fbw;
	target->psm = psm;
	target->width = std::min<uint32>(fbw * 64, 1024);
	target->height = RoundUpTargetHeight(std::max<uint32>(minHeight, 448));
	if((target->height > roomRows) && (RoundUpTargetHeight(minHeight) <= roomRows)) target->height = RoundUpTargetHeight(minHeight);
	target->lastUseFrame = m_frameNumber;

	// Targets whose contents live in this memory go back to GS memory first
	// (the new one is filled from GS memory as rows get used). One that only
	// reaches into it with rows nothing was drawn in is cut short instead
	// (Silent Hill 2's movies draw into 512x256 buffers at 0x200000 and
	// 0x280000 by turns: 512 row targets replaced each other every frame,
	// each one read back first).
	uint32 start = 0, end = 0;
	RowsToBytes(*target, 0, target->height, start, end);
	// Targets of the same pixel size lying inside the new one's memory: their pixels are
	// copied page by page on the GPU after it is created, instead of going through GS
	// memory (a read-back, then an upload). Resident Evil 4 draws its blur buffers at
	// 0x310000, 0x348000 and 0x2D8000 1, 2, 4 and 8 pages wide by turns, every frame.
	std::vector<std::unique_ptr<TARGET>> relayout;
	if(g_gpuRelayout)
	{
		for(auto it = m_targets.begin(); it != m_targets.end();)
		{
			if(CanRelayout(**it, *target, start, end))
			{
				relayout.push_back(std::move(*it));
				it = m_targets.erase(it);
			}
			else
			{
				++it;
			}
		}
	}
	for(auto& other : m_targets)
	{
		if((other->fbp >= start) || other->depthUsed) continue;
		uint32 otherStart = 0, otherEnd = 0;
		RowsToBytes(*other, 0, other->height, otherStart, otherEnd);
		if(otherEnd <= start) continue;
		GS_SURFACE surface;
		surface.Init(other->psm, other->fbp, other->fbw);
		uint32 rowBytes = std::max<uint32>(surface.pagesPerRow, 1) * 8192;
		uint32 keepRows = ((start - other->fbp) / rowBytes) << surface.phShift;
		if((keepRows == 0) || (keepRows >= other->height) || (other->gpuDirty && (other->dirtyY1 > keepRows))) continue;
		ShrinkTarget(*other, keepRows);
	}
	RemoveTargetsOverlapping(nullptr, start, end - start);

	AllocateTargetGl(*target);
	target->lastUse = ++m_useCounter;
	m_targets.push_back(std::move(target));
	m_stateApplied = false;
	TARGET* created = m_targets.back().get();
	for(auto& old : relayout)
	{
		RelayoutTarget(*old, *created);
		DeleteTarget(*old);
	}
	return created;
}

void CGSH_Hardware::DrawCopyQuads(GLuint texture, GLuint framebuffer, uint32 width, uint32 height, const std::vector<BATCH_VERTEX>& quads)
{
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glActiveTexture(GL_TEXTURE1);
	glDisable(GL_TEXTURE_2D);
	glClientActiveTexture(GL_TEXTURE1);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTexture(GL_TEXTURE0);
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
	const GLsizei stride = sizeof(BATCH_VERTEX);
	glVertexPointer(4, GL_FLOAT, stride, &quads[0].x);
	glColorPointer(4, GL_UNSIGNED_BYTE, stride, &quads[0].r);
	glTexCoordPointer(2, GL_FLOAT, stride, &quads[0].s);
	glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(quads.size()));
	m_depthSceneValid = false;
	m_stateApplied = false;
}

void CGSH_Hardware::CopyRegionOnGpu(GLuint texture, uint32 width, uint32 height, GLuint framebuffer, uint32 x, uint32 y, uint32 w, uint32 h)
{
	if((w == 0) || (h == 0)) return;
	auto vertex = [&](uint32 px, uint32 py) {
		BATCH_VERTEX v = {};
		v.x = static_cast<float>(px) / static_cast<float>(width) * 2.0f - 1.0f;
		v.y = static_cast<float>(py) / static_cast<float>(height) * 2.0f - 1.0f;
		v.w = 1;
		v.r = v.g = v.b = v.a = 255;
		v.s = static_cast<float>(px) / static_cast<float>(width);
		v.t = static_cast<float>(py) / static_cast<float>(height);
		return v;
	};
	auto tl = vertex(x, y), tr = vertex(x + w, y), bl = vertex(x, y + h), br = vertex(x + w, y + h);
	std::vector<BATCH_VERTEX> quad = {tl, tr, bl, tr, br, bl};
	DrawCopyQuads(texture, framebuffer, width, height, quad);
}

bool CGSH_Hardware::CanRelayout(const TARGET& old, const TARGET& target, uint32 start, uint32 end) const
{
	if(PsmClass(old.psm) != PsmClass(target.psm)) return false;
	if(old.fbp < target.fbp) return false;
	if(old.staleBegin < old.staleEnd) return false;
	if(old.validRows == 0) return false;
	// Everything it holds (its valid rows) must land inside the new target.
	uint32 oldStart = 0, oldEnd = 0;
	RowsToBytes(old, 0, old.validRows, oldStart, oldEnd);
	if((oldStart < start) || (oldEnd > end) || (oldStart >= end)) return false;
	// Whole pages of 64 pixel columns on both sides (the page width of every color format).
	if((old.width != old.fbw * 64) || (target.width != target.fbw * 64)) return false;
	return true;
}

void CGSH_Hardware::RelayoutTarget(TARGET& old, TARGET& target)
{
	CPartTimer timer(GPU_PART_RELAYOUT);
	GS_SURFACE oldSurface, newSurface;
	oldSurface.Init(old.psm, old.fbp, old.fbw);
	newSurface.Init(target.psm, target.fbp, target.fbw);
	uint32 pageW = 1 << oldSurface.pwShift, pageH = 1 << oldSurface.phShift;
	uint32 oldPerRow = std::max<uint32>(oldSurface.pagesPerRow, 1), newPerRow = std::max<uint32>(newSurface.pagesPerRow, 1);
	uint32 firstPage = (old.fbp - target.fbp) / 8192;
	uint32 pageRows = (old.validRows + pageH - 1) / pageH;
	uint32 pageCount = pageRows * oldPerRow;
	// Rows of the new target the copied pages reach: valid after the copy. Rows among them
	// that the old target does not cover come from GS memory first (no other target holds
	// them: those were removed or copied).
	uint32 lastPage = firstPage + pageCount - 1;
	uint32 rowsReached = std::min(((lastPage / newPerRow) + 1) * pageH, target.height);
	if(rowsReached > target.validRows)
	{
		bool coversAll = ((firstPage % newPerRow) == 0) && ((pageCount % newPerRow) == 0) && ((old.validRows % pageH) == 0) &&
		                 ((firstPage / newPerRow) * pageH == target.validRows);
		if(coversAll)
			target.validRows = rowsReached; //every row is replaced by the copy
		else
			EnsureValidRows(target, rowsReached);
	}

	std::vector<BATCH_VERTEX> quads;
	quads.reserve(pageCount * 6);
	int32 dirtyX0 = 0x7FFFFFFF, dirtyY0 = 0x7FFFFFFF, dirtyX1 = 0, dirtyY1 = 0;
	for(uint32 page = 0; page < pageCount; page++)
	{
		uint32 sx = (page % oldPerRow) * pageW, sy = (page / oldPerRow) * pageH;
		uint32 h = std::min(pageH, old.validRows - sy);
		uint32 newPage = firstPage + page;
		uint32 dx = (newPage % newPerRow) * pageW, dy = (newPage / newPerRow) * pageH;
		if((dy >= target.height) || (dx >= target.width) || (sx >= old.width)) continue;
		h = std::min(h, target.height - dy);
		auto vertex = [&](uint32 ox, uint32 oy) {
			BATCH_VERTEX v = {};
			v.x = static_cast<float>(dx + ox) / static_cast<float>(target.width) * 2.0f - 1.0f;
			v.y = static_cast<float>(dy + oy) / static_cast<float>(target.height) * 2.0f - 1.0f;
			v.z = 0;
			v.w = 1;
			v.r = v.g = v.b = v.a = 255;
			v.s = static_cast<float>(sx + ox) / static_cast<float>(old.width);
			v.t = static_cast<float>(sy + oy) / static_cast<float>(old.height);
			return v;
		};
		auto tl = vertex(0, 0), tr = vertex(pageW, 0), bl = vertex(0, h), br = vertex(pageW, h);
		quads.insert(quads.end(), {tl, tr, bl, tr, br, bl});
		bool dirty = old.gpuDirty && (sy < old.dirtyY1) && (sy + h > old.dirtyY0) && (sx < old.dirtyX1) && (sx + pageW > old.dirtyX0);
		if(dirty)
		{
			dirtyX0 = std::min<int32>(dirtyX0, dx);
			dirtyY0 = std::min<int32>(dirtyY0, dy);
			dirtyX1 = std::max<int32>(dirtyX1, dx + pageW);
			dirtyY1 = std::max<int32>(dirtyY1, dy + h);
		}
	}
	if(quads.empty()) return;

	DrawCopyQuads(old.colorTexture, target.framebuffer, target.width, target.height, quads);
	m_stats.relayouts++;
	if(m_diagFile && (m_diagUploads.size() < 60000))
	{
		char text[64];
		std::snprintf(text, sizeof(text), " R%X/%u>%X/%u:%u", old.fbp, old.fbw, target.fbp, target.fbw, pageCount);
		m_diagUploads += text;
	}

	target.drawnRows = std::max(target.drawnRows, std::min(rowsReached, target.height));
	if(dirtyX1 > dirtyX0)
	{
		dirtyY1 = std::min<int32>(dirtyY1, target.validRows);
		if(!target.gpuDirty)
		{
			target.gpuDirty = true;
			target.dirtyX0 = dirtyX0;
			target.dirtyY0 = dirtyY0;
			target.dirtyX1 = dirtyX1;
			target.dirtyY1 = dirtyY1;
		}
		else
		{
			target.dirtyX0 = std::min<uint32>(target.dirtyX0, dirtyX0);
			target.dirtyY0 = std::min<uint32>(target.dirtyY0, dirtyY0);
			target.dirtyX1 = std::max<uint32>(target.dirtyX1, dirtyX1);
			target.dirtyY1 = std::max<uint32>(target.dirtyY1, dirtyY1);
		}
		if(old.alphaDirty) target.alphaDirty = true;
	}
}

void CGSH_Hardware::AllocateTargetGl(TARGET& target)
{
	CPartTimer timer(GPU_PART_TARGET_CREATE);
	glGenTextures(1, &target.colorTexture);
	glBindTexture(GL_TEXTURE_2D, target.colorTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target.width, target.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

	glGenRenderbuffers(1, &target.depthBuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, target.depthBuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, target.width, target.height);
	glGenFramebuffers(1, &target.framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.colorTexture, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, target.depthBuffer);
	glDisable(GL_SCISSOR_TEST);
	glDepthMask(GL_TRUE);
	glClearDepth(0.0);
	glClear(GL_DEPTH_BUFFER_BIT);
}

void CGSH_Hardware::ShrinkTarget(TARGET& target, uint32 rows)
{
	// The first 'rows' rows of the color buffer move to a smaller one on the
	// GPU (callers make sure no depth was drawn and the rest is not dirty).
	FlushBatch();
	GLuint oldFramebuffer = target.framebuffer, oldColor = target.colorTexture, oldDepth = target.depthBuffer;
	target.height = rows;
	AllocateTargetGl(target);
	glBindFramebuffer(GL_FRAMEBUFFER, oldFramebuffer);
	glBindTexture(GL_TEXTURE_2D, target.colorTexture);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, target.width, rows);
	glDeleteFramebuffers(1, &oldFramebuffer);
	glDeleteTextures(1, &oldColor);
	glDeleteRenderbuffers(1, &oldDepth);
	target.validRows = std::min(target.validRows, rows);
	target.drawnRows = std::min(target.drawnRows, rows);
	target.staleEnd = std::min(target.staleEnd, rows);
	if(target.staleBegin >= target.staleEnd) target.staleBegin = target.staleEnd = 0;
	m_stats.targetsShrunk++;
	m_stateApplied = false;
}

CGSH_Hardware::TARGET* CGSH_Hardware::FindTargetContaining(uint32 ptr, uint32 fbw, uint32 psm, uint32 rows, uint32& offsetX, uint32& offsetY)
{
	if(!IsColorPsm(psm) || (fbw == 0)) return nullptr;
	for(auto& target : m_targets)
	{
		if((target->fbw != fbw) || (PsmClass(target->psm) != PsmClass(psm))) continue;
		if((ptr <= target->fbp) || ((ptr - target->fbp) % 8192) != 0) continue;
		GS_SURFACE surface;
		surface.Init(target->psm, target->fbp, target->fbw);
		uint32 pagesPerRow = std::max<uint32>(surface.pagesPerRow, 1);
		uint32 page = (ptr - target->fbp) / 8192;
		uint32 x = (page % pagesPerRow) << surface.pwShift;
		uint32 y = (page / pagesPerRow) << surface.phShift;
		if((x != 0) || (y + rows > target->height)) continue;
		offsetX = x;
		offsetY = y;
		target->lastUse = ++m_useCounter;
		return target.get();
	}
	return nullptr;
}

void CGSH_Hardware::UploadStaleRows(TARGET& target, uint32 rowEnd)
{
	// Stale rows below rowEnd: [staleBegin, min(staleEnd, rowEnd)).
	if((target.staleBegin >= target.staleEnd) || (target.staleBegin >= rowEnd)) return;
	uint32 begin = target.staleBegin, end = std::min(target.staleEnd, rowEnd);
	uint32 start = 0, stop = 0;
	RowsToBytes(target, begin, end, start, stop);
	RemoveTargetsOverlapping(&target, start, stop - start);
	UploadTarget(target, 0, begin, target.width, end - begin);
	target.staleBegin = end;
	if(target.staleBegin >= target.staleEnd) target.staleBegin = target.staleEnd = 0;
}

void CGSH_Hardware::MarkRowsStale(TARGET& target, uint32 rowBegin, uint32 rowEnd)
{
	// GPU pixels not written back yet cannot be replaced from GS memory later.
	bool dirtyRows = target.gpuDirty && (target.dirtyY0 < rowEnd) && (rowBegin < target.dirtyY1);
	bool hasStale = target.staleBegin < target.staleEnd;
	if(hasStale && ((rowBegin > target.staleEnd) || (rowEnd < target.staleBegin)))
	{
		// One range per target: a separate older one is uploaded now.
		UploadStaleRows(target, target.staleEnd);
		hasStale = false;
	}
	if(dirtyRows)
	{
		UploadTarget(target, 0, rowBegin, target.width, rowEnd - rowBegin);
		return;
	}
	target.staleBegin = hasStale ? std::min(target.staleBegin, rowBegin) : rowBegin;
	target.staleEnd = hasStale ? std::max(target.staleEnd, rowEnd) : rowEnd;
	m_stats.deferredUploads++;
}

void CGSH_Hardware::EnsureValidRows(TARGET& target, uint32 rows)
{
	rows = std::min(rows, target.height);
	UploadStaleRows(target, rows);
	if(rows <= target.validRows) return;
	uint32 begin = target.validRows;
	// Other targets holding newer data for these rows go back to memory.
	uint32 start = 0, end = 0;
	RowsToBytes(target, begin, rows, start, end);
	RemoveTargetsOverlapping(&target, start, end - start);
	UploadTarget(target, 0, begin, target.width, rows - begin);
	target.validRows = rows;
}

void CGSH_Hardware::UploadTarget(TARGET& target, uint32 x0, uint32 y0, uint32 width, uint32 height)
{
	CStageTimer stageTimer(GPU_STAGE_TRANSFER);
	x0 = std::min(x0, target.width);
	y0 = std::min(y0, target.height);
	width = std::min(width, target.width - x0);
	height = std::min(height, target.height - y0);
	if((width == 0) || (height == 0)) return;
	FlushBatch();

	if(m_diagFile && (m_diagUploads.size() < 60000))
	{
		char text[64];
		std::snprintf(text, sizeof(text), " U%X:%u,%u+%ux%u", target.fbp, x0, y0, width, height);
		m_diagUploads += text;
	}
	GS_SURFACE surface;
	surface.Init(target.psm, target.fbp, target.fbw);
	bool is16 = IsPsm16(target.psm);
	bool is24 = GsMemory::IsPsm24(target.psm);
	m_transferPixels.resize(width * height);
	uint32* pixels = m_transferPixels.data();
	uint64 swizzleStart = NowUs();
	for(uint32 y = 0; y < height; y++)
	{
		uint32 rowBase = surface.RowBase(y0 + y);
		const uint32* rowTable = surface.RowTable(y0 + y);
		uint32* dst = pixels + y * width;
		if(is16)
		{
			for(uint32 x = 0; x < width; x++)
			{
				uint32 offset = surface.Offset(rowBase, rowTable, x0 + x);
				uint32 color = GsMemory::Color16To32(*reinterpret_cast<const uint16*>(m_pRAM + offset));
				dst[x] = (color & 0x00FFFFFF) | (static_cast<uint32>(GsAlphaToTarget(color >> 24)) << 24);
			}
		}
		else
		{
			for(uint32 x = 0; x < width; x++)
			{
				uint32 offset = surface.Offset(rowBase, rowTable, x0 + x);
				uint32 color = *reinterpret_cast<const uint32*>(m_pRAM + offset);
				uint32 alpha = is24 ? 0x80 : (color >> 24);
				dst[x] = (color & 0x00FFFFFF) | (static_cast<uint32>(GsAlphaToTarget(alpha)) << 24);
			}
		}
	}
	GetTimes().partUs[GPU_PART_UPLOAD_SWIZZLE] += NowUs() - swizzleStart;
	glBindTexture(GL_TEXTURE_2D, target.colorTexture);
	uint64 glStart = NowUs();
	glTexSubImage2D(GL_TEXTURE_2D, 0, x0, y0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	m_depthSceneValid = false;
	GetTimes().uploadGlUs += NowUs() - glStart;
	GetTimes().partUs[GPU_PART_UPLOAD_GL] += NowUs() - glStart;
	GetTimes().uploadPixels += width * height;
	m_stats.targetUploads++;
	m_stats.uploadedPixels += width * height;
	m_stateApplied = false;
}

void CGSH_Hardware::MarkTargetDirty(TARGET& target, int32 x0, int32 y0, int32 x1, int32 y1)
{
	const auto& s = m_state;
	x0 = std::max<int32>({x0, s.scax0, 0});
	y0 = std::max<int32>({y0, s.scay0, 0});
	x1 = std::min<int32>({x1, s.scax1 + 1, static_cast<int32>(target.width)});
	y1 = std::min<int32>({y1, s.scay1 + 1, static_cast<int32>(target.validRows)});
	if((x0 >= x1) || (y0 >= y1)) return;
	target.drawnRows = std::max<uint32>(target.drawnRows, y1);
	if(m_colorMask[3]) target.alphaDirty = true;
	if(!target.gpuDirty)
	{
		target.gpuDirty = true;
		target.dirtyX0 = x0;
		target.dirtyY0 = y0;
		target.dirtyX1 = x1;
		target.dirtyY1 = y1;
		return;
	}
	target.dirtyX0 = std::min<uint32>(target.dirtyX0, x0);
	target.dirtyY0 = std::min<uint32>(target.dirtyY0, y0);
	target.dirtyX1 = std::max<uint32>(target.dirtyX1, x1);
	target.dirtyY1 = std::max<uint32>(target.dirtyY1, y1);
}

void CGSH_Hardware::DownloadTarget(TARGET& target, uint32 rowBegin, uint32 rowEnd)
{
	CStageTimer stageTimer(GPU_STAGE_TRANSFER);
	if(!target.gpuDirty) return;
	uint32 y0 = std::max(target.dirtyY0, rowBegin);
	uint32 y1 = std::min(target.dirtyY1, rowEnd);
	if(y0 >= y1) return;
	// Downloaded rows must leave the dirty rectangle: the RGBA8 copy of GS
	// pixels is lossy (alpha above 0x80), so writing it back again after GS
	// memory changed (a transfer re-uploads rows) would corrupt that data.
	// Rows from the middle cannot leave the rectangle: download all of it.
	if((y0 != target.dirtyY0) && (y1 != target.dirtyY1))
	{
		y0 = target.dirtyY0;
		y1 = target.dirtyY1;
	}
	FlushBatch();
	uint32 x0 = target.dirtyX0;
	uint32 width = target.dirtyX1 - x0, height = y1 - y0;
	if(m_diagFile && (m_diagUploads.size() < 60000))
	{
		char text[64];
		std::snprintf(text, sizeof(text), " D%X:%u,%u+%ux%u(%u-%u)", target.fbp, x0, y0, width, height, rowBegin, rowEnd);
		m_diagUploads += text;
	}
	m_transferPixels.resize(width * height);
	uint32* pixels = m_transferPixels.data();
	glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
	uint64 glStart = NowUs();
	glReadPixels(x0, y0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	m_depthSceneValid = false;
	GetTimes().downloadGlUs += NowUs() - glStart;
	GetTimes().partUs[GPU_PART_DOWNLOAD_PIXELS] += NowUs() - glStart;
	CPartTimer swizzleTimer(GPU_PART_DOWNLOAD_SWIZZLE);
	GetTimes().downloadPixels += width * height;

	GS_SURFACE surface;
	surface.Init(target.psm, target.fbp, target.fbw);
	bool is16 = IsPsm16(target.psm);
	bool is24 = GsMemory::IsPsm24(target.psm);
	for(uint32 y = 0; y < height; y++)
	{
		uint32 rowBase = surface.RowBase(y0 + y);
		const uint32* rowTable = surface.RowTable(y0 + y);
		const uint32* src = pixels + y * width;
		for(uint32 x = 0; x < width; x++)
		{
			uint32 color = src[x];
			uint32 gsColor = (color & 0x00FFFFFF) | (TargetAlphaToGs(color >> 24) << 24);
			uint32 offset = surface.Offset(rowBase, rowTable, x0 + x);
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
	uint32 start = 0, end = 0;
	RowsToBytes(target, y0, y0 + height, start, end);
	m_textureCache.MarkBytesWritten(start, end - start);
	DiscardDirtyRows(target, y0, y1);
	if(!target.gpuDirty) target.alphaDirty = false;
	m_stats.targetDownloads++;
	m_stats.downloadedPixels += width * height;
	m_stateApplied = false;
}

bool CGSH_Hardware::DiscardDirtyRows(TARGET& target, uint32 rowBegin, uint32 rowEnd)
{
	if(!target.gpuDirty || (rowBegin >= rowEnd)) return true;
	if((rowBegin <= target.dirtyY0) && (rowEnd >= target.dirtyY1))
	{
		target.gpuDirty = false;
	}
	else if((rowBegin <= target.dirtyY0) && (rowEnd > target.dirtyY0))
	{
		target.dirtyY0 = rowEnd;
	}
	else if((rowEnd >= target.dirtyY1) && (rowBegin < target.dirtyY1))
	{
		target.dirtyY1 = rowBegin;
	}
	else
	{
		return (rowEnd <= target.dirtyY0) || (rowBegin >= target.dirtyY1);
	}
	return true;
}

void CGSH_Hardware::DownloadTargetsOverlapping(uint32 start, uint32 size, bool overwritten, bool alphaOnly)
{
	// Only the rows stored in [start, start + size) are needed. When the caller
	// is about to overwrite every byte of the range, rows lying entirely inside
	// it are not needed either.
	for(auto& target : m_targets)
	{
		if(!target->gpuDirty) continue;
		if(alphaOnly && !target->alphaDirty) continue;
		uint32 rowBegin = 0, rowEnd = 0;
		if(!BytesToRows(*target, start, size, rowBegin, rowEnd)) continue;
		if(!overwritten)
		{
			DownloadTarget(*target, rowBegin, rowEnd);
			continue;
		}
		GS_SURFACE surface;
		surface.Init(target->psm, target->fbp, target->fbw);
		uint32 rowBytes = std::max<uint32>(surface.pagesPerRow, 1) * 8192;
		uint32 rowsPerPage = surface.phMask + 1;
		// Page rows of the target covered completely: [coveredBegin, coveredEnd).
		uint32 coveredBegin = rowEnd, coveredEnd = rowEnd;
		for(uint32 row = rowBegin; row < rowEnd; row += rowsPerPage)
		{
			uint32 rowStart = target->fbp + (row / rowsPerPage) * rowBytes;
			bool covered = (rowStart >= start) && (rowStart + rowBytes <= start + size);
			if(covered && (coveredBegin == rowEnd)) coveredBegin = row;
			if(!covered && (coveredBegin != rowEnd))
			{
				coveredEnd = row;
				break;
			}
		}
		// GPU pixels of the covered rows are about to be overwritten in memory
		// (and re-uploaded from it): drop them from the dirty rectangle, or
		// download everything when they sit in its middle.
		if(!DiscardDirtyRows(*target, coveredBegin, coveredEnd))
		{
			DownloadTarget(*target);
			continue;
		}
		DownloadTarget(*target, rowBegin, coveredBegin);
		if(coveredEnd < rowEnd) DownloadTarget(*target, coveredEnd, rowEnd);
	}
}

void CGSH_Hardware::UploadTargetsOverlapping(uint32 start, uint32 size)
{
	for(auto& target : m_targets)
	{
		uint32 rowBegin = 0, rowEnd = 0;
		if(!BytesToRows(*target, start, size, rowBegin, rowEnd)) continue;
		rowEnd = std::min(rowEnd, target->validRows);
		if(rowBegin < rowEnd) UploadTarget(*target, 0, rowBegin, target->width, rowEnd - rowBegin);
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
	if(m_diagFile && (m_diagUploads.size() < 60000) && !((trxReg.nRRW == 16) && (trxReg.nRRH == 336)))
	{
		char text[80];
		std::snprintf(text, sizeof(text), " B%X/%u/%X@%u,%u+%ux%u[%X+%X]", bltBuf.GetDstPtr(), static_cast<uint32>(bltBuf.nDstWidth), static_cast<uint32>(bltBuf.nDstPsm),
		              static_cast<uint32>(trxPos.nDSAX), static_cast<uint32>(trxPos.nDSAY), static_cast<uint32>(trxReg.nRRW), static_cast<uint32>(trxReg.nRRH), start, size);
		m_diagUploads += text;
	}
	// Keep pixels the GPU rendered around the transferred rectangle. A
	// rectangle spanning whole pages of its buffer (textures uploaded into
	// framebuffer memory, every frame in some games) overwrites its whole
	// range: GPU pixels there need not go back to memory first.
	auto pageSize = CGsPixelFormats::GetPsmPageSize(bltBuf.nDstPsm);
	uint32 bufferWidth = bltBuf.GetDstWidth();
	bool wholePages = (trxPos.nDSAX == 0) && (bufferWidth != 0) && ((bufferWidth % pageSize.first) == 0) &&
	                  (trxReg.nRRW == bufferWidth) && ((trxPos.nDSAY % pageSize.second) == 0) &&
	                  ((trxReg.nRRH % pageSize.second) == 0);
	// Rows discarded here (not downloaded) leave GS memory older than the GPU
	// copy: then even a transfer that changes no byte must update the target.
	m_transferDiscards = false;
	if(wholePages)
	{
		for(auto& target : m_targets)
		{
			uint32 rowBegin = 0, rowEnd = 0;
			if(target->gpuDirty && BytesToRows(*target, start, size, rowBegin, rowEnd)) m_transferDiscards = true;
		}
	}
	DownloadTargetsOverlapping(start, size, wholePages);
	CGSH_Primitives::BeginTransferWrite();
}

void CGSH_Hardware::ProcessHostToLocalTransfer()
{
	CStageTimer stageTimer(GPU_STAGE_TRANSFER);
	m_stats.transfersIn++;
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	auto [start, size] = GsTransfer::GetDstRange(bltBuf, trxReg, trxPos);
	if(!m_trxCtx.nDirty && !m_transferDiscards)
	{
		// Same bytes as already in memory (menus send the same images every
		// frame): textures and targets stay current.
		m_stats.unchangedTransfers++;
		return;
	}
	m_textureCache.MarkBytesWritten(start, size);
	if(m_diagFile && (m_diagUploads.size() < 60000) && !((trxReg.nRRW == 16) && (trxReg.nRRH == 336)))
	{
		char text[64];
		std::snprintf(text, sizeof(text), " %X/%u/%X@%u,%u+%ux%u", bltBuf.GetDstPtr(), static_cast<uint32>(bltBuf.nDstWidth),
		              static_cast<uint32>(bltBuf.nDstPsm), static_cast<uint32>(trxPos.nDSAX), static_cast<uint32>(trxPos.nDSAY),
		              static_cast<uint32>(trxReg.nRRW), static_cast<uint32>(trxReg.nRRH));
		m_diagUploads += text;
	}

	for(auto& target : m_targets)
	{
		uint32 rowBegin = 0, rowEnd = 0;
		if(!BytesToRows(*target, start, size, rowBegin, rowEnd)) continue;
		rowEnd = std::min(rowEnd, target->validRows);
		if(rowBegin >= rowEnd) continue;
		bool sameLayout = (target->fbp == bltBuf.GetDstPtr()) && (target->fbw == bltBuf.nDstWidth) &&
		                  (PsmClass(target->psm) == PsmClass(bltBuf.nDstPsm)) && IsColorPsm(bltBuf.nDstPsm);
		if(sameLayout)
		{
			uint32 y0 = std::min<uint32>(trxPos.nDSAY, rowEnd);
			uint32 y1 = std::min<uint32>(trxPos.nDSAY + trxReg.nRRH, rowEnd);
			if(y0 < y1) UploadTarget(*target, trxPos.nDSAX, y0, trxReg.nRRW, y1 - y0);
		}
		else if((rowEnd >= target->validRows) && !(target->gpuDirty && (target->dirtyY1 > rowBegin)))
		{
			// Data in another layout (usually textures) written over the
			// target up to the end of its valid rows: those rows are filled
			// from GS memory again when used. Often they never are: menus
			// upload textures into frame buffer memory, then clear the frame
			// with an opaque sprite (see SkipOverwrittenRows).
			target->validRows = rowBegin;
			target->staleEnd = std::min(target->staleEnd, rowBegin);
			if(target->staleBegin >= target->staleEnd) target->staleBegin = target->staleEnd = 0;
			m_stats.deferredUploads++;
		}
		else
		{
			// Data in another layout written over the middle of the target:
			// only the page rows it touched change, and only when used (Silent
			// Hill 2's title screen uploads its textures into the next frame
			// buffer, then covers them with the frame).
			MarkRowsStale(*target, rowBegin, rowEnd);
		}
	}
	m_stateApplied = false;
}

void CGSH_Hardware::ProcessLocalToHostTransfer()
{
	CStageTimer stageTimer(GPU_STAGE_TRANSFER);
	m_stats.transfersOut++;
	FlushBatch();
	auto bltBuf = make_convertible<BITBLTBUF>(m_nReg[GS_REG_BITBLTBUF]);
	auto trxReg = make_convertible<TRXREG>(m_nReg[GS_REG_TRXREG]);
	auto trxPos = make_convertible<TRXPOS>(m_nReg[GS_REG_TRXPOS]);
	auto [start, size] = GsTransfer::GetSrcRange(bltBuf, trxReg, trxPos);
	DownloadTargetsOverlapping(start, size);
}

void CGSH_Hardware::ProcessLocalToLocalTransfer()
{
	CStageTimer stageTimer(GPU_STAGE_TRANSFER);
	m_stats.transfersLocal++;
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
		CPartTimer timer(GPU_PART_LOCAL_COPY);
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
		FlushBatch();
		m_clutHash = hash;
		m_stateApplied = false; //the bound texture depends on the CLUT
		MarkStateDirty();
	}
}

//-----------------------------------------------------------------------------
// Drawing
//-----------------------------------------------------------------------------

void CGSH_Hardware::OnStateChanged()
{
	// Games rewrite registers with the same values all the time: keep
	// batching when the decoded state did not change.
	auto state = DecodeState();
	if(m_stateApplied && SameState(state, m_state))
	{
		m_stats.redundantStateChanges++;
		return;
	}
	FlushBatch();
	m_state = state;
	m_stateApplied = false;
}

void CGSH_Hardware::BindTexture()
{
	CStageTimer stageTimer(GPU_STAGE_TEXTURE);
	const auto& s = m_state;
	glActiveTexture(GL_TEXTURE0);
	glEnable(GL_TEXTURE_2D);
	m_texScaleS = m_texScaleT = 1.0f;
	m_texOffsetS = m_texOffsetT = 0.0f;
	m_textureAlphaDoubled = false;

	if(!s.textured)
	{
		glBindTexture(GL_TEXTURE_2D, m_glTextures[{0, 0}].texture);
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
		// Past everything drawn in this target: the texture only lies inside it
		// because the target's height was rounded up (Resident Evil 4 uploads
		// its movie pictures right after a 448 row back buffer whose target has
		// 512 rows). It is not a render target: decode it from GS memory.
		// (drawnRows can be 2 too high: dirty rectangles of primitives are padded.)
		if((offsetY != 0) && (offsetY + 2 >= target->drawnRows)) continue;
		// TH is a power of 2 and often more than the buffer has (448 rows sampled
		// as 512). Rows past what was drawn are other data, and filling them in
		// would stall the GPU on every frame for rows no draw reads.
		uint32 sampledRows = offsetY + s.th;
		if(target->drawnRows > 2) sampledRows = std::min(sampledRows, std::max(target->drawnRows - 2, offsetY + 1));
		EnsureValidRows(*target, sampledRows);
		target->lastUseFrame = m_frameNumber; //sampled: still in use (see FindTarget)

		GLuint texture = target->colorTexture;
		if(target.get() == m_currentTarget)
		{
			// Sampling the target being drawn: work on a copy of the
			// sampled region.
			FlushBatch();
			if(!m_feedbackTexture || (m_feedbackWidth != target->width) || (m_feedbackHeight != target->height))
			{
				if(!m_feedbackTexture) glGenTextures(1, &m_feedbackTexture);
				glBindTexture(GL_TEXTURE_2D, m_feedbackTexture);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target->width, target->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
				m_feedbackWidth = target->width;
				m_feedbackHeight = target->height;
				if(m_feedbackFramebuffer) glDeleteFramebuffers(1, &m_feedbackFramebuffer);
				glGenFramebuffers(1, &m_feedbackFramebuffer);
				glBindFramebuffer(GL_FRAMEBUFFER, m_feedbackFramebuffer);
				glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_feedbackTexture, 0);
			}
			uint32 copyW = std::min(s.tw, target->width - std::min(offsetX, target->width));
			uint32 copyH = std::min(s.th, target->height - offsetY);
			CPartTimer copyTimer(GPU_PART_FEEDBACK_COPY);
			if(g_feedbackDraw)
			{
				// Drawn on the GPU into the copy (glCopyTexSubImage2D waits for the GPU
				// on the Vita: Resident Evil 4 grades its colours in 16 strips, each one
				// sampling the frame it draws on).
				CopyRegionOnGpu(target->colorTexture, target->width, target->height, m_feedbackFramebuffer, offsetX, offsetY, copyW, copyH);
			}
			else
			{
				glBindTexture(GL_TEXTURE_2D, m_feedbackTexture);
				glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
				glCopyTexSubImage2D(GL_TEXTURE_2D, 0, offsetX, offsetY, offsetX, offsetY, copyW, copyH);
			}
			m_depthSceneValid = false;
			texture = m_feedbackTexture;
		}
		glBindTexture(GL_TEXTURE_2D, texture);
		if(m_diagFile && (m_diagUploads.size() < 60000))
		{
			char text[64];
			if(m_diagUploads.find(" T") == std::string::npos)
			{
				std::snprintf(text, sizeof(text), " T%X/%u/%X:%ux%u@%X", s.tbp, s.tbw, s.tpsm, s.tw, s.th, target->fbp);
				m_diagUploads += text;
			}
		}
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
	if(m_diagFile && (m_diagUploads.size() < 60000))
	{
		char text[64];
		std::snprintf(text, sizeof(text), " M%X/%u/%X:%ux%u", s.tbp, s.tbw, s.tpsm, s.tw, s.th);
		if(m_diagUploads.find(text) == std::string::npos) m_diagUploads += text;
	}
	// A texture of alpha values (PSMT8H, PSMT4HL, PSMT4HH) reads only the top byte of
	// each pixel: GPU pixels drawn with alpha masked do not change it. Games that
	// grade the colours by drawing the frame over itself through a palette indexed by
	// its alpha (Resident Evil 4, in strips) would read the frame back for each strip.
	bool alphaTexture = (s.tpsm == CGSHandler::PSMT8H) || (s.tpsm == CGSHandler::PSMT4HL) || (s.tpsm == CGSHandler::PSMT4HH);
	// Alpha indices of a render target whose alpha the GPU drew: looked up in the CLUT on
	// the GPU (a shader pass) instead of reading the target back.
	if(alphaTexture && g_gpuAlphaClut && BindAlphaClutTexture())
	{
		return;
	}
	DownloadTargetsOverlapping(firstPage * 8192, pageCount * 8192, false, alphaTexture);

	uint64 lookupStart = NowUs();
	auto texture = m_textureCache.Get(key);
	GetTimes().partUs[GPU_PART_TEX_LOOKUP] += NowUs() - lookupStart;
	auto regionU = ResolveWrap(s.wms, s.minu, s.maxu, s.tw);
	auto regionV = ResolveWrap(s.wmt, s.minv, s.maxv, s.th);
	uint64 regionKey = static_cast<uint64>(regionU.origin) | (static_cast<uint64>(regionU.size) << 16) |
	                   (static_cast<uint64>(regionV.origin) << 32) | (static_cast<uint64>(regionV.size) << 48);
	// Games stream their textures through a few slots of GS memory (Resident Evil 4:
	// about 130 small CLUT textures per frame through 0x348000-0x3FE000), so the same
	// texture comes back at the same address with other textures in between, and an
	// entry per address is decoded and uploaded again each time. Textures whose pages
	// hash the same (same format, size, CLUT and TEXA) share one GL texture instead.
	uint64 contentHash = g_contentTextures ? texture->GetContentHash() : 0;
	uint64 contentKey = 0;
	if(contentHash != 0)
	{
		uint64 parts[6] = {contentHash, key.clutHash, static_cast<uint64>(key.tpsm) | (static_cast<uint64>(key.tbw) << 8) | (static_cast<uint64>(key.tbp & 0x1FFF) << 16) | (static_cast<uint64>(key.cpsm) << 32) | (static_cast<uint64>(key.csa) << 40),
		                   static_cast<uint64>(key.tw) | (static_cast<uint64>(key.th) << 16), key.texa, 0};
		contentKey = XXH3_64bits(parts, sizeof(parts)) | (1ULL << 63); //unique ids never reach bit 63
		auto found = m_glTextures.find({contentKey, regionKey});
		if((found != m_glTextures.end()) && found->second.texture)
		{
			m_stats.contentTextureHits++;
			GetTimes().partUs[GPU_PART_CONTENT_HITS] += 1000;
			found->second.lastUse = ++m_useCounter;
			glBindTexture(GL_TEXTURE_2D, found->second.texture);
			GLint filter = s.bilinear ? GL_LINEAR : GL_NEAREST;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, regionU.wrap);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, regionV.wrap);
			m_texScaleS = static_cast<float>(s.tw) / static_cast<float>(regionU.size);
			m_texScaleT = static_cast<float>(s.th) / static_cast<float>(regionV.size);
			m_texOffsetS = -static_cast<float>(regionU.origin) / static_cast<float>(regionU.size);
			m_texOffsetT = -static_cast<float>(regionV.origin) / static_cast<float>(regionV.size);
			return;
		}
	}
	auto& glTexture = m_glTextures[{texture->GetUniqueId(), regionKey}];
	// Movie pictures: rows the decoder's buffers hold go to GL from there, column by
	// column (TextureCache::GetPictureBand); only the rows below are decoded.
	std::vector<CTextureCache::PICTURE_RECT> pictureRects;
	uint32 pictureRows = 0;
	// (the region sampled: from 0, 0 and the texture's whole width; often fewer rows)
	if((!glTexture.texture || (glTexture.generation != texture->GetGeneration())) && g_pictureTextures &&
	   (regionU.size == s.tw) && (regionV.size <= s.th) && (regionU.origin == 0) && (regionV.origin == 0))
	{
		pictureRows = m_textureCache.GetPictureBand(*texture, pictureRects);
	}
	if(pictureRows != 0)
	{
		uint32 rows = regionV.size;
		uint64 decodeStart = NowUs();
		if(pictureRows < rows) texture->DecodeRows(pictureRows, rows);
		GetTimes().texDecodeUs += NowUs() - decodeStart;
		if(!glTexture.texture) glGenTextures(1, &glTexture.texture);
		glBindTexture(GL_TEXTURE_2D, glTexture.texture);
		uint64 glStart = NowUs();
		// New storage, as below
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, s.tw, rows, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		for(const auto& rect : pictureRects)
		{
			//The first rows of a rectangle are the first bytes of its data
			glTexSubImage2D(GL_TEXTURE_2D, 0, rect.x, rect.y, rect.w, std::min(rect.h, rows), GL_RGBA, GL_UNSIGNED_BYTE, rect.data.get());
		}
		if(pictureRows < rows)
		{
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, pictureRows, s.tw, rows - pictureRows, GL_RGBA, GL_UNSIGNED_BYTE, texture->Texels() + pictureRows * s.tw);
		}
		GetTimes().texGlUs += NowUs() - glStart;
		glTexture.generation = texture->GetGeneration();
		m_stats.textureUploads++;
		m_stats.pictureUploads++;
		g_pictureUploadCount++;
	}
	else if(!glTexture.texture || (glTexture.generation != texture->GetGeneration()))
	{
		uint64 decodeStart = NowUs();
		texture->DecodeAll();
		GetTimes().texDecodeUs += NowUs() - decodeStart;
		GetTimes().partUs[GPU_PART_TEX_DECODE] += NowUs() - decodeStart;
		if(!glTexture.texture) glGenTextures(1, &glTexture.texture);
		glBindTexture(GL_TEXTURE_2D, glTexture.texture);
		const void* texels = texture->Texels();
		if((regionU.size != s.tw) || (regionV.size != s.th))
		{
			m_transferPixels.resize(regionU.size * regionV.size);
			for(uint32 y = 0; y < regionV.size; y++)
			{
				std::memcpy(m_transferPixels.data() + y * regionU.size,
				            texture->Texels() + (regionV.origin + y) * s.tw + regionU.origin, regionU.size * 4);
			}
			texels = m_transferPixels.data();
		}
		// Always new storage: draws queued earlier in the frame may still
		// sample the previous contents.
		uint64 glStart = NowUs();
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, regionU.size, regionV.size, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels);
		GetTimes().texGlUs += NowUs() - glStart;
		GetTimes().partUs[GPU_PART_TEX_GL] += NowUs() - glStart;
		glTexture.generation = texture->GetGeneration();
		m_stats.textureUploads++;
		if(contentKey != 0)
		{
			// Kept under its contents: the address entry's GL texture moves there (the
			// address entry gets a new one next time, as draws queued earlier may still
			// sample this one).
			auto& contentTexture = m_glTextures[{contentKey, regionKey}];
			if(contentTexture.texture) glDeleteTextures(1, &contentTexture.texture);
			contentTexture = glTexture;
			glTexture.texture = 0;
			glTexture.generation = ~0U;
			contentTexture.lastUse = ++m_useCounter;
			glBindTexture(GL_TEXTURE_2D, contentTexture.texture);
			GLint filter = s.bilinear ? GL_LINEAR : GL_NEAREST;
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, regionU.wrap);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, regionV.wrap);
			m_texScaleS = static_cast<float>(s.tw) / static_cast<float>(regionU.size);
			m_texScaleT = static_cast<float>(s.th) / static_cast<float>(regionV.size);
			m_texOffsetS = -static_cast<float>(regionU.origin) / static_cast<float>(regionU.size);
			m_texOffsetT = -static_cast<float>(regionV.origin) / static_cast<float>(regionV.size);
			return;
		}
	}
	glTexture.lastUse = ++m_useCounter;
	glBindTexture(GL_TEXTURE_2D, glTexture.texture);
	GLint filter = s.bilinear ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, regionU.wrap);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, regionV.wrap);
	// Texture coordinates are normalized to TW x TH: remap to the region.
	m_texScaleS = static_cast<float>(s.tw) / static_cast<float>(regionU.size);
	m_texScaleT = static_cast<float>(s.th) / static_cast<float>(regionV.size);
	m_texOffsetS = -static_cast<float>(regionU.origin) / static_cast<float>(regionU.size);
	m_texOffsetT = -static_cast<float>(regionV.origin) / static_cast<float>(regionV.size);

	// Evict GL textures that have not been used for a while. A use is one bind:
	// a 3D scene does hundreds per frame.
	if(m_glTextures.size() > 512)
	{
		for(auto it = m_glTextures.begin(); it != m_glTextures.end();)
		{
			if((it->first.first != 0) && ((m_useCounter - it->second.lastUse > 8192) || !it->second.texture))
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

namespace
{
	const char* g_alphaClutVertexShader =
	    "attribute vec4 aPosition;\n"
	    "attribute vec2 aTexCoord;\n"
	    "varying vec2 vTexCoord;\n"
	    "void main()\n"
	    "{\n"
	    "	gl_Position = aPosition;\n"
	    "	vTexCoord = aTexCoord;\n"
	    "}\n";
	// Target alpha is stored doubled (GS 0x80 -> 255): GS alpha = (stored + 1) / 2.
	// uMode: 0 PSMT8H (index = alpha), 1 PSMT4HL (low nibble), 2 PSMT4HH (high nibble).
	const char* g_alphaClutFragmentShader =
	    "uniform sampler2D uSource;\n"
	    "uniform sampler2D uClut;\n"
	    "uniform float uMode;\n"
	    "varying vec2 vTexCoord;\n"
	    "void main()\n"
	    "{\n"
	    "	float stored = floor(texture2D(uSource, vTexCoord).a * 255.0 + 0.5);\n"
	    "	float alpha = floor((stored + 1.0) * 0.5);\n"
	    "	float index = alpha;\n"
	    "	if(uMode > 1.5) index = floor(alpha / 16.0);\n"
	    "	else if(uMode > 0.5) index = alpha - floor(alpha / 16.0) * 16.0;\n"
	    "	gl_FragColor = texture2D(uClut, vec2((index + 0.5) / 256.0, 0.5));\n"
	    "}\n";

	GLuint CompileShader(GLenum type, const char* source)
	{
		GLuint shader = glCreateShader(type);
		glShaderSource(shader, 1, &source, nullptr);
		glCompileShader(shader);
		GLint ok = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
		if(!ok)
		{
			char log[512] = {};
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			std::fprintf(stderr, "GSH_Hardware: shader compile failed: %s\n", log);
			glDeleteShader(shader);
			return 0;
		}
		return shader;
	}
}

bool CGSH_Hardware::BindAlphaClutTexture()
{
	const auto& s = m_state;
	// The target holding the texture, same buffer width, the texture page aligned in it
	// (as for a target sampled directly: see the start of BindTexture).
	TARGET* source = nullptr;
	uint32 offsetX = 0, offsetY = 0;
	for(auto& target : m_targets)
	{
		if((target->fbw != s.tbw) || (PsmClass(target->psm) != 32) || GsMemory::IsPsm24(target->psm)) continue;
		if((s.tbp < target->fbp) || ((s.tbp - target->fbp) % 8192) != 0) continue;
		GS_SURFACE surface;
		surface.Init(target->psm, target->fbp, target->fbw);
		uint32 pagesPerRow = std::max<uint32>(surface.pagesPerRow, 1);
		uint32 page = (s.tbp - target->fbp) / 8192;
		uint32 x = (page % pagesPerRow) << surface.pwShift;
		uint32 y = (page / pagesPerRow) << surface.phShift;
		if((y >= target->height) || (x >= target->width)) continue;
		if((y != 0) && (y + 2 >= target->drawnRows)) continue;
		source = target.get();
		offsetX = x;
		offsetY = y;
		break;
	}
	// Alpha not drawn by the GPU: GS memory is current, the usual decode (cached) is cheaper.
	if(!source || !source->alphaDirty) return false;
	if(!m_alphaClutProgram)
	{
		if(m_alphaClutFailed) return false;
		GLuint vs = CompileShader(GL_VERTEX_SHADER, g_alphaClutVertexShader);
		GLuint fs = CompileShader(GL_FRAGMENT_SHADER, g_alphaClutFragmentShader);
		if(!vs || !fs)
		{
			m_alphaClutFailed = true;
			return false;
		}
		m_alphaClutProgram = glCreateProgram();
		glAttachShader(m_alphaClutProgram, vs);
		glAttachShader(m_alphaClutProgram, fs);
		glBindAttribLocation(m_alphaClutProgram, 0, "aPosition");
		glBindAttribLocation(m_alphaClutProgram, 1, "aTexCoord");
		glLinkProgram(m_alphaClutProgram);
		GLint ok = 0;
		glGetProgramiv(m_alphaClutProgram, GL_LINK_STATUS, &ok);
		if(!ok)
		{
			std::fprintf(stderr, "GSH_Hardware: alpha CLUT program link failed\n");
			glDeleteProgram(m_alphaClutProgram);
			m_alphaClutProgram = 0;
			m_alphaClutFailed = true;
			return false;
		}
		m_alphaClutSourceLocation = glGetUniformLocation(m_alphaClutProgram, "uSource");
		m_alphaClutClutLocation = glGetUniformLocation(m_alphaClutProgram, "uClut");
		m_alphaClutModeLocation = glGetUniformLocation(m_alphaClutProgram, "uMode");
		glGenTextures(1, &m_alphaClutTexture);
		glBindTexture(GL_TEXTURE_2D, m_alphaClutTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	CPartTimer timer(GPU_PART_ALPHA_CLUT);
	FlushBatch();
	uint32 sampledRows = std::min(offsetY + s.th, source->height);
	if(source->drawnRows > 2) sampledRows = std::min(sampledRows, std::max(source->drawnRows - 2, offsetY + 1));
	EnsureValidRows(*source, sampledRows);
	source->lastUseFrame = m_frameNumber;

	// The palette, as the texture cache decodes it (texels hold GS alpha as is).
	uint32 clut[256] = {};
	{
		bool clut32 = !GsMemory::IsPsm16(s.cpsm);
		bool idtex4 = (s.tpsm != CGSHandler::PSMT8H);
		uint32 count = idtex4 ? 16 : 256;
		const uint16* clutRam = m_pCLUT;
		for(uint32 index = 0; index < count; index++)
		{
			if(clut32)
			{
				uint32 offset = idtex4 ? (((s.csa & 0xF) * 16) + index) : (((s.csa * 16) + index) & 0xFF);
				clut[index] = static_cast<uint32>(clutRam[offset]) | (static_cast<uint32>(clutRam[offset + 0x100]) << 16);
			}
			else
			{
				uint32 offset = ((s.csa * 16) + index) & 0x1FF;
				uint32 color16 = clutRam[offset];
				uint32 alpha = 0;
				if(color16 & 0x8000)
					alpha = s.ta1;
				else if(!(s.aem && ((color16 & 0x7FFF) == 0)))
					alpha = s.ta0;
				clut[index] = (GsMemory::Color16To32(color16) & 0x00FFFFFF) | (alpha << 24);
			}
		}
	}
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, m_alphaClutTexture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, clut);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	// The result: a ring of render textures of this size (draws queued earlier may still
	// sample the previous ones).
	uint32 width = std::min<uint32>(s.tw, 1024), height = std::min<uint32>(s.th, 1024);
	auto& ring = m_alphaClutTargets[(static_cast<uint64>(width) << 32) | height];
	if(ring.size() < 4)
	{
		ring.emplace_back();
		auto& entry = ring.back();
		glActiveTexture(GL_TEXTURE0);
		glGenTextures(1, &entry.texture);
		glBindTexture(GL_TEXTURE_2D, entry.texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glGenFramebuffers(1, &entry.framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, entry.framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, entry.texture, 0);
	}
	auto& entry = ring[m_alphaClutNext++ % ring.size()];

	// One quad over the result, sampling the target where the texture lies.
	float s0 = static_cast<float>(offsetX) / source->width, t0 = static_cast<float>(offsetY) / source->height;
	float s1 = static_cast<float>(offsetX + width) / source->width, t1 = static_cast<float>(offsetY + height) / source->height;
	const float vertices[6][6] = {
	    {-1, -1, 0, 1, s0, t0}, {1, -1, 0, 1, s1, t0}, {-1, 1, 0, 1, s0, t1},
	    {1, -1, 0, 1, s1, t0}, {1, 1, 0, 1, s1, t1}, {-1, 1, 0, 1, s0, t1}};
	glBindFramebuffer(GL_FRAMEBUFFER, entry.framebuffer);
	glViewport(0, 0, width, height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
	glDisable(GL_BLEND);
	glDisable(GL_ALPHA_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, source->colorTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glUseProgram(m_alphaClutProgram);
	glUniform1i(m_alphaClutSourceLocation, 0);
	glUniform1i(m_alphaClutClutLocation, 1);
	glUniform1f(m_alphaClutModeLocation, (s.tpsm == CGSHandler::PSMT8H) ? 0.0f : (s.tpsm == CGSHandler::PSMT4HL) ? 1.0f : 2.0f);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), &vertices[0][0]);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(vertices[0]), &vertices[0][4]);
	glDrawArrays(GL_TRIANGLES, 0, 6);
	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glUseProgram(0);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glActiveTexture(GL_TEXTURE0);
	m_depthSceneValid = false;
	m_stateApplied = false;
	m_stats.alphaClutPasses++;
	if(m_diagFile && (m_diagUploads.size() < 60000))
	{
		char text[64];
		std::snprintf(text, sizeof(text), " A%X/%u/%X@%X", s.tbp, s.tbw, s.tpsm, source->fbp);
		m_diagUploads += text;
	}

	// Bound for the draws (ApplyState binds the draw's own framebuffer next).
	glBindTexture(GL_TEXTURE_2D, entry.texture);
	GLint filter = s.bilinear ? GL_LINEAR : GL_NEAREST;
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, (s.wms == CLAMP_MODE_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, (s.wmt == CLAMP_MODE_REPEAT) ? GL_REPEAT : GL_CLAMP_TO_EDGE);
	m_textureAlphaDoubled = false;
	m_texScaleS = static_cast<float>(s.tw) / static_cast<float>(width);
	m_texScaleT = static_cast<float>(s.th) / static_cast<float>(height);
	m_texOffsetS = m_texOffsetT = 0;
	return true;
}

CGSH_Hardware::AXIS_REGION CGSH_Hardware::ResolveWrap(uint32 mode, uint32 minValue, uint32 maxValue, uint32 size)
{
	AXIS_REGION region;
	region.origin = 0;
	region.size = size;
	switch(mode)
	{
	case CLAMP_MODE_REPEAT:
		region.wrap = GL_REPEAT;
		break;
	case CLAMP_MODE_CLAMP:
		region.wrap = GL_CLAMP_TO_EDGE;
		break;
	case CLAMP_MODE_REGION_CLAMP:
	{
		// Clamp to [min, max] (within the texture).
		uint32 first = std::min(minValue, size - 1);
		uint32 last = std::min(std::max(maxValue, first), size - 1);
		region.origin = first;
		region.size = last - first + 1;
		region.wrap = GL_CLAMP_TO_EDGE;
	}
	break;
	case CLAMP_MODE_REGION_REPEAT:
	{
		// u = (u & mask) | fix: a repeating 2^k sub-texture when the mask is
		// a low bit mask and fix does not overlap it.
		uint32 mask = minValue, fix = maxValue;
		bool lowMask = ((mask & (mask + 1)) == 0);
		if(lowMask && ((fix & mask) == 0) && (fix + mask + 1 <= size))
		{
			region.origin = fix;
			region.size = mask + 1;
		}
		else
		{
			m_stats.approximateWrapModes++;
		}
		region.wrap = GL_REPEAT;
	}
	break;
	}
	return region;
}

void CGSH_Hardware::ApplyState()
{
	const auto& s = m_state;
	m_stateApplied = true;
	m_drawNothing = s.zte && (s.ztst == DEPTH_TEST_NEVER);
	if(m_drawNothing) return;

	m_currentTarget = nullptr;
	TARGET* target = FindTarget(s.fbp, s.fbw, s.fpsm, true, static_cast<uint32>(std::max(s.scay1, 0)) + 1);
	if(!target)
	{
		if(m_diagFile) m_diagDropped[DiagKey(s.fbp, s.fbw, s.fpsm)]++;
		m_drawNothing = true;
		return;
	}
	EnsureValidRows(*target, static_cast<uint32>(std::max(s.scay1, 0)) + 1);
	m_currentTarget = target;
	// Depth tested or written: the target's depth buffer matters.
	bool depthActive = s.zte && !((s.ztst == DEPTH_TEST_ALWAYS) && s.zmsk);
	if(depthActive) target->depthUsed = true;
	// Points that just write their color (see AddDirectPoint).
	m_pointsDirect = !s.textured && !s.alphaBlend && !s.fog && (s.fbmsk == 0) && !s.date && !s.fba && !depthActive &&
	                 (!s.ate || (s.atst == ALPHA_TEST_ALWAYS)) && (s.fpsm == PSMCT32);

	// Texture next: binding may download targets or copy the current one.
	BindTexture();

	glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
	glViewport(0, 0, target->width, target->height);
#ifdef __vita__
	// On the Vita a new GPU scene does not load the depth buffer (vitaGL stores it
	// but never loads it): it starts at the surface's background depth, 1.0, which
	// fails every PS2 GEQUAL test. Going to another target, reading one back and
	// copying from one all start a new scene. Depth goes back to the PS2's usual
	// clear value before drawing with it, so what was drawn earlier in the frame
	// no longer hides what comes later: the fewer of these breaks, the better.
	if(target != m_depthSceneTarget) m_depthSceneValid = false;
	m_depthSceneTarget = target;
	if(!m_depthSceneValid && depthActive && !target->depthKept)
	{
		CPartTimer timer(GPU_PART_DEPTH_CLEAR);
		glDisable(GL_SCISSOR_TEST);
		glDepthMask(GL_TRUE);
		glClearDepth(0.0);
		glClear(GL_DEPTH_BUFFER_BIT);
		m_depthSceneValid = true;
		m_stats.depthRestores++;
	}
	// vitaGL sets a framebuffer's depth surface up when it is first drawn with depth: the
	// force load is tried then (a few times), not when the target is created.
	if(depthActive && !target->depthKept && g_depthForceLoad && (target->depthForceTries < 4))
	{
		target->depthForceTries++;
		DumpGlObjects(target->depthBuffer, target->framebuffer);
		target->depthKept = EnableDepthForceLoad(target->depthBuffer, target->framebuffer);
		if(target->depthKept)
			g_depthForceLoadTargets++;
		else
			g_depthForceLoadFailures++;
	}
#endif
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

	// Diagnostics: gs_debug=1 no depth test, 2 no alpha test, 4 no blending.
	if(m_debugFlags & 1)
	{
		glDisable(GL_DEPTH_TEST);
		m_depthFunc = GL_ALWAYS;
	}
	if(m_debugFlags & 2) glDisable(GL_ALPHA_TEST);

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
	SetupFog();
}

void CGSH_Hardware::SetupFog()
{
	const auto& s = m_state;
	glActiveTexture(GL_TEXTURE1);
	if(s.fog)
	{
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, m_fogRamp);
		float fogColor[4] = {static_cast<float>(s.fogColor & 0xFF) / 255.0f, static_cast<float>((s.fogColor >> 8) & 0xFF) / 255.0f,
		                     static_cast<float>((s.fogColor >> 16) & 0xFF) / 255.0f, 1.0f};
		glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, fogColor);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
		// previous * f + fogColor * (1 - f)
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_INTERPOLATE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_PREVIOUS);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_CONSTANT);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC2_RGB, GL_TEXTURE);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND2_RGB, GL_SRC_ALPHA);
		glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
		glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
		glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PREVIOUS);
		glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_ALPHA, GL_SRC_ALPHA);
		glTexEnvf(GL_TEXTURE_ENV, GL_ALPHA_SCALE, 1.0f);
	}
	else
	{
		glDisable(GL_TEXTURE_2D);
	}
	glActiveTexture(GL_TEXTURE0);
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
	out.fogS = (static_cast<float>(v.fog & 0xFF) + 0.5f) / 256.0f;
	out.fogT = 0.5f;
	if(s.fst)
	{
		out.s = v.u / static_cast<float>(s.tw) * m_texScaleS + m_texOffsetS;
		out.t = v.v / static_cast<float>(s.th) * m_texScaleT + m_texOffsetT;
	}
	else
	{
		// Q <= 0 cannot be expressed as a clip space w; clamp.
		float q = std::max(v.q, 1.0e-12f);
		float texS = v.s / q, texT = v.t / q;
		out.s = texS * m_texScaleS + m_texOffsetS;
		out.t = texT * m_texScaleT + m_texOffsetT;
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

void CGSH_Hardware::SkipOverwrittenRows(const CSoftwareRasterizer::VERTEX& a, const CSoftwareRasterizer::VERTEX& b)
{
	// Only pixels the sprite surely replaces: no blending, masks or tests
	// that can keep the old color.
	const auto& s = m_state;
	if(s.alphaBlend || (s.fbmsk != 0) || s.date) return;
	if(s.ate && (s.atst != ALPHA_TEST_ALWAYS)) return;
	if(s.zte && (s.ztst != DEPTH_TEST_ALWAYS)) return;
	uint32 needed = static_cast<uint32>(std::max(s.scay1, 0)) + 1;
	TARGET* target = FindTarget(s.fbp, s.fbw, s.fpsm, false, needed);
	if(!target) return;
	needed = std::min(needed, target->height);
	// Pixels the sprite draws: [ceil(x0), ceil(x1)) (12.4 fixed point corners,
	// as CSoftwareRasterizer; a frame drawn at -0.25..511.75 covers 0..511).
	// They and the scissor span the target's width.
	auto ceilPixel = [](int32 value) { return (value + 15) >> 4; };
	int32 x0 = ceilPixel(std::min(a.x, b.x)), x1 = ceilPixel(std::max(a.x, b.x));
	int32 y0 = ceilPixel(std::min(a.y, b.y)), y1 = ceilPixel(std::max(a.y, b.y));
	int32 width = static_cast<int32>(target->width);
	if((x0 > 0) || (x1 < width) || (s.scax0 > 0) || (s.scax1 < width - 1)) return;
	// Whole rows [begin, end) covered: their GS memory is not needed on the
	// GPU, no upload. Other targets holding these rows still go back to
	// memory, as for an upload.
	auto covers = [&](uint32 begin, uint32 end) {
		return (y0 <= static_cast<int32>(begin)) && (y1 >= static_cast<int32>(end)) &&
		       (s.scay0 <= static_cast<int32>(begin)) && (s.scay1 + 1 >= static_cast<int32>(end));
	};
	auto skip = [&](uint32 begin, uint32 end) {
		uint32 start = 0, stop = 0;
		RowsToBytes(*target, begin, end, start, stop);
		RemoveTargetsOverlapping(target, start, stop - start);
		m_stats.skippedUploadRows += end - begin;
	};
	if((target->staleBegin < target->staleEnd) && covers(target->staleBegin, target->staleEnd))
	{
		skip(target->staleBegin, target->staleEnd);
		target->staleBegin = target->staleEnd = 0;
	}
	if((needed > target->validRows) && covers(target->validRows, needed))
	{
		skip(target->validRows, needed);
		target->validRows = needed;
	}
}

void CGSH_Hardware::OnDraw(CSoftwareRasterizer::PRIMITIVE_KIND kind, const CSoftwareRasterizer::VERTEX* vertices)
{
	if(m_diagFile) m_diagKinds[kind & 3]++;
	if(!m_stateApplied)
	{
		// The first primitive after a state change: an opaque sprite over
		// rows that were never uploaded makes their upload unnecessary.
		if(kind == CSoftwareRasterizer::PRIMITIVE_SPRITE) SkipOverwrittenRows(vertices[0], vertices[1]);
		ApplyState();
	}
	if(m_diagFile && (kind == CSoftwareRasterizer::PRIMITIVE_TRIANGLE) && ((m_diagKinds[2] % 700) == 1) && (m_diagPrims.size() < 6000))
	{
		const auto& st = m_state;
		char text[400];
		std::snprintf(text, sizeof(text),
		              " {tri#%u fb%X/%u/%X msk%X z%X/%X zm%d zte%d zt%u ate%d at%u ref%u af%u date%d abe%d pabe%d %u%u%u%u fix%u fst%d tex%d %X/%u/%X %ux%u tfx%u tcc%d nothing%d sc%d,%d-%d,%d v0 %.1f,%.1f z%X rgba%02X%02X%02X%02X q%.3g v1 %.1f,%.1f z%X v2 %.1f,%.1f z%X}",
		              m_diagKinds[2], st.fbp, st.fbw, st.fpsm, st.fbmsk, st.zbp, st.zpsm, st.zmsk ? 1 : 0, st.zte ? 1 : 0, st.ztst, st.ate ? 1 : 0, st.atst, st.aref, st.afail, st.date ? 1 : 0, st.alphaBlend ? 1 : 0, st.pabe ? 1 : 0,
		              st.blendA, st.blendB, st.blendC, st.blendD, st.blendFix, st.fst ? 1 : 0, st.textured ? 1 : 0, st.tbp, st.tbw, st.tpsm, st.tw, st.th, st.tfx, st.tcc ? 1 : 0, m_drawNothing ? 1 : 0, st.scax0, st.scay0, st.scax1, st.scay1,
		              vertices[0].x / 16.0, vertices[0].y / 16.0, vertices[0].z, vertices[0].r, vertices[0].g, vertices[0].b, vertices[0].a, vertices[0].q, vertices[1].x / 16.0, vertices[1].y / 16.0, vertices[1].z, vertices[2].x / 16.0, vertices[2].y / 16.0, vertices[2].z);
		m_diagPrims += text;
	}
	if(m_drawNothing) return;

	GLenum mode = (kind == CSoftwareRasterizer::PRIMITIVE_POINT) ? GL_POINTS : (kind == CSoftwareRasterizer::PRIMITIVE_LINE) ? GL_LINES : GL_TRIANGLES;
	if(mode != m_batchMode)
	{
		FlushBatch();
		m_batchMode = mode;
	}
	if((kind == CSoftwareRasterizer::PRIMITIVE_POINT) && m_pointsDirect && m_currentTarget)
	{
		AddDirectPoint(vertices[0]);
		return;
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
		if(m_diagFile && m_currentTarget && (m_diagSpriteCount[m_currentTarget->fbp]++ < 6))
		{
			char text[160];
			const auto& s = m_state;
			std::snprintf(text, sizeof(text), " [%X: %.4g,%.4g-%.4g,%.4g z%X zte%d zt%u zm%d zp%X/%X", m_currentTarget->fbp, a.x / 16.0, a.y / 16.0, b.x / 16.0, b.y / 16.0, b.z, s.zte ? 1 : 0, s.ztst, s.zmsk ? 1 : 0, s.zbp, s.zpsm);
			m_diagPrims += text;
			if(s.alphaBlend)
			{
				// Blend (A-B)*C+D: 0 Cs/As, 1 Cd/Ad, 2 zero/FIX; vertex alpha.
				std::snprintf(text, sizeof(text), " abe%u%u%u%u/%02X a%02X", s.blendA, s.blendB, s.blendC, s.blendD, s.blendFix, b.a);
				m_diagPrims += text;
			}
			if(s.textured)
			{
				if(s.fst) std::snprintf(text, sizeof(text), " t%X/%u/%X %ux%u uv%.1f,%.1f-%.1f,%.1f b%d]", s.tbp, s.tbw, s.tpsm, s.tw, s.th, a.u, a.v, b.u, b.v, s.bilinear ? 1 : 0);
				else std::snprintf(text, sizeof(text), " t%X/%u/%X %ux%u st%.4f,%.4f-%.4f,%.4f b%d]", s.tbp, s.tbw, s.tpsm, s.tw, s.th, a.s / a.q, a.t / a.q, b.s / b.q, b.t / b.q, s.bilinear ? 1 : 0);
			}
			else
			{
				std::snprintf(text, sizeof(text), " rgb%02X%02X%02X]", b.r, b.g, b.b);
			}
			m_diagPrims += text;
		}
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
	if(m_currentTarget)
	{
		int32 minX = vertices[0].x, maxX = vertices[0].x, minY = vertices[0].y, maxY = vertices[0].y;
		uint32 count = (kind == CSoftwareRasterizer::PRIMITIVE_POINT) ? 1 : (kind == CSoftwareRasterizer::PRIMITIVE_TRIANGLE) ? 3 : 2;
		for(uint32 i = 1; i < count; i++)
		{
			minX = std::min(minX, vertices[i].x);
			maxX = std::max(maxX, vertices[i].x);
			minY = std::min(minY, vertices[i].y);
			maxY = std::max(maxY, vertices[i].y);
		}
		MarkTargetDirty(*m_currentTarget, minX >> 4, minY >> 4, (maxX >> 4) + 2, (maxY >> 4) + 2);
	}
	if(m_batch.size() >= 3 * 4096) FlushBatch();
}

void CGSH_Hardware::AddDirectPoint(const CSoftwareRasterizer::VERTEX& v)
{
	const auto& s = m_state;
	TARGET& target = *m_currentTarget;
	// The pixel a GPU point covers (and CSoftwareRasterizer's rule).
	int32 x = (v.x + 8) >> 4, y = (v.y + 8) >> 4;
	if((x < std::max(s.scax0, 0)) || (x > s.scax1) || (y < std::max(s.scay0, 0)) || (y > s.scay1)) return;
	if((x >= static_cast<int32>(target.width)) || (y >= static_cast<int32>(target.height))) return;
	if(m_pointTarget != &target)
	{
		m_pointTarget = &target;
		m_pointPixels.resize(target.width * target.height);
		m_pointWritten.assign(target.width * target.height, 0);
	}
	if(m_points.empty())
	{
		m_pointX0 = m_pointX1 = x;
		m_pointY0 = m_pointY1 = y;
		m_pointUnique = 0;
	}
	else
	{
		m_pointX0 = std::min(m_pointX0, x);
		m_pointX1 = std::max(m_pointX1, x);
		m_pointY0 = std::min(m_pointY0, y);
		m_pointY1 = std::max(m_pointY1, y);
	}
	uint32 index = x + y * target.width;
	m_pointPixels[index] = v.r | (v.g << 8) | (v.b << 16) | (static_cast<uint32>(GsAlphaToTarget(v.a)) << 24);
	if(!m_pointWritten[index])
	{
		m_pointWritten[index] = 1;
		m_pointUnique++;
	}
	m_points.push_back({v.x, v.y, static_cast<uint32>(v.r) | (v.g << 8) | (v.b << 16) | (static_cast<uint32>(v.a) << 24)});
}

void CGSH_Hardware::FlushPoints()
{
	if(m_points.empty()) return;
	TARGET& target = *m_pointTarget;
	uint32 width = m_pointX1 - m_pointX0 + 1, height = m_pointY1 - m_pointY0 + 1;
	if(m_pointUnique == width * height)
	{
		// Every pixel of the rectangle written: upload it.
		m_transferPixels.resize(width * height);
		for(uint32 row = 0; row < height; row++)
		{
			uint32 index = m_pointX0 + (m_pointY0 + row) * target.width;
			std::memcpy(&m_transferPixels[row * width], &m_pointPixels[index], width * 4);
		}
		glBindTexture(GL_TEXTURE_2D, target.colorTexture);
		glTexSubImage2D(GL_TEXTURE_2D, 0, m_pointX0, m_pointY0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, m_transferPixels.data());
		MarkTargetDirty(target, m_pointX0, m_pointY0, m_pointX1 + 1, m_pointY1 + 1);
		m_stats.pointRects++;
		m_stats.drawCalls++;
		m_stateApplied = false; //texture binding changed
	}
	else
	{
		// Gaps: draw them as points after all (the batch is in point mode).
		TARGET* current = m_currentTarget;
		m_currentTarget = &target;
		// Only position and color matter for these points (no texture, fog or
		// depth). Drawn in batches like other primitives (the list is moved out:
		// FlushBatch comes back here).
		std::vector<PENDING_POINT> points;
		points.swap(m_points);
		CSoftwareRasterizer::VERTEX vertex = {};
		for(const auto& point : points)
		{
			if(m_batch.size() >= 3 * 4096) FlushBatch();
			vertex.x = point.x;
			vertex.y = point.y;
			vertex.r = point.rgba & 0xFF;
			vertex.g = (point.rgba >> 8) & 0xFF;
			vertex.b = (point.rgba >> 16) & 0xFF;
			vertex.a = point.rgba >> 24;
			AddVertex(vertex);
		}
		MarkTargetDirty(target, m_pointX0, m_pointY0, m_pointX1 + 1, m_pointY1 + 1);
		m_currentTarget = current;
		m_stats.pointsDrawn += static_cast<uint32>(points.size());
		points.clear();
		m_points.swap(points); //keeps the allocation
	}
	for(uint32 row = 0; row < height; row++)
		std::memset(&m_pointWritten[m_pointX0 + (m_pointY0 + row) * target.width], 0, width);
	m_points.clear();
}

void CGSH_Hardware::FlushBatch()
{
	CStageTimer stageTimer(GPU_STAGE_GL);
	FlushPoints();
	if(m_batch.empty()) return;
	const GLsizei stride = sizeof(BATCH_VERTEX);
	glVertexPointer(4, GL_FLOAT, stride, &m_batch[0].x);
	glColorPointer(4, GL_UNSIGNED_BYTE, stride, &m_batch[0].r);
	glTexCoordPointer(2, GL_FLOAT, stride, &m_batch[0].s);
	if(m_state.fog)
	{
		glClientActiveTexture(GL_TEXTURE1);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, stride, &m_batch[0].fogS);
		glClientActiveTexture(GL_TEXTURE0);
	}
	{
		CPartTimer timer(GPU_PART_DRAW);
		glDrawArrays(m_batchMode, 0, static_cast<GLsizei>(m_batch.size()));
	}
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
	if(m_currentTarget) m_currentTarget->drawBatches++;
	if(m_diagFile && m_currentTarget) m_diagDraws[DiagKey(m_currentTarget->fbp, m_currentTarget->fbw, m_currentTarget->psm)]++;
}

//-----------------------------------------------------------------------------
// Display
//-----------------------------------------------------------------------------

void CGSH_Hardware::SetDiagnosticsFile(const char* path, uint32 maxLines)
{
	if(m_diagFile) std::fclose(m_diagFile);
	m_diagFile = std::fopen(path, "w");
	m_diagLinesLeft = maxLines;
	m_diagDraws.clear();
	m_diagDropped.clear();
}

void CGSH_Hardware::WriteFlipDiagnostics(const DISPLAY_INFO& dispInfo)
{
	if(!m_diagFile) return;
	if(m_diagLinesLeft == 0)
	{
		std::fclose(m_diagFile);
		m_diagFile = nullptr;
		return;
	}
	m_diagLinesLeft--;
	const auto& layer = dispInfo.layers[0];
	std::fprintf(m_diagFile, "flip %u: %s buf %X fbw %u psm %X off %u,%u size %ux%u skip %d | draws", m_diagFlips++,
	             layer.enabled ? "on" : "off", layer.bufPtr, layer.bufWidth / 64, layer.psm, layer.offsetX, layer.offsetY,
	             dispInfo.width, dispInfo.height, m_lastFrameSkipped ? 1 : 0);
	for(const auto& [key, count] : m_diagDraws)
		std::fprintf(m_diagFile, " %X/%u/%X:%u", static_cast<uint32>(key >> 24), static_cast<uint32>((key >> 8) & 0xFFFF), static_cast<uint32>(key & 0xFF), count);
	if(!m_diagDropped.empty())
	{
		std::fprintf(m_diagFile, " | dropped");
		for(const auto& [key, count] : m_diagDropped)
			std::fprintf(m_diagFile, " %X/%u/%X:%u", static_cast<uint32>(key >> 24), static_cast<uint32>((key >> 8) & 0xFFFF), static_cast<uint32>(key & 0xFF), count);
	}
	if(!m_diagUploads.empty()) std::fprintf(m_diagFile, " | uploads%s", m_diagUploads.c_str());
	m_diagUploads.clear();
	std::fprintf(m_diagFile, " | prims p%u l%u t%u s%u", m_diagKinds[0], m_diagKinds[1], m_diagKinds[2], m_diagKinds[3]);
	std::memset(m_diagKinds, 0, sizeof(m_diagKinds));
	if(!m_diagPrims.empty()) std::fprintf(m_diagFile, " | sprites%s", m_diagPrims.c_str());
	m_diagPrims.clear();
	m_diagSpriteCount.clear();
	std::fprintf(m_diagFile, " | targets");
	for(const auto& target : m_targets)
	{
		std::fprintf(m_diagFile, " %X/%u/%X %ux%u v%u%s", target->fbp, target->fbw, target->psm, target->width, target->height,
		             target->validRows, target->gpuDirty ? "d" : "");
		if(target->staleBegin < target->staleEnd) std::fprintf(m_diagFile, " s%u-%u", target->staleBegin, target->staleEnd);
	}
	std::fprintf(m_diagFile, "\n");
	std::fflush(m_diagFile);
	m_diagDraws.clear();
	m_diagDropped.clear();
}

void CGSH_Hardware::DumpTargets()
{
	char path[128];
	for(const auto& target : m_targets)
	{
		std::vector<uint32> pixels(target->width * target->height);
		glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
		glReadPixels(0, 0, target->width, target->height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		std::snprintf(path, sizeof(path), "%s/t_%X_%u_%X.ppm", m_targetDumpDirectory.c_str(), target->fbp, target->fbw, target->psm);
		m_frameDumpPath = path;
		WriteFrameDump(pixels.data(), target->width, target->height);
	}
	m_targetDumpDirectory.clear();
	m_stateApplied = false;
}

void CGSH_Hardware::WriteFrameDump(const uint32* pixels, uint32 width, uint32 height)
{
	std::FILE* file = std::fopen(m_frameDumpPath.c_str(), "wb");
	m_frameDumpPath.clear();
	if(!file) return;
	std::fprintf(file, "P6\n%u %u\n255\n", width, height);
	std::vector<uint8> row(width * 3);
	for(uint32 y = 0; y < height; y++)
	{
		for(uint32 x = 0; x < width; x++)
		{
			uint32 pixel = pixels[y * width + x];
			row[x * 3 + 0] = static_cast<uint8>(pixel);
			row[x * 3 + 1] = static_cast<uint8>(pixel >> 8);
			row[x * 3 + 2] = static_cast<uint8>(pixel >> 16);
		}
		std::fwrite(row.data(), 1, row.size(), file);
	}
	std::fclose(file);
}

void CGSH_Hardware::FlipImpl(const DISPLAY_INFO& dispInfo)
{
	CStageTimer stageTimer(GPU_STAGE_DISPLAY);
	FlushBatch();
	m_depthSceneValid = false;
	WriteFlipDiagnostics(dispInfo);
	if(!m_targetDumpDirectory.empty()) DumpTargets();
	const auto& layer = dispInfo.layers[0];
	// A skipped frame was not drawn: keep showing the last drawn one (double
	// buffered games would otherwise flash an older frame).
	if(m_lastFrameSkipped)
	{
		m_displayStats.skipped++;
		CGSH_Primitives::FlipImpl(dispInfo);
		return;
	}
	if(layer.enabled && (dispInfo.width != 0) && (dispInfo.height != 0))
	{
		uint32 width = std::min<uint32>(dispInfo.width, 1024);
		uint32 height = std::min<uint32>(dispInfo.height, 1024);
		uint32 bufWidth = layer.bufWidth / 64;
		m_displayStats.bufPtr = layer.bufPtr;
		m_displayStats.bufWidth = bufWidth;
		m_displayStats.psm = layer.psm;
		m_displayStats.offsetY = layer.offsetY;
		uint32 baseX = 0, baseY = 0;
		TARGET* target = FindTarget(layer.bufPtr, bufWidth, layer.psm, false, 0);
		// Double buffering inside one larger drawing area: the displayed
		// buffer starts some page rows into a render target.
		if(!target) target = FindTargetContaining(layer.bufPtr, bufWidth, layer.psm, layer.offsetY + height, baseX, baseY);
		if(target)
		{
			m_displayStats.fromTarget++;
			if((target != m_lastDisplayedTarget) || (target->drawBatches != m_lastDisplayedBatches)) g_gameFrames++;
			m_lastDisplayedTarget = target;
			m_lastDisplayedBatches = target->drawBatches;
			uint32 x = baseX + layer.offsetX, y = baseY + layer.offsetY;
			EnsureValidRows(*target, y + height);
			target->lastUseFrame = m_frameNumber; //displayed: still in use
			m_display.texture = target->colorTexture;
			m_display.textureWidth = target->width;
			m_display.textureHeight = target->height;
			m_display.x = x;
			m_display.y = y;
			m_display.width = std::min(width, target->width - std::min(x, target->width));
			m_display.height = std::min(height, target->height - std::min(y, target->height));
			if(!m_frameDumpPath.empty())
			{
				std::vector<uint32> dump(m_display.width * m_display.height);
				glBindFramebuffer(GL_FRAMEBUFFER, target->framebuffer);
				glReadPixels(m_display.x, m_display.y, m_display.width, m_display.height, GL_RGBA, GL_UNSIGNED_BYTE, dump.data());
				WriteFrameDump(dump.data(), m_display.width, m_display.height);
			}
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
			// Not rendered by the GPU (2D games, movies): display GS memory,
			// after fetching anything the GPU drew there.
			m_displayStats.fromMemory++;
			GS_SURFACE surface;
			bool tabled = surface.Init(layer.psm, layer.bufPtr, bufWidth) && !surface.nibbles;
			uint32 pageRows = ((layer.offsetY + height) + surface.phMask) >> surface.phShift;
			DownloadTargetsOverlapping(layer.bufPtr, std::max<uint32>(surface.pagesPerRow, 1) * pageRows * 8192);
			m_frameBuffer.resize(width * height);
			bool is16 = (layer.psm == CGSHandler::PSMCT16) || (layer.psm == CGSHandler::PSMCT16S);
			bool is32 = (layer.psm == CGSHandler::PSMCT32) || (layer.psm == CGSHandler::PSMCT24);
			if(tabled && (is16 || is32))
			{
				// Movies and 2D games come through here every frame: walk each
				// row with the page table instead of a generic call per pixel.
				for(uint32 y = 0; y < height; y++)
				{
					uint32 gsY = (layer.offsetY + y) & 2047;
					uint32 rowBase = surface.RowBase(gsY);
					const uint32* rowTable = surface.RowTable(gsY);
					uint32* dst = m_frameBuffer.data() + y * width;
					for(uint32 x = 0; x < width; x++)
					{
						uint32 offset = surface.Offset(rowBase, rowTable, (layer.offsetX + x) & 2047);
						dst[x] = is16 ? GsMemory::Color16To32(*reinterpret_cast<const uint16*>(m_pRAM + offset)) | 0xFF000000
						              : *reinterpret_cast<const uint32*>(m_pRAM + offset) | 0xFF000000;
					}
				}
			}
			else
			{
				for(uint32 y = 0; y < height; y++)
					for(uint32 x = 0; x < width; x++)
						m_frameBuffer[x + y * width] = CSoftwareRasterizer::ReadColor32(m_pRAM, layer.psm, layer.bufPtr, bufWidth, layer.offsetX + x, layer.offsetY + y) | 0xFF000000;
			}
			if(!m_frameDumpPath.empty()) WriteFrameDump(m_frameBuffer.data(), width, height);
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
	else
	{
		m_displayStats.disabled++;
	}
	m_stateApplied = false;
	CGSH_Primitives::FlipImpl(dispInfo);
}

void CGSH_Hardware::DiscardGpuCopies()
{
	FlushBatch();
	for(auto& target : m_targets) DeleteTarget(*target);
	m_targets.clear();
	m_currentTarget = nullptr;
	for(auto it = m_glTextures.begin(); it != m_glTextures.end();)
	{
		// Keep the white texture used for untextured draws.
		if(it->first.first == 0)
		{
			++it;
			continue;
		}
		glDeleteTextures(1, &it->second.texture);
		it = m_glTextures.erase(it);
	}
	m_textureCache.MarkAllWritten();
	m_display = DISPLAY_TEXTURE();
	m_stateApplied = false;
}
