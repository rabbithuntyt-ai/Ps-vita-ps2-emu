// Host front end for the software GS benchmark.
//
//   gs_benchmark [seconds-per-workload] [rasterizer-threads]

#include <cstdio>
#include <cstdlib>
#include "EmuSession.h"
#include "GsBenchmark.h"

int main(int argc, char** argv)
{
	double secondsPerWorkload = (argc > 1) ? std::atof(argv[1]) : 1.0;
	uint32 threads = (argc > 2) ? std::atoi(argv[2]) : 1;
	CEmuSession::SetDataPaths("vitaps2_test_data", ".");
	std::printf("rasterizer threads: %u\n", threads);

	double total = 0;
	auto results = RunGsBenchmark(secondsPerWorkload, threads, [](const GS_BENCHMARK_RESULT& result) {
		std::printf("%-48s %8.1f Mpix/s  (%6.1f full frames/s)\n", result.name.c_str(), result.mpixelsPerSecond, result.framesPerSecond);
	});
	for(const auto& result : results) total += result.mpixelsPerSecond;
	std::printf("%-48s %8.1f Mpix/s\n", "average", total / results.size());
	return 0;
}
