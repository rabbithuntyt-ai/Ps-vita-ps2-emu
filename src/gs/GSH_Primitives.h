#pragma once

#include "gs/GSHandler.h"
#include "SoftwareRasterizer.h"

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
