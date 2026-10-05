#pragma once

#include <functional>
#include <memory>
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

// Something that accepts GS register writes on top of a GS memory image.
class IGsBenchmarkTarget
{
public:
	virtual ~IGsBenchmarkTarget() = default;
	virtual void Write(uint8 reg, uint64 value) = 0;
	virtual uint8* Ram() = 0;
	// Waits until everything written so far has been drawn.
	virtual void Sync() = 0;
};

using GS_BENCHMARK_TARGET_FACTORY = std::function<std::unique_ptr<IGsBenchmarkTarget>()>;

// Same workloads, on targets made by the factory (a fresh one per workload).
std::vector<GS_BENCHMARK_RESULT> RunGsBenchmarkOn(double secondsPerWorkload, const GS_BENCHMARK_TARGET_FACTORY& factory,
                                                  const std::function<void(const GS_BENCHMARK_RESULT&)>& onResult = {});
