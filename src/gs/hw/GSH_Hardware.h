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
		uint32 approximateBlends = 0; //state changes with a blend GL cannot express
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
		bool gpuDirty = false; //rendered pixels not yet in GS memory
		uint32 lastUse = 0;
	};

	struct BATCH_VERTEX
	{
		// Clip space position; w = 1/Q makes the GPU's perspective correct
		// interpolation reproduce STQ texturing with plain 2D texcoords.
		float x, y, z, w;
		uint8 r, g, b, a;
		float s, t;
	};

	struct CACHED_TEXTURE
	{
		GLuint texture = 0;
		uint32 generation = 0;
		uint32 lastUse = 0;
	};

	TARGET* FindTarget(uint32 fbp, uint32 fbw, uint32 psm, bool create, uint32 minHeight);
	void DeleteTarget(TARGET&);
	bool RangesOverlap(const TARGET&, uint32 start, uint32 size) const;
	uint32 TargetBytes(const TARGET&) const;

	void UploadTarget(TARGET&, uint32 x, uint32 y, uint32 width, uint32 height);
	void DownloadTarget(TARGET&);
	void DownloadTargetsOverlapping(uint32 start, uint32 size);
	void UploadTargetsOverlapping(uint32 start, uint32 size);

	void ApplyState();
	void SetupAlphaCombiner(bool fix);
	void BindTexture();
	void FlushBatch();
	void AddVertex(const CSoftwareRasterizer::VERTEX&);

	OPTIONS m_options;
	bool m_gpuInitialized = false;
	CTextureCache m_textureCache;
	std::vector<std::unique_ptr<TARGET>> m_targets;
	std::map<uint64, CACHED_TEXTURE> m_glTextures;
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
	std::vector<uint32> m_frameBuffer;
	STATS m_stats;
	STATS m_lastFrameStats;
};
