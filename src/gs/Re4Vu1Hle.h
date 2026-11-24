#pragma once

// Resident Evil 4's world vertex program done without VU1 (re4_vu1_hle): when the program
// starts, its batch is taken from VU1 memory (Re4Vu1Batch.h) and drawn on the GS thread
// (CGSH_Primitives::DrawWorldBatch), in order with the other GIF packets. The program is
// recognised by its code, so other games (and other programs) are untouched. Needs a GS
// handler derived from CGSH_Primitives (both VitaPS2 renderers are).

namespace Re4Vu1Hle
{
	// Installs (or removes) CVpu's program replacement hooks.
	void Enable(bool);
	bool IsEnabled();
	// W2: hash each batch's data where it is built (the retained world path's key)
	inline bool g_hashBatches = false;
	// A run of consecutive batches in the kick queue goes to the GS thread as one call (on;
	// group_submit, PC --no-group-submit): one call per batch was ~2,100 a game frame
	inline bool g_groupSubmit = true;
}
