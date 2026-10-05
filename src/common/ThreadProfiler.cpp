#include "ThreadProfiler.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>

#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#else
#include <pthread.h>
#include <time.h>
#endif

namespace
{
	struct THREAD
	{
#if defined(__vita__)
		int threadId = -1;
#else
		clockid_t clock = 0;
#endif
		uint64_t lastCpuMicros = 0;
	};

	std::mutex g_mutex;
	std::map<std::string, THREAD> g_threads;
	uint64_t g_lastWallMicros = 0;

	uint64_t WallMicros()
	{
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
	}

	bool CpuMicros(const THREAD& thread, uint64_t& micros)
	{
#if defined(__vita__)
		SceKernelThreadInfo info = {};
		info.size = sizeof(info);
		if(sceKernelGetThreadInfo(thread.threadId, &info) < 0) return false;
		micros = info.runClocks; //SceKernelSysClock: microseconds
		return true;
#else
		timespec ts;
		if(clock_gettime(thread.clock, &ts) != 0) return false;
		micros = static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
		return true;
#endif
	}
}

void ThreadProfiler::RegisterCurrentThread(const char* name)
{
	THREAD thread;
#if defined(__vita__)
	thread.threadId = sceKernelGetThreadId();
#else
	if(pthread_getcpuclockid(pthread_self(), &thread.clock) != 0) return;
#endif
	CpuMicros(thread, thread.lastCpuMicros);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_threads[name] = thread;
}

void ThreadProfiler::Unregister(const char* name)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_threads.erase(name);
}

std::vector<ThreadProfiler::SAMPLE> ThreadProfiler::Sample()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	uint64_t now = WallMicros();
	uint64_t elapsed = (g_lastWallMicros != 0) ? (now - g_lastWallMicros) : 0;
	g_lastWallMicros = now;

	std::vector<SAMPLE> samples;
	for(auto it = g_threads.begin(); it != g_threads.end();)
	{
		uint64_t cpu = 0;
		if(!CpuMicros(it->second, cpu))
		{
			it = g_threads.erase(it); //thread is gone
			continue;
		}
		SAMPLE sample;
		sample.name = it->first;
		if(elapsed != 0) sample.cpuPercent = 100.0f * static_cast<float>(cpu - it->second.lastCpuMicros) / static_cast<float>(elapsed);
		it->second.lastCpuMicros = cpu;
		samples.push_back(sample);
		++it;
	}
	return samples;
}
