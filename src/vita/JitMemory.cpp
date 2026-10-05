#include "JitMemory.h"

#include <cstdio>
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
}

bool VitaJit_Init(size_t poolSize)
{
	if(g_block >= 0) return true;
	poolSize = (poolSize + 0xFFFFF) & ~static_cast<size_t>(0xFFFFF); //1MB granularity
	g_block = sceKernelAllocMemBlockForVM("VitaPS2_JIT", poolSize);
	if(g_block < 0)
	{
		std::printf("sceKernelAllocMemBlockForVM failed: 0x%08X\n", static_cast<unsigned int>(g_block));
		return false;
	}
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
