#include "JitMemory.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <cstdlib>
#include <mutex>
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
	void* result = g_pool.Alloc(size);
	if(!result)
	{
		std::printf("JIT pool exhausted (%u bytes requested, %u/%u used)\n",
		            static_cast<unsigned int>(size), static_cast<unsigned int>(g_pool.GetUsedBytes()),
		            static_cast<unsigned int>(g_pool.GetCapacity()));
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
	g_writeMutex.unlock();
}
