#pragma once

// The Vita's cores for this app's threads (docs/PERF_WORK_NOT_DONE.md, P3).
// Game-mode apps get cores 0-2. Core 3 (the system core) only with a plugin such as
// CapUnlocker; without one the affinity change is refused. Detect() tries it once, at start,
// and Mask() turns a core number from the settings into an affinity mask, moving core 3 to
// core 2 when it is not available, so "3" is a safe default for light threads.
#include <psp2/kernel/threadmgr.h>
#include <psp2/io/stat.h>
#include <psp2/appmgr.h>
#include <taihen.h>
#include <cstdint>
#include <cstdio>

namespace VitaCores
{
	inline bool g_core3 = false;

	inline bool TryCore3(int& result)
	{
		SceUID self = sceKernelGetThreadId();
		int previous = sceKernelGetThreadCpuAffinityMask(self);
		result = sceKernelChangeThreadCpuAffinityMask(self, 0x80000 /* core 3 */);
		bool ok = (result >= 0) && (sceKernelGetThreadCpuAffinityMask(self) == 0x80000);
		sceKernelChangeThreadCpuAffinityMask(self, (previous > 0) ? previous : 0);
		return ok;
	}

	// Call once, early, on the main thread (it changes and restores that thread's affinity).
	// When core 3 is refused, loads app0:vitaps2_core3.skprx (src/vita/core3/core3.c), which
	// lets the affinity check pass until the next reboot, and tries again. It refuses to patch
	// on firmware where the code does not look as expected (ux0:data/VitaPS2/core3.txt).
	// ux0:data/VitaPS2/no_core3.txt: do not load it.
	inline bool Detect()
	{
		int result = 0, loaded = 0;
		g_core3 = TryCore3(result);
		if(!g_core3)
		{
			SceIoStat stat;
			if(sceIoGetstat("ux0:data/VitaPS2/no_core3.txt", &stat) >= 0)
				loaded = -1;
			else
			{
				// The kernel does not see app0: (a mount of this process): the installed path
				char titleId[16] = "VTPS20000";
				sceAppMgrAppParamGetString(0, 12 /* title ID */, titleId, sizeof(titleId));
				char path[64];
				std::snprintf(path, sizeof(path), "ux0:app/%s/vitaps2_core3.skprx", titleId);
				loaded = taiLoadStartKernelModule(path, 0, nullptr, 0);
				g_core3 = TryCore3(result);
			}
		}
		char line[128];
		std::snprintf(line, sizeof(line), "core 3: %s (affinity change %08X, module %08X)\n", g_core3 ? "available" : "not available, light threads on core 2",
		              static_cast<unsigned int>(result), static_cast<unsigned int>(loaded));
		std::fputs(line, stdout);
		if(std::FILE* file = std::fopen("ux0:data/VitaPS2/core3_app.txt", "w"))
		{
			std::fputs(line, file);
			std::fclose(file);
		}
		return g_core3;
	}

	inline uint32_t Mask(uint32_t core)
	{
		if(core > 3) core = 3;
		if((core == 3) && !g_core3) core = 2;
		return 0x10000u << core;
	}
}
