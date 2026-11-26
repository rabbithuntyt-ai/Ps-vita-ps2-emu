#pragma once

// GPU accelerated GS renderer.
//
// PS2 primitives are translated into OpenGL draws (fixed-function subset
// supported by vitaGL on the Vita, Mesa on the host). Framebuffers live in GPU
// render targets; GS memory is kept coherent on demand:
//  - CPU-side writes to GS memory (transfers) are uploaded into render targets,
//  - render targets are downloaded to GS memory before anything reads it from
//    the CPU side (texture decode of a different layout, CLUT loads, readback).
// Textures are decoded with the software renderer's texture cache and
// uploaded as RGBA textures; textures that alias a render target are sampled
// from the render target directly.
//
// All OpenGL calls must happen on one thread: run the GS non-threaded and
// pump it with ProcessSingleFrame() from the thread owning the GL context.

#include <cstdio>
#include <functional>
#include <map>
#include <unordered_map>
#include <memory>
#include <string>
#include <vector>
#include "GSH_Primitives.h"
#include "GpuGl.h"
#include "TextureCache.h"

class CGSH_Hardware : public CGSH_Primitives
{
public:
	// Called on flip with the displayed image read back as RGBA8888. Only used
	// when frame readback is enabled (tests, host runner).
	using FrameSink = std::function<void(std::vector<uint32>& pixels, uint32 width, uint32 height)>;

	struct OPTIONS
	{
		bool readbackFrames = false;
		FrameSink frameSink;
	};

	// What the frontend needs to draw the current frame from the GPU.
	struct DISPLAY_TEXTURE
	{
		GLuint texture = 0;
		uint32 textureWidth = 0, textureHeight = 0;
		uint32 x = 0, y = 0, width = 0, height = 0; //displayed rectangle, in texels
	};

	explicit CGSH_Hardware(const OPTIONS&);
	~CGSH_Hardware() override;

	static FactoryFunction GetFactoryFunction(const OPTIONS&);

	// Runs queued GS work on the calling thread, which must own the GL
	// context. Waits up to timeoutMs for work when idle and returns after a
	// frame was presented or the queue ran empty. True if a frame was flipped.
	bool Pump(uint32 timeoutMs);

	// Frees GL objects; call on the GL thread before the handler is destroyed
	// (the handler itself is deleted on the emulation thread).
	void ReleaseGpu();

	// Speed hack: draw only one frame out of frameSkip + 1.
	void SetFrameSkip(uint32 frameSkip) override;

	// Appends one line per flip (displayed buffer, render target used, draws per
	// target since the previous flip) to a file, up to maxLines (diagnostics).
	void SetDiagnosticsFile(const char* path, uint32 maxLines);

	// After a state load: GS memory was replaced, so the GPU copies (render
	// targets, decoded textures) are dropped without writing them back; they
	// are rebuilt from GS memory. GS thread (the one owning the GL context).
	void DiscardGpuCopies();

	DISPLAY_TEXTURE GetDisplayTexture() const
	{
		return m_display;
	}

	void ProcessHostToLocalTransfer() override;
	void ProcessLocalToHostTransfer() override;
	void ProcessLocalToLocalTransfer() override;
	void ProcessClutTransfer(uint32, uint32) override;

	struct STATS
	{
		uint32 drawCalls = 0;
		uint32 stripShared = 0; //strip triangles that added one vertex (batch program)
		uint32 ringOverflows = 0; //batches drawn from client memory: the vertex ring was full
		uint32 targetDownloads = 0;
		uint32 depthRestores = 0; //depth set again after a new GPU scene (Vita)
		uint32 targetUploads = 0;
		uint32 textureUploads = 0;
		uint32 pictureUploads = 0; //of them, movie pictures sent from the decoder's buffers
		uint32 downloadedPixels = 0;
		uint32 uploadedPixels = 0;
		uint32 approximateBlends = 0;
		uint32 approximateWrapModes = 0; //region repeat masks GL cannot express
		uint32 targetsCreated = 0;
		uint32 targetsPooled = 0; //of them, GL objects reused from the pool (W1)
		uint32 transfersIn = 0, transfersOut = 0, transfersLocal = 0; //host->local, local->host, local->local
		uint32 deferredUploads = 0;   //target rows left to fill from GS memory on use (texture data over a target)
		uint32 skippedUploadRows = 0; //target rows an opaque sprite replaced before they were uploaded
		uint32 unchangedTransfers = 0; //host to local transfers that changed no byte of GS memory
		uint32 targetsShrunk = 0;      //targets cut short for a new one in their unused rows
		uint32 pointRects = 0;         //point runs uploaded as one rectangle
		uint32 pointsDrawn = 0;        //points drawn as points
		uint32 relayouts = 0;
		uint32 partialRelayouts = 0;  //old targets the new one started inside, copied on the GPU and shrunk (W1)
		uint32 alphaClutPasses = 0;    //alpha index textures converted on the GPU          //targets copied into a new layout on the GPU
		uint32 contentTextureHits = 0; //textures found by content (see BindTexture)
		uint32 redundantStateChanges = 0; //state rewrites that did not break batching //state changes with a blend GL cannot express
	};

	// (A - B) * C + D with A, B, D in {Cs, Cd, 0}: fixed-function blending
	// cannot express D == A when A is a color and A != B (e.g. Cs * (1 + As)).
	static bool IsBlendExact(uint32 a, uint32 b, uint32 d)
	{
		return (a == b) || (a == ALPHABLEND_ABD_ZERO) || (d != a);
	}
	STATS GetLastFrameStats() const
	{
		return m_lastFrameStats;
	}

	// Display diagnostics (frontend thread, between pumps): where flipped
	// frames came from, and the last displayed buffer.
	struct DISPLAY_STATS
	{
		uint32 fromTarget = 0;  //frames shown from a GPU render target
		uint32 fromMemory = 0;  //frames read from GS memory
		uint32 skipped = 0;     //flips that kept the previous frame (frame skip)
		uint32 disabled = 0;    //flips with the display circuit off or empty
		uint32 bufPtr = 0, bufWidth = 0, psm = 0, offsetY = 0; //last displayed buffer
	};
	DISPLAY_STATS GetDisplayStats() const
	{
		return m_displayStats;
	}
	STATS GetCurrentFrameStats() const
	{
		return m_stats;
	}

	// Time spent on the GS thread by stage since start, in microseconds
	// (perf.csv). Nested stages count toward the outermost one.
	enum GPU_STAGE
	{
		GPU_STAGE_GL,       //batch submission (FlushBatch)
		GPU_STAGE_TEXTURE,  //texture decode + upload (BindTexture)
		GPU_STAGE_TRANSFER, //GS memory transfers, target uploads/downloads
		GPU_STAGE_DISPLAY,  //flip: display image preparation
		GPU_STAGE_COUNT
	};
	// Finer parts (perf.csv gpu_parts, in this order), timed wherever they happen.
	enum GPU_PART
	{
		GPU_PART_DOWNLOAD_PIXELS, //read-back: glReadPixels
		GPU_PART_DOWNLOAD_SWIZZLE, //read-back: writing the pixels into GS memory
		GPU_PART_UPLOAD_SWIZZLE,  //target upload: reading GS memory
		GPU_PART_UPLOAD_GL,       //target upload: glTexSubImage2D
		GPU_PART_FEEDBACK_COPY,   //sampling the target being drawn: glCopyTexSubImage2D
		GPU_PART_TEX_LOOKUP,      //texture cache lookup (TextureCache::Get, page checks)
		GPU_PART_TEX_DECODE,
		GPU_PART_TEX_GL,
		GPU_PART_TARGET_CREATE,   //new render targets (GL objects)
		GPU_PART_DEPTH_CLEAR,     //depth restores at new GPU scenes
		GPU_PART_DRAW,            //glDrawArrays
		GPU_PART_LOCAL_COPY,      //local to local transfers
		GPU_PART_RELAYOUT,        //targets copied into a new layout on the GPU
		GPU_PART_ALPHA_CLUT,      //alpha index textures converted on the GPU
		GPU_PART_CONTENT_HITS,    //not a time: textures found by content, in thousandths (count = value)
		GPU_PART_DRAW_PROGRAM,    //batches drawn with a shader program (DrawBatchWithProgram)
		GPU_PART_DRAW_WORLD,      //RE4 world slots (FlushWorld)
		GPU_PART_DRAW_RETAINED,   //RE4 retained world draws (run setup and draws)
		GPU_PART_DRAWS_FF,        //not times: draw calls (count = value) by fixed function batches,
		GPU_PART_DRAWS_PROGRAM,   //batches with a program,
		GPU_PART_DRAWS_RETAINED,  //retained world draws
		GPU_PART_COUNT
	};
	struct GPU_TIMES
	{
		uint64 partUs[GPU_PART_COUNT] = {};
		uint64 pumpUs = 0; //everything done in Pump (the rest is primitive setup)
		uint64 stageUs[GPU_STAGE_COUNT] = {};
		uint64 texDecodeUs = 0, texGlUs = 0;         //texture misses: decoding, glTexImage2D
		uint64 uploadGlUs = 0, uploadPixels = 0;     //render target uploads: glTexSubImage2D only
		uint64 downloadGlUs = 0, downloadPixels = 0; //render target downloads: glReadPixels only
	};
	static GPU_TIMES& GetTimes()
	{
		static GPU_TIMES times;
		return times;
	}

protected:
	void InitializeImpl() override;
	void ReleaseImpl() override;
	void ResetImpl() override;
	void FlipImpl(const DISPLAY_INFO&) override;
	void MarkNewFrame() override;
	void BeginTransferWrite() override;
	void TransferWrite(const uint8*, uint32) override;
	void SyncCLUT(const TEX0&) override;

	void OnStateChanged() override;
	void OnDraw(CSoftwareRasterizer::PRIMITIVE_KIND, const CSoftwareRasterizer::VERTEX*) override;
	void OnDrawStripTriangle(const CSoftwareRasterizer::VERTEX*, bool continues) override;
	// Movie pictures' texels come from the decoder's buffer, not from GS memory (TextureCache)
	void OnImageDataSource(uint32 dbp, uint32 dbw, uint32 x, uint32 y, uint32 w, uint32 h, const std::shared_ptr<const uint8>& data) override
	{
		if(g_pictureTextures) m_textureCache.AddPictureSource(dbp, dbw, x, y, w, h, data);
	}

public:
	// Movie picture texels from the pictures' buffers (OnImageDataSource), on by default
	static inline bool g_pictureTextures = true;
	static inline uint32 g_pictureUploadCount = 0;
	// GL textures shared by textures with the same contents (content_textures, on by default)
	static inline bool g_contentTextures = true;
	// Targets copied into a new layout (buffer width) on the GPU (gpu_relayout, on by default)
	static inline bool g_gpuRelayout = true;
	static inline bool g_partialRelayout = true; //W1: also targets the new one starts inside (partial_relayout)
	static inline bool g_spriteCreatesTarget = true; //W1: an opaque first sprite creates its target without an upload (sprite_new_target)
	static inline bool g_stripCover = true; //W1: strip clears replace a new target's rows without an upload (strip_cover)
	static inline bool g_gpuWorldRetained = false; //W2: world geometry kept on the GPU (gpu_world_retained)
	static inline uint32 g_retainedBudgetMb = 24;  //W2: GPU memory for it (gpu_world_cache_mb)
	static inline uint64 g_retainedHits = 0, g_retainedMisses = 0, g_retainedEvictions = 0;
	// Consecutive retained batches with the same uniforms drawn as one (gpu_world_merge, on);
	// batches merged into the draw before them, and the draw calls issued
	static inline bool g_retainedMerge = true;
	static inline uint64 g_retainedMerged = 0, g_retainedDrawCalls = 0;
	static inline uint64 g_retainedInside = 0, g_retainedOutside = 0, g_retainedEdge = 0; //batches drawn retained, skipped, left to the clipping path
	static inline float g_retainedMegabytes = 0; //W2: GPU memory in use for it (perf.csv world_cache_mb)
	static inline bool g_retainedRunsOn = true; //W2: consecutive retained draws share their setup (gpu_world_runs)
	static inline uint64 g_retainedRuns = 0;    //runs started
	static inline uint64 g_retainedSegmentUs[6] = {}; //W2 diagnostics: matrix, hash, box, flush/setup, uniforms, draw call (perf.csv world_seg)
	static inline uint64 g_worldSlotUs = 0; //W2 diagnostics: time in the slot (non-retained) world path (perf.csv world_slot_ms)
	static inline uint64 g_regSplitUs[4] = {}; //diagnostics, inclusive: ApplyState, FlushBatch, OnDraw, OnDrawStripTriangle (perf.csv reg_split)
	static inline bool g_fineTimers = false; //per primitive and per batch timers (reg_split, world_seg, world_slot_ms): fine_timers=1; each costs two clock reads
	static inline bool g_feedShapes = false; //host --feed-shapes: host to local transfers counted by format, size and alignment
	static inline std::map<std::string, std::pair<uint64, uint64>> g_feedShapeCounts; //shape: transfers, bytes
	//GL draw calls by kind (0 generic batch with the batch program, 1 fixed function, 2 world slot draw, 3 retained world
	//draw) and the vertices/indices they pass (whole run; Vita gs_draws.txt, host end of run)
	static inline uint64 g_drawsByKind[4] = {}, g_drawElementsByKind[4] = {};
	//World batches by path (0 retained, 1 slot, 2 skinning slot, 3 other: CPU path or nothing drawn) and their vertices
	static inline uint64 g_worldBatchesByKind[4] = {}, g_worldVerticesByKind[4] = {};
	static inline uint64 g_worldClipCalls = 0; //slot path triangles sent to the CPU clipper (whole run)
	//clip_reject (on): triangles the clipper would return nothing for (Re4Vu1::ClipRejects) skip it;
	//g_clipRejectCheck (host --clip-reject-check) runs the clipper on them anyway and counts any it draws
	static inline bool g_clipReject = true, g_clipRejectCheck = false;
	//world_share_slots (on): a world batch with the open slot's constants (bit for bit) joins its draw
	static inline bool g_worldShareSlots = true;
	static inline bool g_worldUniformsUsed = true; //world_uniforms_used (on): world draws upload only the slots in use
	static inline uint64 g_worldSlotShares = 0; //batches that joined the open slot (whole run)
	static inline bool g_stripMerge = true; //strip_merge (see m_stripMergeOk)
	static inline bool g_stripMergeOpaque = true; //strip_merge_opaque: opaque sprites continuing a strip cover too
	static inline uint64 g_stripMergesOpaque = 0; //of those: opaque sprites continuing a strip cover
	static inline uint64 g_stripMerges = 0; //sprites drawn by joining the previous column's draw (whole run)
	static inline bool g_stateBreaks = false; //host --state-breaks: fixed function draws ended by a state change, by the fields that changed
	static inline std::map<std::string, std::pair<uint64, uint64>> g_stateBreakCounts; //fields: draws, vertices
	static inline uint64 g_drawsWithClippedWorld = 0, g_drawsWithClippedWorldVertices = 0; //fixed function draws carrying CPU-clipped world triangles
	static inline uint64 g_worldClipRejected = 0, g_clipRejectMismatches = 0;
	static inline uint64 g_feedBytesByPsm[64] = {}, g_feedCallsByPsm[64] = {}; //image data written, by destination format (whole run)
	static inline uint64 g_transfersInTotal = 0, g_unchangedTransfersTotal = 0; //host to local transfers, and those that changed no byte (whole run)
	static inline uint64 g_applyStateCalls = 0; //state applications (perf.csv apply_calls)
	static inline uint64 g_primitiveCalls = 0; //OnDraw and strip triangle calls (perf.csv prim_calls)
	static inline uint64 g_applySplitUs[3] = {}; //fine_timers: ApplyState target, texture, GL state (perf.csv apply_split)
	static inline uint64 g_stripCoverSkips = 0, g_stripCoverSettles = 0; //strip clears completed, cut short
	// Copies of the target being drawn made by a draw, not glCopyTexSubImage2D (feedback_draw, on)
	static inline bool g_feedbackDraw = true;
	// Alpha index textures (PSMT8H/4HL/4HH) of render targets converted on the GPU (gpu_alpha_clut, on)
	static inline bool g_gpuAlphaClut = true;
	// Target uploads and read-backs a GS block at a time (TargetPixels.h; target_pixel_blocks, on)
	static inline bool g_targetPixelBlocks = true;
	// Vita: depth buffers loaded at each new GPU scene (depth_force_load, on)
	static inline bool g_depthForceLoad = true;
	// Resident Evil 4's world batches (Re4Vu1Hle) transformed by a vertex shader (gpu_world, on)
	// Resident Evil 4: draws left out after the depth-of-field pass starts, as PCSX2's
	// GSC_ResidentEvil4 rule did (re4_skip_dof, 0 = off, PCSX2 used 176). The game loses
	// its depth blur; the Z read-back and the blur round trips go away.
	static inline uint32 g_re4SkipDof = 0;
	// Ordinary batches drawn with one GLSL program instead of the fixed-function texture
	// combiner, fog unit and alpha test (gs_shaders, off: ~27 us of GL time per batch on the Vita
	// against ~63 us through vitaGL's fixed-function path, but RE4 crashed in 4 of 9 runs with
	// it on, 24 Nov 2026: a trap in a system allocator, heap corruption suspected)
	static inline bool g_shaderBatches = false;
	// Compiles and links a series of shader pairs (simplest first, then the batch and world
	// programs), writing a line before and after each link to 'path' (closed each time, so a
	// crash leaves the step it died in). For vitaGL's GLSL translation on the Vita.
	static void RunShaderSelfTest(const char* path);
	// With gs_shaders: batch vertices in a ring of GPU buffers, one copy instead of vitaGL's
	// copy per attribute (gs_vertex_ring, on)
	static inline bool g_vertexRing = true;
	// Size of each vertex ring buffer (gs_vertex_ring_kb, at most 4096; batches that do not fit go
	// from client memory). Four 4 MB buffers left too little GPU memory: RE4 crashed in texture
	// uploads (24 Nov 2026)
	static inline uint32 g_vertexRingKb = 1024;
	static inline uint64 g_ringDraws = 0, g_ringOverflows = 0, g_ringBytes = 0, g_stripShared = 0, g_stripCalls = 0, g_stripContinues = 0, g_triangles = 0;
	static inline uint64 g_skippedDraws = 0;
	static inline bool g_gpuWorld = false; //right on the PC, not yet run on the Vita (docs/RE4_GAMEPLAY.md)
	static inline uint64 g_gpuWorldBatches = 0, g_gpuWorldDraws = 0, g_gpuWorldFallbacks = 0;
	static inline uint32 g_gpuWorldDebug = 0; //1: triangles near the clip volume left out
	//Batches per world draw, at most WORLD_SLOTS (gpu_world_slots). The Vita: 1. The vertex
	//shader picks its batch's matrix with uM[slot * 4], and with a slot above 0 the Vita drew
	//some triangles wrong or not at all (grey patches near the camera; vitaGL's translation of
	//the dynamic uniform index, most likely). One batch per draw is clean there, at the same speed.
#if defined(__vita__)
	static inline uint32 g_gpuWorldSlots = 1;
#else
	static inline uint32 g_gpuWorldSlots = 16;
#endif
	//Triangles with a corner more than this many hundredths of a screen beyond the target's
	//edge are drawn by the CPU path (gpu_world_guard; 0: off)
	static inline uint32 g_gpuWorldGuard = 0;
	static inline uint64 g_mailCalls = 0; //GPU thread mailbox calls received (G0: W5 sizes the queue records)
	static inline bool g_targetPool = true; //W1: reuse released targets' GL objects (target_pool)
	//target_pool_lru (on): a full pool makes room by deleting its oldest entry (not the target released
	//now), and entries not reused within g_targetPoolMaxAge frames are deleted (RE4 kept 18 MB of
	//1024-wide targets from its menus in the pool for good while the 256x512/256x384 targets it
	//uses every frame were created and deleted twice a game frame)
	static inline bool g_targetPoolLru = true;
	//host --readback-liveness: GS memory blocks (256 bytes) filled by read-backs, and whether anything
	//reads them before a later read-back replaces them (what a deferred read-back could skip)
	static inline bool g_readbackLiveness = false;
	static inline std::vector<uint8> g_liveBlocks; //0 none, else 1 + the read-back's cause
	static inline uint64 g_liveDownloaded[16] = {}, g_liveNeeded[16] = {}, g_liveSuperseded[16] = {}, g_liveOverwritten[16] = {};
	void LivenessDownloaded(uint32 start, uint32 end);
	void LivenessRead(uint32 start, uint32 size, uint32 reader);
	static inline uint64 g_liveNeededBy[16][16] = {}; //reader: 0 texture, 1 CLUT, 2 local copy, 3 local to host, 4 display, 5+ upload (by cause % 10)
	static inline bool g_gpuUpload = true; //gpu_upload (on): target uploads drawn from a scratch texture (see UploadTarget)
	static inline uint64 g_gpuUploads = 0;
	static inline uint32 g_targetPoolMaxAge = 300;
	static inline uint32 g_targetPoolMinAge = 1; //target_pool_min_age: frames before a pooled target is reused
	static inline uint64 g_targetPoolEvictions = 0;
	//vitaGL's free memory (VRAM + RAM pools, bytes) before each new render target's GL objects, and the
	//lowest seen (perf.csv gl_free_mb, gl_free_min_mb; Vita); new allocations (not from the pool)
	static inline std::map<std::string, uint64> g_targetMisses; //host --state-breaks: new render targets by size and pool contents
	static inline uint64 g_glFreeBeforeTarget = 0, g_glFreeMin = UINT64_MAX, g_targetAllocs = 0;
	static uint64 GlFreeBytes();
	size_t GetTargetPoolSize() const { return m_targetPool.size(); }
	size_t GetTargetCount() const { return m_targets.size(); }
	static inline uint64 g_targetsPooledTotal = 0; //targets that got pooled GL objects (perf.csv targets_pooled)
	//World vertices in one array per attribute (gpu_world_soa, on)
	static inline bool g_gpuWorldSoa = false;
	static inline uint64 g_gpuWorldGuarded = 0, g_gpuWorldMaxVertices = 0;
	static inline uint32 g_depthForceLoadTargets = 0, g_depthForceLoadFailures = 0;
	// Pictures the game finished: flips showing another target, or one drawn into since
	// the previous flip (vblanks with the same picture don't count). perf.csv game_fps.
	static inline uint32 g_gameFrames = 0;
	static inline const char* g_glObjectDumpPath = nullptr; //Vita: vitaGL object bytes, once (layout check) //movie pictures uploaded from their buffers (perf.csv gpu_pic_up)

public:
	// Diagnostics: the picture of the next flip goes to this file (PPM). Same
	// thread as the GS. Independent of any screen capture tool.
	void RequestFrameDump(const std::string& path)
	{
		m_frameDumpPath = path;
	}

	void SetDebugFlags(uint32 flags)
	{
		m_debugFlags = flags;
	}
	// Diagnostics: every render target at the next flip, as PPM files.
	void RequestTargetDump(const std::string& directory)
	{
		m_targetDumpDirectory = directory;
	}

private:
	uint32 m_debugFlags = 0;
	const void* m_depthSceneTarget = nullptr;
	bool m_depthSceneValid = false;
	void DumpTargets();
	std::string m_targetDumpDirectory;
	void WriteFrameDump(const uint32* pixels, uint32 width, uint32 height);
	std::string m_frameDumpPath;

	struct TARGET
	{
		uint32 fbp = 0;  //bytes
		uint32 fbw = 0;  //units of 64 pixels
		uint32 psm = 0;
		uint32 width = 0, height = 0;
		GLuint framebuffer = 0;
		GLuint colorTexture = 0;
		GLuint depthBuffer = 0;
		// Rows [0, validRows) of the GPU copy are current; more rows are
		// filled from GS memory when drawing, display or sampling needs them.
		uint32 validRows = 0;
		// Rows below this were drawn by the GPU at least once. A target can
		// reach further (heights are rounded up) into memory that belongs to
		// something else, such as textures kept right after the frame.
		uint32 drawnRows = 0;
		// Rows [staleBegin, staleEnd) inside them are older than GS memory (a
		// transfer in another layout wrote there): filled from it like rows past
		// validRows, or never if an opaque sprite covers them first. Not dirty.
		uint32 staleBegin = 0, staleEnd = 0;
		// Drawn with depth testing or writes (its depth buffer must be kept).
		bool depthUsed = false;
		// Vita: its depth buffer is loaded at each new GPU scene (GXM force load), so
		// depth survives target switches without the clear in ApplyState.
		bool depthKept = false;
		uint32 depthForceTries = 0;
		uint32 drawBatches = 0; //batches drawn into it (game frame counting)
		uint32 lastUseFrame = 0;
		// Pixels rendered by the GPU and not yet written back to GS memory.
		bool gpuDirty = false;
		// Some of those pixels had their alpha written (else GS memory's alpha is current).
		bool alphaDirty = false;
		uint32 dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0;
		uint32 lastUse = 0;
	};

	struct BATCH_VERTEX
	{
		// Clip space position; w = 1/Q makes the GPU's perspective correct
		// interpolation reproduce STQ texturing with plain 2D texcoords.
		float x, y, z, w;
		uint8 r, g, b, a;
		float s, t;
		float fogS, fogT; //texture unit 1: fog factor lookup in a ramp
	};

	struct CACHED_TEXTURE
	{
		GLuint texture = 0;
		uint32 generation = 0;
		uint32 lastUse = 0;
	};

	TARGET* FindTarget(uint32 fbp, uint32 fbw, uint32 psm, bool create, uint32 minHeight);
	// A target that holds the buffer at 'ptr' (same layout) 'rows' high,
	// starting some page rows into it.
	TARGET* FindTargetContaining(uint32 ptr, uint32 fbw, uint32 psm, uint32 rows, uint32& offsetX, uint32& offsetY);
	void DeleteTarget(TARGET&);
	// Same pixel size, inside the new target's memory [start, end): copy instead of a read-back.
	bool CanRelayout(const TARGET& old, const TARGET& target, uint32 start, uint32 end) const;
	void RelayoutTarget(TARGET& old, TARGET& target);
public:
	void DrawWorldBatch(const Re4Vu1::WORLD_BATCH&) override;

private:
	// World batches waiting for one draw (same GS state): vertices (raw P, T, C, slot) and
	// triangles, constants per batch (slot).
	enum
	{
		WORLD_SLOTS = 16
	};
	struct WORLD_GPU_VERTEX
	{
		float p[4], t[4], c[4];
		float slot;
	};
	std::vector<WORLD_GPU_VERTEX> m_worldVertices;
	std::vector<float> m_worldSoa; //gpu_world_soa: one array per attribute
	bool m_batchHasClippedWorld = false; //m_batch holds CPU-clipped world triangles (counts only)
	// strip_merge: a sprite drawn again, unchanged, with only the scissor moved to the next
	// column (RE4 draws full-screen passes in 32/64 pixel columns) joins the draw of the first
	// one, under the union of the columns: every pixel gets the same values and one write, as
	// before. Only while the batch holds nothing but that sprite (and no world vertices,
	// points or retained draw are waiting), the sprite does not sample its own target, the RE4
	// draw-skip rule is idle and the strip-cover logic (W1) would do nothing.
	bool m_stripMergeOk = false;      //m_batch holds exactly one sprite, m_stripSprite
	bool m_stripUnionActive = false;  //the GL scissor is m_stripUnion, not m_state's
	bool m_stripExpectSprite = false; //a scissor change was taken in; the next primitive must be the sprite again
	bool m_boundFeedback = false;     //the bound texture is the copy of the target being drawn
	CSoftwareRasterizer::VERTEX m_stripSprite[2] = {};
	int32 m_stripUnion[4] = {};       //x0, y0, x1, y1 (inclusive pixels)
	int m_batchScissorFit = 0; //m_batch's primitives against their scissors: 0 strictly inside, 1 an edge on the scissor's, 2 crossing
	std::vector<uint16> m_worldIndices;
	float m_worldMatrix[WORLD_SLOTS * 16] = {};
	float m_worldColorOffset[WORLD_SLOTS * 4] = {}, m_worldColorScale[WORLD_SLOTS * 4] = {}, m_worldTexOffset[WORLD_SLOTS * 4] = {};
	uint32 m_worldSlots = 0;
	bool m_worldSlotProjected = false; //the last slot holds projected skinning batches (gpu_skin)
	GLuint m_worldProgram = 0;
	bool m_worldProgramFailed = false;
	GLint m_worldLocations[16] = {};
	bool BuildWorldProgram();
	void FlushWorld();
	// W2: world batches kept on the GPU. A batch's vertices (floats) and triangles go into
	// static buffers once, keyed by a hash of its data; each draw sets one matrix that folds the
	// projection, the 4096 pixel wrap, w > 0 and the depth mapping, and the GPU transforms and
	// clips (gpu_world_retained).
	// Batches' vertices and triangles share large buffers (chunks), in the order first seen,
	// so the next batch drawn with the same uniforms is usually right after the previous one
	// in the same chunk: the two become one draw (92% of RE4's retained draws, PC count).
	struct RETAINED_CHUNK
	{
		GLuint vbo = 0, ibo = 0;
		uint32 vertices = 0, indices = 0; //used
	};
	static constexpr uint32 RETAINED_CHUNK_VERTICES = 65535;    //16-bit indices
	std::vector<RETAINED_CHUNK> m_retainedChunks;
	// This frame's new batches, a chunk made at the end of the frame (MarkNewFrame)
	std::vector<float> m_stagedVertices;
	std::vector<uint16> m_stagedIndices;
	std::vector<uint64> m_stagedKeys;
	void SealRetainedChunk();
	struct RETAINED_WORLD
	{
		bool built = false;  //in a chunk
		bool staged = false; //new this frame: in the chunk being staged (drawn by the slot path)
		uint32 chunk = 0, firstIndex = 0;
		uint32 indexCount = 0, bytes = 0, lastFrame = 0;
		float boxMin[3] = {}, boxMax[3] = {}; //object space bounds of its positions
	};
	std::unordered_map<uint64, RETAINED_WORLD> m_retainedWorld;
	uint64 m_retainedBytes = 0;
	// The retained draw not issued yet (extended while batches follow on with the same
	// uniforms; issued at the next other one, or when the run ends)
	struct RETAINED_PENDING
	{
		bool active = false;
		uint32 chunk = 0, firstIndex = 0, indexCount = 0;
		float uniforms[28] = {}; //projection (16), color offset (4), color scale (4), vf31/fog (4)
	};
	RETAINED_PENDING m_retainedPending;
	uint32 m_retainedBoundChunk = ~0u; //chunk whose buffers the retained streams point at
	void FlushRetainedDraw();
	GLuint m_retainedProgram = 0;
	bool m_retainedProgramFailed = false;
	GLint m_retainedLocations[9] = {};
	bool BuildRetainedProgram();
	bool DrawRetainedWorld(const Re4Vu1::WORLD_BATCH&);
	void SetWorldFragmentUniforms(GLint tex, GLint texXf, GLint mode, GLint fog, GLint alpha);
	void TrimRetainedWorld();
	void ReleaseRetainedWorld();
	bool m_retainedRun = false; //W2: the retained program and streams are set up (EndRetainedRun undoes it)
	void EndRetainedRun();
	// Alpha test as ApplyState set it (GL function, reference), for the shader
	GLenum m_alphaTestFunc = GL_ALWAYS;
	float m_alphaTestRef = 0;
	// The bound texture is an alpha index texture (PSMT8H/4HL/4HH) lying in a render target
	// whose alpha the GPU drew: converted through the CLUT by a shader pass. False: not handled.
	bool BindAlphaClutTexture();
	GLuint m_alphaClutProgram = 0;
	bool m_alphaClutFailed = false;
	GLint m_alphaClutSourceLocation = -1, m_alphaClutClutLocation = -1, m_alphaClutModeLocation = -1;
	GLuint m_alphaClutTexture = 0; //256x1 palette
	struct ALPHA_CLUT_TARGET
	{
		GLuint texture = 0;
		GLuint framebuffer = 0;
	};
	std::map<uint64, std::vector<ALPHA_CLUT_TARGET>> m_alphaClutTargets; //by size (w << 32 | h)
	uint32 m_alphaClutNext = 0;
	// Textured quads (clip space positions, normalized texcoords) from a texture into a
	// framebuffer, nothing but the color written; GL state is applied again afterwards.
	void DrawCopyQuads(GLuint texture, GLuint framebuffer, uint32 width, uint32 height, const std::vector<BATCH_VERTEX>& quads);
	// A rectangle of a texture to the same place of another of the same size.
	void CopyRegionOnGpu(GLuint texture, uint32 width, uint32 height, GLuint framebuffer, uint32 x, uint32 y, uint32 w, uint32 h);
	void AllocateTargetGl(TARGET&);
	// W1: GL objects of released targets, reused by AllocateTargetGl for the same size
	struct POOLED_TARGET
	{
		uint32 width, height;
		GLuint framebuffer, colorTexture, depthBuffer;
		uint32 releasedFrame = 0; //m_frameNumber when it went into the pool
	};
	void DeletePooledTarget(const POOLED_TARGET&);
	enum { TARGET_POOL_SIZE = 12 };
	std::vector<POOLED_TARGET> m_targetPool;
	uint32 m_diagFirstKind = 4; //diagnostics: kind of the primitive that applied the state (S entries)
	void ReleaseTargetPool();
	// Keeps only the first rows of a target (on the GPU, no read back).
	void ShrinkTarget(TARGET&, uint32 rows);
	// W1: drops the first rows (whole page rows): the target starts after them
	void TrimTargetFront(TARGET&, uint32 rows);
	void RemoveTargetsOverlapping(const TARGET* except, uint32 start, uint32 size);
	void EnsureValidRows(TARGET&, uint32 rows);
	// Rows a transfer rewrote in another layout: uploaded when used (stale range).
	void MarkRowsStale(TARGET&, uint32 rowBegin, uint32 rowEnd);
	void UploadStaleRows(TARGET&, uint32 rowEnd);
	// Before the first primitive of a state: an opaque sprite over whole rows
	// not uploaded yet makes them valid without an upload.
	bool SkipOverwrittenRows(const CSoftwareRasterizer::VERTEX&, const CSoftwareRasterizer::VERTEX&);
	// W1: a target cleared by full-height sprites in vertical strips (scissor 64 wide, RE4):
	// its new rows are not uploaded; columns [0, coveredX) are drawn. Anything else that
	// touches GPU targets or GS memory first uploads the rest (SettleStripCover).
	struct STRIP_COVER
	{
		TARGET* target = nullptr;
		uint32 begin = 0, end = 0, coveredX = 0;
	};
	STRIP_COVER m_stripCover;
	void SettleStripCover();
	void MarkTargetDirty(TARGET&, int32 x0, int32 y0, int32 x1, int32 y1);
	uint32 m_roundTripCause = 0; //diagnostics (g_roundTripPixels)

public:
	// Pixels read back [0] and uploaded [1] by cause: 1 FindTarget, 2 stale rows, 3 rows made
	// stale, 4 valid rows, 5 transfer into GS memory, 6 transfer out, 7 local transfer, 8 CLUT,
	// 9 texture, 10 strip cover, 11 overwritten rows, 12 flip, 13 overlapping targets removed
	static inline uint64 g_roundTripPixels[2][16] = {};

private:
	// GS memory spanned by a range of rows (whole page rows).
	void RowsToBytes(const TARGET&, uint32 rowBegin, uint32 rowEnd, uint32& start, uint32& end) const;
	// Rows of the target stored in a range of GS memory.
	bool BytesToRows(const TARGET&, uint32 start, uint32 size, uint32& rowBegin, uint32& rowEnd) const;
	bool RowsOverlap(const TARGET&, uint32 rowBegin, uint32 rowEnd, uint32 start, uint32 size) const;

	void UploadTarget(TARGET&, uint32 x, uint32 y, uint32 width, uint32 height);
	// Writes GPU pixels of rows [rowBegin, rowEnd) of the dirty rectangle back to GS memory.
	void DownloadTarget(TARGET&, uint32 rowBegin = 0, uint32 rowEnd = ~0U);
	// overwritten: the caller writes every byte of the range next.
	void DownloadTargetsOverlapping(uint32 start, uint32 size, bool overwritten = false, bool alphaOnly = false);
	// Forgets GPU pixels of rows [rowBegin, rowEnd) (overwritten in GS memory). False
	// when they are in the middle of the dirty rectangle (it cannot have holes).
	bool DiscardDirtyRows(TARGET&, uint32 rowBegin, uint32 rowEnd);
	void UploadTargetsOverlapping(uint32 start, uint32 size);

	// Part of a decoded texture uploaded as a GL texture, with its wrap mode:
	// region clamp/repeat modes become a sub-texture with clamp/repeat.
	struct AXIS_REGION
	{
		uint32 origin = 0;
		uint32 size = 0;
		GLint wrap = GL_REPEAT;
	};
	AXIS_REGION ResolveWrap(uint32 mode, uint32 minValue, uint32 maxValue, uint32 size);

	void WriteFlipDiagnostics(const DISPLAY_INFO&);
	void ApplyState();
	bool SkipDrawRule();
	bool WouldSkipDraw() const;
	bool WouldSkipDrawFor(const CSoftwareRasterizer::STATE&) const;
	void FlushBatchImpl();
	bool StripMergeAllowed(const CSoftwareRasterizer::STATE&) const;
	bool StripCoverContinues(const CSoftwareRasterizer::STATE&, const CSoftwareRasterizer::VERTEX&, const CSoftwareRasterizer::VERTEX&) const;
	uint32 m_skipDraws = 0;
	// Batch program (gs_shaders)
	bool BuildBatchProgram();
	void SetBatchUniforms(bool fixAlpha);
	void DrawBatchWithProgram();
	GLuint m_batchProgram = 0;
	bool m_batchProgramFailed = false;
	bool m_batchShaderActive = false;
	GLint m_batchLocations[8] = {};
	// Vertex ring (gs_vertex_ring, with gs_shaders): a buffer per GPU frame
	enum
	{
		VERTEX_RING_COUNT = 4,
		VERTEX_RING_MAX = 4 * 1024 * 1024,
	};
	bool UploadToRing(const void* data, uint32 size, uint32& offset);
	static uint32 RingSize()
	{
		return std::min<uint32>(std::max<uint32>(g_vertexRingKb, 64), 4096) * 1024;
	}
	GLuint m_vertexRing[VERTEX_RING_COUNT] = {};
	uint8* m_vertexRingMemory[VERTEX_RING_COUNT] = {};
	uint32 m_ringCurrent = 0;
	uint32 m_ringOffset = 0;
	uint32 m_ringFrame = ~0U;
	bool m_ringFailed = false;
	// AddVertex's reciprocals and what they were computed for (target width, height, Z format,
	// texture width, height)
	uint32 m_vertexKey[5] = {};
	float m_vertexScaleX = 0, m_vertexScaleY = 0, m_vertexInvTw = 0, m_vertexInvTh = 0;
	uint32 m_vertexZMax = 1;
	double m_vertexZScale = 0;
	void SetupAlphaCombiner(bool fix);
	void SetupFog();
	void BindTexture();
	void FlushBatch();
	void AddVertex(const CSoftwareRasterizer::VERTEX&);

	OPTIONS m_options;
	bool m_gpuInitialized = false;
	uint32 m_frameSkip = 0;
	uint32 m_frameCounter = 0;
	bool m_lastFrameSkipped = false;
	CTextureCache m_textureCache;
	std::vector<std::unique_ptr<TARGET>> m_targets;
	// Keyed by texture cache entry and uploaded region (see TEXTURE_REGION).
	std::map<std::pair<uint64, uint64>, CACHED_TEXTURE> m_glTextures;
	uint32 m_useCounter = 0;
	uint32 m_frameNumber = 0; //MarkNewFrame calls
	uint64 m_clutHash = 0;

	CSoftwareRasterizer::STATE m_state;
	bool m_stateApplied = false;
	bool m_drawNothing = false;
	TARGET* m_currentTarget = nullptr;
	const TARGET* m_diagLastTarget = nullptr;
	const TARGET* m_lastDisplayedTarget = nullptr;
	uint32 m_lastDisplayedBatches = 0;
	GLenum m_batchMode = GL_TRIANGLES;
	std::vector<BATCH_VERTEX> m_batch;
	// Batch program, triangle batches: indices into m_batch (strip triangles share vertices)
	std::vector<uint16> m_batchIndices;
	size_t m_stripEnd = ~size_t(0); //m_batch's size after the last strip triangle

	// Points that only write their color (no texture, blending, tests, mask or
	// fog) collect in a pixel buffer; when they fill their bounding rectangle
	// it is uploaded at once instead of drawn as points (Silent Hill 2's movies
	// draw every pixel of a 512x192 field as a point: 98K per frame).
	struct PENDING_POINT
	{
		int32 x, y; //12.4 fixed point, as in the vertex
		uint32 rgba;
	};
	bool m_pointsDirect = false; //the applied state allows it
	TARGET* m_pointTarget = nullptr;
	std::vector<PENDING_POINT> m_points;
	std::vector<uint32> m_pointPixels;
	std::vector<uint8> m_pointWritten;
	int32 m_pointX0 = 0, m_pointY0 = 0, m_pointX1 = 0, m_pointY1 = 0;
	uint32 m_pointUnique = 0;
	void AddDirectPoint(const CSoftwareRasterizer::VERTEX&);
	void FlushPoints();
	// Texture coordinate transform for render target textures.
	float m_texScaleS = 1, m_texScaleT = 1, m_texOffsetS = 0, m_texOffsetT = 0;
	bool m_fixAlpha = false;
	bool m_alphaPass = false;
	bool m_colorMask[4] = {true, true, true, true};
	GLenum m_depthFunc = GL_ALWAYS;
	bool m_depthWrite = false;
	// Render target textures store alpha doubled (0x80 -> 255).
	bool m_textureAlphaDoubled = false;

	DISPLAY_TEXTURE m_display;
	GLuint m_displayUploadTexture = 0;
	GLuint m_fogRamp = 0;
	GLuint m_feedbackTexture = 0;
	GLuint m_feedbackFramebuffer = 0;
	uint32 m_feedbackWidth = 0, m_feedbackHeight = 0;
	std::vector<uint32> m_frameBuffer;
	std::vector<uint32> m_transferPixels;
	STATS m_stats;
	STATS m_lastFrameStats;
	bool m_transferDiscards = false; //the current transfer dropped GPU pixels without a download
	DISPLAY_STATS m_displayStats;
	std::FILE* m_diagFile = nullptr;
	uint32 m_diagLinesLeft = 0;
	uint32 m_diagFlips = 0;
	// Draw batches per (fbp, fbw, psm) since the last flip; dropped: no color target.
	std::map<uint64, uint32> m_diagDraws, m_diagDropped;
	std::string m_diagUploads; //host->local transfers since the last flip
	std::string m_diagPrims;   //sprites drawn since the last flip
	std::map<uint32, uint32> m_diagSpriteCount; //sprites logged per target since the last flip
	uint32 m_diagKinds[4] = {}; //points, lines, triangles, sprites since the last flip
};
