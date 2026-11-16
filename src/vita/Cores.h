#pragma once

// The Vita's cores for this app's threads (docs/PERF_WORK_NOT_DONE.md, P3).
// Game-mode apps get cores 0-2. Core 3 (the system core) only with a plugin such as
// CapUnlocker; without one the affinity change is refused. Detect() tries it once, at start,
// and Mask() turns a core number from the settings into an affinity mask, moving core 3 to
// core 2 when it is not available, so "3" is a safe default for light threads.
#include <psp2/kernel/threadmgr.h>
#include <cstdint>
#include <cstdio>

namespace VitaCores
{
	inline bool g_core3 = false;

	// Call once, early, on the main thread (it changes and restores that thread's affinity).
	inline bool Detect()
	{
		SceUID self = sceKernelGetThreadId();
		int previous = sceKernelGetThreadCpuAffinityMask(self);
		int result = sceKernelChangeThreadCpuAffinityMask(self, 0x80000 /* core 3 */);
		g_core3 = (result >= 0) && (sceKernelGetThreadCpuAffinityMask(self) == 0x80000);
		sceKernelChangeThreadCpuAffinityMask(self, (previous > 0) ? previous : 0);
		std::printf("core 3: %s (affinity change result %08X)\n", g_core3 ? "available" : "not available, light threads on core 2", static_cast<unsigned int>(result));
		return g_core3;
	}

	inline uint32_t Mask(uint32_t core)
	{
		if(core > 3) core = 3;
		if((core == 3) && !g_core3) core = 2;
		return 0x10000u << core;
	}
}
