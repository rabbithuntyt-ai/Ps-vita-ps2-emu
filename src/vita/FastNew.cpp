// Replacement for the global operator new/delete on the Vita.
//
// newlib's malloc takes a kernel lock on every call (about 1.3us measured on
// the hardware). Play!'s recompiler allocates constantly while compiling a
// block (statement lists, symbol maps, shared pointers): that alone made
// compiling a block take milliseconds. Small allocations are served from free
// lists by size class instead, behind a spinlock (one atomic exchange when
// uncontended).
//
// Small blocks live in one arena with a region per size class, so delete
// finds the class from the address and needs no header: pointers outside the
// arena go to free(), which keeps code that frees malloc memory with delete
// working. No thread_local: operator new runs during static initialization,
// before the Vita's emulated TLS works. Regions never shrink; a full region
// falls back to malloc.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <psp2/kernel/threadmgr.h>

namespace
{
	constexpr size_t GRANULE = 8;
	constexpr size_t SMALL_MAX = 256;
	constexpr size_t CLASS_COUNT = SMALL_MAX / GRANULE;
	constexpr size_t REGION_SIZE = 512 * 1024;
	constexpr size_t ARENA_SIZE = REGION_SIZE * CLASS_COUNT; //16MB

	struct FREE_BLOCK
	{
		FREE_BLOCK* next;
	};

	//Zero-initialized (constant initialization): usable before any constructor runs.
	uint8_t* g_arena;
	bool g_arenaFailed;
	FREE_BLOCK* g_free[CLASS_COUNT];
	size_t g_used[CLASS_COUNT]; //bytes handed out from each region
	std::atomic_flag g_lock = ATOMIC_FLAG_INIT;

	class CSpinLock
	{
	public:
		CSpinLock()
		{
			//Threads of different priorities share cores: a spinning thread must
			//let a lower priority lock holder run.
			for(unsigned int spins = 0; g_lock.test_and_set(std::memory_order_acquire); spins++)
			{
				if(spins >= 64) sceKernelDelayThread(1);
			}
		}
		~CSpinLock()
		{
			g_lock.clear(std::memory_order_release);
		}
	};

	inline void* AllocateLarge(size_t size)
	{
		void* result = std::malloc(size);
		if(!result) throw std::bad_alloc();
		return result;
	}

	inline void* Allocate(size_t size)
	{
		if(size == 0) size = 1;
		if(size > SMALL_MAX) return AllocateLarge(size);
		uint32_t sizeClass = static_cast<uint32_t>((size - 1) / GRANULE);
		size_t blockSize = (sizeClass + 1) * GRANULE;
		{
			CSpinLock lock;
			if(!g_arena && !g_arenaFailed)
			{
				g_arena = static_cast<uint8_t*>(std::malloc(ARENA_SIZE));
				g_arenaFailed = (g_arena == nullptr);
			}
			if(g_arena)
			{
				if(FREE_BLOCK* block = g_free[sizeClass])
				{
					g_free[sizeClass] = block->next;
					return block;
				}
				if(g_used[sizeClass] + blockSize <= REGION_SIZE)
				{
					void* result = g_arena + sizeClass * REGION_SIZE + g_used[sizeClass];
					g_used[sizeClass] += blockSize;
					return result;
				}
			}
		}
		return AllocateLarge(size);
	}

	inline void Free(void* ptr)
	{
		if(!ptr) return;
		auto address = static_cast<uint8_t*>(ptr);
		if(!g_arena || (address < g_arena) || (address >= g_arena + ARENA_SIZE))
		{
			std::free(ptr);
			return;
		}
		size_t sizeClass = (address - g_arena) / REGION_SIZE;
		auto block = static_cast<FREE_BLOCK*>(ptr);
		CSpinLock lock;
		block->next = g_free[sizeClass];
		g_free[sizeClass] = block;
	}
}

void* operator new(size_t size)
{
	return Allocate(size);
}

void* operator new[](size_t size)
{
	return Allocate(size);
}

void* operator new(size_t size, const std::nothrow_t&) noexcept
{
	try
	{
		return Allocate(size);
	}
	catch(...)
	{
		return nullptr;
	}
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept
{
	try
	{
		return Allocate(size);
	}
	catch(...)
	{
		return nullptr;
	}
}

void operator delete(void* ptr) noexcept
{
	Free(ptr);
}

void operator delete[](void* ptr) noexcept
{
	Free(ptr);
}

void operator delete(void* ptr, size_t) noexcept
{
	Free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept
{
	Free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept
{
	Free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept
{
	Free(ptr);
}
