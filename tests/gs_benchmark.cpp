// Host front end for the GS benchmark.
//
//   gs_benchmark [seconds-per-workload] [rasterizer-threads]
//   gs_benchmark --gpu [seconds-per-workload]   (GPU renderer on OSMesa)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "EmuSession.h"
#include "GsBenchmark.h"
#ifdef VITAPS2_HAS_GL
#include <GL/osmesa.h>
#include "GsHardwareBenchmark.h"
#endif

int main(int argc, char** argv)
{
	bool gpu = (argc > 1) && (std::strcmp(argv[1], "--gpu") == 0);
	if(gpu)
	{
		argc--;
		argv++;
	}
	double secondsPerWorkload = (argc > 1) ? std::atof(argv[1]) : 1.0;
	uint32 threads = (argc > 2) ? std::atoi(argv[2]) : 1;
	CEmuSession::SetDataPaths("vitaps2_test_data", ".");

	auto print = [](const GS_BENCHMARK_RESULT& result) {
		std::printf("%-48s %8.1f Mpix/s  (%6.1f full frames/s)\n", result.name.c_str(), result.mpixelsPerSecond, result.framesPerSecond);
	};
	std::vector<GS_BENCHMARK_RESULT> results;
	if(gpu)
	{
#ifdef VITAPS2_HAS_GL
		const int attribs[] = {OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_PROFILE, OSMESA_COMPAT_PROFILE, 0};
		OSMesaContext context = OSMesaCreateContextAttribs(attribs, nullptr);
		std::vector<uint32> windowBuffer(16 * 16);
		if(!context || !OSMesaMakeCurrent(context, windowBuffer.data(), GL_UNSIGNED_BYTE, 16, 16))
		{
			std::printf("could not create an OSMesa context\n");
			return 1;
		}
		std::printf("GPU renderer (OSMesa)\n");
		results = RunGsHardwareBenchmark(secondsPerWorkload, print);
		OSMesaDestroyContext(context);
#else
		std::printf("built without GL\n");
		return 1;
#endif
	}
	else
	{
		std::printf("rasterizer threads: %u\n", threads);
		results = RunGsBenchmark(secondsPerWorkload, threads, print);
	}
	double total = 0;
	for(const auto& result : results) total += result.mpixelsPerSecond;
	std::printf("%-48s %8.1f Mpix/s\n", "average", total / results.size());
	return 0;
}
