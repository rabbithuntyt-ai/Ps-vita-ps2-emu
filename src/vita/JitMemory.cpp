#include "JitMemory.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <mutex>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include "JitPool.h"

// Code is written through the VM domain (sceKernelOpenVMDomain makes the
// RX block writable for the calling process), then the written range is made
// coherent between the data and instruction caches with sceKernelSyncVMDomain.

namespace
{
	CJitPool g_pool;
	SceUID g_block = -1;
	std::recursive_mutex g_writeMutex;
	int g_writeDepth = 0;
	std::string g_diagnostics;
	VITAJIT_STATS g_stats;
	SceUInt64 g_writeStart = 0;
	// Free memory kept for what the VU1 thread and the current EE slice compile
	// before the next safe point; the EE's code is dropped when less is left.
	constexpr size_t FLUSH_RESERVE = 1536 * 1024;
	constexpr SceUInt64 MIN_FLUSH_INTERVAL_US = 5000000;
	size_t g_softLimit = 0; //0 = the whole pool
	SceUInt64 g_lastFlush = 0;
	unsigned int g_flushes = 0;
}

void VitaJit_SetSoftLimit(size_t bytes)
{
	g_softLimit = bytes;
}

unsigned int VitaJit_GetFlushCount()
{
	return g_flushes;
}

extern "C" int vitaps2_jit_should_flush()
{
	size_t limit = g_pool.GetCapacity();
	if((g_softLimit != 0) && (g_softLimit < limit)) limit = g_softLimit;
	if(g_pool.GetUsedBytes() + FLUSH_RESERVE < limit) return 0;
	// What is left after a flush may be VU and IOP code that cannot be dropped:
	// never spend all the time flushing.
	SceUInt64 now = sceKernelGetProcessTimeWide();
	if((g_flushes != 0) && (now - g_lastFlush < MIN_FLUSH_INTERVAL_US)) return 0;
	g_lastFlush = now;
	g_flushes++;
	std::printf("JIT memory: %u/%u KB used, dropping EE code (flush %u)\n", static_cast<unsigned int>(g_pool.GetUsedBytes() / 1024),
	            static_cast<unsigned int>(limit / 1024), g_flushes);
	return 1;
}

VITAJIT_STATS VitaJit_GetStats()
{
	VITAJIT_STATS stats = g_stats;
	stats.freeRanges = static_cast<unsigned int>(g_pool.GetFreeRangeCount());
	return stats;
}

std::string VitaJit_GetDiagnostics()
{
	return g_diagnostics;
}

bool VitaJit_Init(size_t minSize, size_t maxSize)
{
	if(g_block >= 0) return true;
	const size_t MB = 1024 * 1024;
	minSize = std::max<size_t>((minSize + MB - 1) & ~(MB - 1), MB);
	maxSize = std::max<size_t>((maxSize + MB - 1) & ~(MB - 1), minSize);

	SceKernelFreeMemorySizeInfo freeInfo = {};
	freeInfo.size = sizeof(freeInfo);
	sceKernelGetFreeMemorySize(&freeInfo);
	char buffer[256];
	std::snprintf(buffer, sizeof(buffer), "Free memory: user %u KB, cdram %u KB, phycont %u KB\n",
	              freeInfo.size_user / 1024, freeInfo.size_cdram / 1024, freeInfo.size_phycont / 1024);
	g_diagnostics = buffer;
	std::printf("%s", buffer);

	// Try big first, then step down: the largest block that fits wins.
	size_t poolSize = maxSize;
	int lastError = 0;
	while(true)
	{
		g_block = sceKernelAllocMemBlockForVM("VitaPS2_JIT", poolSize);
		if(g_block >= 0) break;
		lastError = g_block;
		std::printf("sceKernelAllocMemBlockForVM(%u MB) failed: 0x%08X\n", static_cast<unsigned int>(poolSize / MB), static_cast<unsigned int>(lastError));
		if(poolSize <= minSize)
		{
			std::snprintf(buffer, sizeof(buffer), "sceKernelAllocMemBlockForVM failed for %u..%u MB, last error 0x%08X\n",
			              static_cast<unsigned int>(minSize / MB), static_cast<unsigned int>(maxSize / MB), static_cast<unsigned int>(lastError));
			g_diagnostics += buffer;
			return false;
		}
		poolSize = std::max(minSize, poolSize - 4 * MB);
	}
	std::printf("JIT pool: %u MB\n", static_cast<unsigned int>(poolSize / MB));
	void* base = nullptr;
	if(sceKernelGetMemBlockBase(g_block, &base) < 0)
	{
		return false;
	}
	g_pool.Init(base, poolSize);
	return true;
}

size_t VitaJit_GetUsedBytes()
{
	return g_pool.GetUsedBytes();
}

size_t VitaJit_GetCapacity()
{
	return g_pool.GetCapacity();
}

extern "C" void* vitaps2_jit_alloc(size_t size)
{
	SceUInt64 start = sceKernelGetProcessTimeWide();
	void* result = g_pool.Alloc(size);
	g_stats.allocUs += sceKernelGetProcessTimeWide() - start;
	g_stats.allocs++;
	if(!result)
	{
		std::printf("JIT pool exhausted (%u bytes requested, %u/%u used)\n",
		            static_cast<unsigned int>(size), static_cast<unsigned int>(g_pool.GetUsedBytes()),
		            static_cast<unsigned int>(g_pool.GetCapacity()));
		// stdout goes nowhere on the Vita.
		if(FILE* file = std::fopen("ux0:data/VitaPS2/crash.txt", "w"))
		{
			std::fprintf(file, "JIT pool exhausted: %u bytes requested, %u/%u used, %u flushes\n", static_cast<unsigned int>(size),
			             static_cast<unsigned int>(g_pool.GetUsedBytes()), static_cast<unsigned int>(g_pool.GetCapacity()), g_flushes);
			std::fclose(file);
		}
		std::abort();
	}
	return result;
}

extern "C" void vitaps2_jit_free(void* ptr)
{
	g_pool.Free(ptr);
}

extern "C" void vitaps2_jit_begin_write()
{
	g_writeMutex.lock();
	if(g_writeDepth++ == 0)
	{
		g_writeStart = sceKernelGetProcessTimeWide();
		sceKernelOpenVMDomain();
	}
}

extern "C" void vitaps2_jit_end_write(void* ptr, size_t size)
{
	if(--g_writeDepth == 0)
	{
		sceKernelCloseVMDomain();
	}
	if(ptr && size)
	{
		sceKernelSyncVMDomain(g_block, ptr, size);
	}
	if(g_writeDepth == 0)
	{
		g_stats.writes++;
		g_stats.writeUs += sceKernelGetProcessTimeWide() - g_writeStart;
	}
	g_writeMutex.unlock();
}
