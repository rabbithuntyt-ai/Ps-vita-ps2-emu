#pragma once

#include <functional>
#include <vector>
#include "gs/GSHandler.h"
#include "SoftwareRasterizer.h"

// GS handler backed by CSoftwareRasterizer. All drawing happens in the
// emulated GS local memory; on each flip the displayed buffer is converted to
// RGBA8888 and handed to the frontend through the frame sink.
class CGSH_Software : public CGSHandler
{
public:
	// Called on the GS thread with a tightly packed RGBA8888 (R in the low byte)
	// image of the currently displayed buffer.
	using FrameSink = std::function<void(const uint32* pixels, uint32 width, uint32 height)>;

	explicit CGSH_Software(bool gsThreaded = true);
	~CGSH_Software() override = default;

	static FactoryFunction GetFactoryFunction(FrameSink sink, bool gsThreaded = true);

	void SetFrameSink(FrameSink);

	void ProcessHostToLocalTransfer() override;
	void ProcessLocalToHostTransfer() override;
	void ProcessLocalToLocalTransfer() override;
	void ProcessClutTransfer(uint32, uint32) override;

	uint32 GetPrimitiveCount() const
	{
		return m_primitiveCount;
	}

	// Speed hacks -------------------------------------------------------------
	// Interlaced rendering: rasterize only every other row, alternating each
	// frame. Halves fill cost; can leave artifacts in render-to-texture effects.
	void SetInterlacedRendering(bool);
	// Skip drawing of N frames out of N+1 (0 = draw everything).
	void SetFrameSkip(uint32);

protected:
	void InitializeImpl() override;
	void ReleaseImpl() override;
	void ResetImpl() override;
	void WriteRegisterImpl(uint8, uint64) override;
	void FlipImpl(const DISPLAY_INFO&) override;
	void MarkNewFrame() override;
	void TransferWrite(const uint8*, uint32) override;
	void SyncCLUT(const TEX0&) override;

private:
	void VertexKick(uint8, uint64);
	void BeginPrimitive(uint64);
	void BuildState();
	void UpdateRowFilter();
	CSoftwareRasterizer::VERTEX ConvertVertex(const VERTEX&) const;

	CSoftwareRasterizer m_rasterizer;
	FrameSink m_frameSink;
	std::vector<uint32> m_frameBuffer;

	VERTEX m_vtxBuffer[3] = {};
	uint32 m_vtxCount = 0;
	uint32 m_primitiveType = PRIM_INVALID;
	PRMODE m_primitiveMode;
	bool m_pendingPrim = false;
	uint64 m_pendingPrimValue = 0;
	uint32 m_primitiveCount = 0;
	bool m_stateDirty = true;
	uint64 m_lastPrimitiveMode = ~0ULL;

	bool m_interlaced = false;
	uint32 m_frameSkip = 0;
	uint32 m_frameCounter = 0;
	bool m_skipThisFrame = false;
};
