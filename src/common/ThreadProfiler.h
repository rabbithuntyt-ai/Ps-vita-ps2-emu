#pragma once

// Per-thread CPU usage, for the performance overlay. Threads register
// themselves by name; Sample() returns each thread's CPU usage (percent of one
// core) since the previous Sample() call.
//
// Vita: kernel thread run clocks. Host: POSIX per-thread CPU clocks.

#include <string>
#include <vector>

namespace ThreadProfiler
{
	// Call on the thread to be measured. Re-registering a name replaces it.
	void RegisterCurrentThread(const char* name);
	void Unregister(const char* name);

	struct SAMPLE
	{
		std::string name;
		float cpuPercent = 0;
	};
	std::vector<SAMPLE> Sample();
}
