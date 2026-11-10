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
		uint32 transfersIn = 0, transfersOut = 0, transfersLocal = 0; //host->local, local->host, local->local
		uint32 deferredUploads = 0;   //target rows left to fill from GS memory on use (texture data over a target)
		uint32 skippedUploadRows = 0; //target rows an opaque sprite replaced before they were uploaded
		uint32 unchangedTransfers = 0; //host to local transfers that changed no byte of GS memory
		uint32 targetsShrunk = 0;      //targets cut short for a new one in their unused rows
		uint32 pointRects = 0;         //point runs uploaded as one rectangle
		uint32 pointsDrawn = 0;        //points drawn as points
		uint32 relayouts = 0;
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
	void SyncCLUT(const TEX0&) override;

	void OnStateChanged() override;
	void OnDraw(CSoftwareRasterizer::PRIMITIVE_KIND, const CSoftwareRasterizer::VERTEX*) override;
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
	// Copies of the target being drawn made by a draw, not glCopyTexSubImage2D (feedback_draw, on)
	static inline bool g_feedbackDraw = true;
	// Alpha index textures (PSMT8H/4HL/4HH) of render targets converted on the GPU (gpu_alpha_clut, on)
	static inline bool g_gpuAlphaClut = true;
	// Vita: depth buffers loaded at each new GPU scene (depth_force_load, on)
	static inline bool g_depthForceLoad = true;
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
	// Keeps only the first rows of a target (on the GPU, no read back).
	void ShrinkTarget(TARGET&, uint32 rows);
	void RemoveTargetsOverlapping(const TARGET* except, uint32 start, uint32 size);
	void EnsureValidRows(TARGET&, uint32 rows);
	// Rows a transfer rewrote in another layout: uploaded when used (stale range).
	void MarkRowsStale(TARGET&, uint32 rowBegin, uint32 rowEnd);
	void UploadStaleRows(TARGET&, uint32 rowEnd);
	// Before the first primitive of a state: an opaque sprite over whole rows
	// not uploaded yet makes them valid without an upload.
	void SkipOverwrittenRows(const CSoftwareRasterizer::VERTEX&, const CSoftwareRasterizer::VERTEX&);
	void MarkTargetDirty(TARGET&, int32 x0, int32 y0, int32 x1, int32 y1);
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
	const TARGET* m_lastDisplayedTarget = nullptr;
	uint32 m_lastDisplayedBatches = 0;
	GLenum m_batchMode = GL_TRIANGLES;
	std::vector<BATCH_VERTEX> m_batch;

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
