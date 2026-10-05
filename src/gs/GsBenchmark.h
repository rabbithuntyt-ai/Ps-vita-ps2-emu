#pragma once

#include <functional>
#include <string>
#include <vector>
#include "Types.h"

struct GS_BENCHMARK_RESULT
{
	std::string name;
	double mpixelsPerSecond = 0;
	double framesPerSecond = 0; //full 640x448 frames worth of pixels
};

// Runs every workload for the given time with the given number of rasterizer
// threads. onResult is called after each workload.
std::vector<GS_BENCHMARK_RESULT> RunGsBenchmark(double secondsPerWorkload, uint32 threads,
                                                const std::function<void(const GS_BENCHMARK_RESULT&)>& onResult = {});
