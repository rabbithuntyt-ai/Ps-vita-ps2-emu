#pragma once

// The GS benchmark workloads on the GPU renderer. Needs a current GL context;
// leaves GL state changed (callers redraw their UI from scratch).

#include "GsBenchmark.h"

std::vector<GS_BENCHMARK_RESULT> RunGsHardwareBenchmark(double secondsPerWorkload,
                                                        const std::function<void(const GS_BENCHMARK_RESULT&)>& onResult = {});
