#pragma once

// VU1 microprograms by start address (EmuProfile::g_vu1Programs), most expensive first.

#include <algorithm>
#include <cstdio>
#include <vector>
#include "EmuProfile.h"

inline void WriteVu1ProgramStats(std::FILE* file)
{
	const auto& stats = EmuProfile::g_vu1Programs;
	std::vector<unsigned int> order;
	for(unsigned int i = 0; i < 2048; i++)
		if(stats.starts[i] != 0) order.push_back(i);
	std::sort(order.begin(), order.end(), [&](unsigned int a, unsigned int b) { return stats.timeUs[a] > stats.timeUs[b]; });
	std::fprintf(file, "address starts cycles cycles_per_start unfinished time_ms us_per_start\n");
	for(auto i : order)
	{
		std::fprintf(file, "0x%04X %u %llu %llu %u %.1f %.1f\n", i * 8, stats.starts[i], static_cast<unsigned long long>(stats.cycles[i]),
		             static_cast<unsigned long long>(stats.cycles[i] / stats.starts[i]), stats.unfinished[i], stats.timeUs[i] / 1000.0,
		             static_cast<double>(stats.timeUs[i]) / stats.starts[i]);
	}
}
