#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "JitPool.h"

static int g_failures = 0;
#define CHECK(cond)                                                       \
	do                                                                    \
	{                                                                     \
		if(!(cond))                                                       \
		{                                                                 \
			g_failures++;                                                 \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                 \
	} while(0)

int main()
{
	alignas(16) static uint8_t backing[64 * 1024];
	CJitPool pool;
	pool.Init(backing, sizeof(backing));

	void* a = pool.Alloc(100);
	void* b = pool.Alloc(17);
	CHECK(a == backing);
	CHECK(reinterpret_cast<uintptr_t>(b) % CJitPool::ALIGNMENT == 0);
	CHECK(reinterpret_cast<uint8_t*>(b) == backing + 112);
	CHECK(pool.GetUsedBytes() == 112 + 32);

	pool.Free(a);
	void* c = pool.Alloc(64); //reuses the hole left by a
	CHECK(c == backing);
	pool.Free(c);
	pool.Free(b);
	CHECK(pool.GetUsedBytes() == 0);
	CHECK(pool.GetFreeRangeCount() == 1); //fully coalesced

	CHECK(pool.Alloc(sizeof(backing) + 1) == nullptr);
	void* all = pool.Alloc(sizeof(backing));
	CHECK(all == backing);
	CHECK(pool.Alloc(16) == nullptr);
	pool.Free(all);

	//Random churn must always coalesce back to a single range.
	std::mt19937 rng(1234);
	std::vector<void*> live;
	for(int i = 0; i < 20000; i++)
	{
		if(live.empty() || (rng() % 3 != 0))
		{
			void* p = pool.Alloc(1 + rng() % 700);
			if(p) live.push_back(p);
		}
		else
		{
			size_t index = rng() % live.size();
			pool.Free(live[index]);
			live[index] = live.back();
			live.pop_back();
		}
	}
	for(void* p : live) pool.Free(p);
	CHECK(pool.GetUsedBytes() == 0);
	CHECK(pool.GetFreeRangeCount() == 1);

	std::printf("jit pool tests: %s\n", g_failures ? "FAILED" : "passed");
	return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
