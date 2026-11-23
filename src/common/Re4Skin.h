#pragma once

// Resident Evil 4 (SLUS-21134): its VU1 skinning programs (0x0000, 0x0050, 0x0498, 0x07C8 and
// the loops at 0x01D0 and 0x04F8: weighted bone matrices, lighting, XGKICK) run as native code
// translated ahead of time (Re4Skin.gen.cpp, tools/vu_aot.py) instead of the VU JIT. Used only
// while micro memory holds exactly the code the translation was made from.
#include <cstdint>
#include <string>

class CVpu;

namespace Re4Skin
{
	// CVpu::g_nativeProgram: runs the program natively; false when 'address' is not one of the
	// skinning entries or micro memory holds other code (the JIT runs it).
	bool Run(CVpu& vpu, uint32_t address);

	// PC runner --skin-verify (inline VU1 only, as CVpu::g_programHook): each skinning program
	// runs in the JIT as usual, and natively from the same starting state; VU memory and the
	// registers are compared. WriteReport writes the counts and the first differences.
	void VerifyHook(CVpu& vpu, uint32_t address, bool before);
	void WriteReport(const std::string& path);

	struct STATS
	{
		uint64_t runs = 0;       // native runs (Run)
		uint64_t refused = 0;    // entry address with other code in micro memory
		uint64_t verified = 0;   // VerifyHook comparisons
		uint64_t mismatched = 0; // comparisons with a difference
	};
	extern STATS g_stats;
}
