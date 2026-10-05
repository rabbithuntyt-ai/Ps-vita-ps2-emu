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

#include <functional>
#include <map>
#include <memory>
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
		uint32 targetUploads = 0;
		uint32 textureUploads = 0;
		uint32 downloadedPixels = 0;
		uint32 uploadedPixels = 0;
		uint32 approximateBlends = 0;
		uint32 approximateWrapModes = 0; //region repeat masks GL cannot express
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

private:
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
		// Pixels rendered by the GPU and not yet written back to GS memory.
		bool gpuDirty = false;
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
	void RemoveTargetsOverlapping(const TARGET* except, uint32 start, uint32 size);
	void EnsureValidRows(TARGET&, uint32 rows);
	void MarkTargetDirty(TARGET&, int32 x0, int32 y0, int32 x1, int32 y1);
	// GS memory spanned by a range of rows (whole page rows).
	void RowsToBytes(const TARGET&, uint32 rowBegin, uint32 rowEnd, uint32& start, uint32& end) const;
	// Rows of the target stored in a range of GS memory.
	bool BytesToRows(const TARGET&, uint32 start, uint32 size, uint32& rowBegin, uint32& rowEnd) const;
	bool RowsOverlap(const TARGET&, uint32 rowBegin, uint32 rowEnd, uint32 start, uint32 size) const;

	void UploadTarget(TARGET&, uint32 x, uint32 y, uint32 width, uint32 height);
	void DownloadTarget(TARGET&);
	void DownloadTargetsOverlapping(uint32 start, uint32 size);
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
	uint64 m_clutHash = 0;

	CSoftwareRasterizer::STATE m_state;
	bool m_stateApplied = false;
	bool m_drawNothing = false;
	TARGET* m_currentTarget = nullptr;
	GLenum m_batchMode = GL_TRIANGLES;
	std::vector<BATCH_VERTEX> m_batch;
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
	uint32 m_feedbackWidth = 0, m_feedbackHeight = 0;
	std::vector<uint32> m_frameBuffer;
	std::vector<uint32> m_transferPixels;
	STATS m_stats;
	STATS m_lastFrameStats;
	DISPLAY_STATS m_displayStats;
};
