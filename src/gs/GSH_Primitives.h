#pragma once

#include "gs/GSHandler.h"
#include "SoftwareRasterizer.h"
#include "Re4Vu1Batch.h"

// Common front end of the VitaPS2 GS renderers: tracks GS register writes,
// assembles primitives from vertex kicks (strips, fans, sprites...) and
// decodes the drawing state. Renderers implement OnStateChanged() and OnDraw().
class CGSH_Primitives : public CGSHandler
{
public:
	explicit CGSH_Primitives(bool gsThreaded)
	    : CGSHandler(gsThreaded)
	{
		m_primitiveMode <<= 0;
	}

	uint32 GetPrimitiveCount() const
	{
		return m_primitiveCount;
	}

	// Speed hacks; renderers ignore the ones they do not support. Call on the
	// GS thread (SendGSCall).
	virtual void SetFrameSkip(uint32)
	{
	}
	virtual void SetInterlacedRendering(bool)
	{
	}

	// GS thread: a batch of Resident Evil 4's world vertex program, done here instead of on
	// VU1 (Re4Vu1Hle). Same registers, kicks and draws as the program's GIF packet, but
	// triangles crossing the clip volume are clipped here (the program's clipping routine).
	virtual void DrawWorldBatch(const Re4Vu1::WORLD_BATCH&);
	static inline uint64 g_worldBatches = 0, g_worldVertices = 0, g_worldClipped = 0;
	static inline uint64 g_worldUs = 0;           //time in DrawWorldBatch (with g_clockUs)
	static inline uint64 (*g_clockUs)() = nullptr; //a cheap clock, set by the renderer

protected:
	void WriteRegisterImpl(uint8, uint64) override;

	// Called before a drawing kick when any state register changed.
	virtual void OnStateChanged() = 0;
	// Vertices are in drawing order (oldest first).
	virtual void OnDraw(CSoftwareRasterizer::PRIMITIVE_KIND, const CSoftwareRasterizer::VERTEX*) = 0;

	// Drawing state for the current primitive, decoded from GS registers.
	CSoftwareRasterizer::STATE DecodeState() const;

	void ResetPrimitiveState()
	{
		m_vtxCount = 0;
		m_primitiveType = PRIM_INVALID;
		m_pendingPrim = false;
		m_pendingPrimValue = 0;
		m_primitiveCount = 0;
		m_stateDirty = true;
	}

	void MarkStateDirty()
	{
		m_stateDirty = true;
	}

	PRMODE m_primitiveMode;
	bool m_skipDrawing = false; //frame skip

protected:
	// For batches drawn outside the vertex queue: starts the primitive the way a kick does
	// (pending PRIM, PRMODE, OnStateChanged when the state changed). True when drawing is
	// enabled and the primitive is a triangle strip.
	bool BeginBatchStrip();
	// After a batch: the vertex queue and registers as the batch's last two kicks leave them.
	void EndBatchStrip(const Re4Vu1::GS_VERTEX* secondLast, const Re4Vu1::GS_VERTEX& last);
	uint32 m_batchPrimitives = 0; //triangles a batch drew outside the queue (statistics)

	// A triangle straight to OnDraw (state brought up to date first), outside the vertex queue.
	void DrawDirectTriangle(const CSoftwareRasterizer::VERTEX*);
	CSoftwareRasterizer::VERTEX ConvertBatchVertex(const Re4Vu1::GS_VERTEX&) const;
	// The program's clipping routine for the triangle ending at vertex i (clipped to the clip
	// volume, culled, drawn as a fan through DrawDirectTriangle).
	void DrawClippedWorldTriangle(const Re4Vu1::WORLD_VERTEX* last3);

private:
	void VertexKick(uint8, uint64);
	void BeginPrimitive(uint64);
	CSoftwareRasterizer::VERTEX ConvertVertex(const VERTEX&) const;

	VERTEX m_vtxBuffer[3] = {};
	uint32 m_vtxCount = 0;
	uint32 m_primitiveType = PRIM_INVALID;
	bool m_pendingPrim = false;
	uint64 m_pendingPrimValue = 0;
	uint32 m_primitiveCount = 0;
	bool m_stateDirty = true;
	uint64 m_lastPrimitiveMode = ~0ULL;
};
