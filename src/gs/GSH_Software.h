#pragma once

#include <functional>
#include <vector>
#include "GSH_Primitives.h"
#include <atomic>
#include <memory>
#include "SoftwareRasterizer.h"
#include "ParallelRasterizer.h"

// GS handler backed by CSoftwareRasterizer. All drawing happens in the
// emulated GS local memory; on each flip the displayed buffer is converted to
// RGBA8888 and handed to the frontend through the frame sink.
class CGSH_Software : public CGSH_Primitives
{
public:
	// Called on the GS thread with a tightly packed RGBA8888 (R in the low byte)
	// image of the currently displayed buffer. The sink may take the buffer's
	// contents (swap) and leave another buffer in its place.
	using FrameSink = std::function<void(std::vector<uint32>& pixels, uint32 width, uint32 height)>;

	explicit CGSH_Software(bool gsThreaded = true);
	~CGSH_Software() override = default;

	struct OPTIONS
	{
		bool gsThreaded = true;
		uint32 rasterizerThreads = 1;
		bool interlacedRendering = false;
		uint32 frameSkip = 0;
	};

	static FactoryFunction GetFactoryFunction(FrameSink sink, const OPTIONS&);

	void SetFrameSink(FrameSink);

	void ProcessHostToLocalTransfer() override;
	void ProcessLocalToHostTransfer() override;
	void ProcessLocalToLocalTransfer() override;
	void ProcessClutTransfer(uint32, uint32) override;

	// Time the GS spent rasterizing during the last completed frame (us).
	uint32 GetLastFrameRasterMicros() const
	{
		return m_lastFrameRasterMicros.load();
	}

	// Speed hacks -------------------------------------------------------------
	// Interlaced rendering: rasterize only every other row, alternating each
	// frame. Halves fill cost; can leave artifacts in render-to-texture effects.
	void SetInterlacedRendering(bool);
	// Skip drawing of N frames out of N+1 (0 = draw everything).
	void SetFrameSkip(uint32);
	// Number of threads rasterizing (including the GS thread). 1 = single threaded.
	// Must be called before Initialize().
	void SetRasterizerThreads(uint32);

protected:
	void InitializeImpl() override;
	void ReleaseImpl() override;
	void ResetImpl() override;
	void FlipImpl(const DISPLAY_INFO&) override;
	void MarkNewFrame() override;
	void TransferWrite(const uint8*, uint32) override;
	void SyncCLUT(const TEX0&) override;

private:
	void OnStateChanged() override;
	void OnDraw(CSoftwareRasterizer::PRIMITIVE_KIND, const CSoftwareRasterizer::VERTEX*) override;
	void Submit(CSoftwareRasterizer::PRIMITIVE_KIND, const CSoftwareRasterizer::VERTEX*);
	void FlushRendering();
	void UpdateRowFilter();

	CSoftwareRasterizer m_rasterizer;
	std::unique_ptr<CParallelRasterizer> m_parallel;
	uint32 m_rasterizerThreads = 1;
	FrameSink m_frameSink;
	std::vector<uint32> m_frameBuffer;


	bool m_interlaced = false;
	uint32 m_frameSkip = 0;
	uint32 m_frameCounter = 0;
	uint64 m_frameRasterMicros = 0;
	std::atomic<uint32> m_lastFrameRasterMicros{0};
};
