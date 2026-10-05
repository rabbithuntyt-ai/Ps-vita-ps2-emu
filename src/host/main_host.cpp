// Headless host runner: boots a PS2 executable or disc image with the same
// emulation core and software GS renderer used on the Vita, runs for a number
// of emulated frames and writes the last displayed frame to a PPM file.
//
//   vitaps2_host <game.elf|game.iso> [--frames N] [--timeout SECONDS] [--out frame.ppm]
//                [--gs-threads N] [--interlaced] [--frameskip N]
//
// Exit code is 0 when at least one frame was presented.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "EmuSession.h"

namespace
{
	bool WritePpm(const std::string& path, const std::vector<uint32_t>& pixels, uint32_t width, uint32_t height)
	{
		FILE* file = std::fopen(path.c_str(), "wb");
		if(!file) return false;
		std::fprintf(file, "P6\n%u %u\n255\n", width, height);
		std::vector<uint8_t> row(width * 3);
		for(uint32_t y = 0; y < height; y++)
		{
			for(uint32_t x = 0; x < width; x++)
			{
				uint32_t c = pixels[x + y * width];
				row[x * 3 + 0] = c & 0xFF;
				row[x * 3 + 1] = (c >> 8) & 0xFF;
				row[x * 3 + 2] = (c >> 16) & 0xFF;
			}
			std::fwrite(row.data(), 1, row.size(), file);
		}
		std::fclose(file);
		return true;
	}
}

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		std::fprintf(stderr, "usage: %s <game.elf|game.iso> [--frames N] [--timeout SECONDS] [--out frame.ppm]\n", argv[0]);
		return 2;
	}

	std::string bootPath = argv[1];
	uint64_t targetFrames = 120;
	int timeoutSeconds = 60;
	std::string outPath = "frame.ppm";
	uint32_t gsThreads = 1;
	bool interlaced = false;
	uint32_t frameSkip = 0;
	for(int i = 2; i < argc; i++)
	{
		if(!std::strcmp(argv[i], "--frames") && (i + 1 < argc)) targetFrames = std::strtoull(argv[++i], nullptr, 10);
		else if(!std::strcmp(argv[i], "--timeout") && (i + 1 < argc)) timeoutSeconds = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--out") && (i + 1 < argc)) outPath = argv[++i];
		else if(!std::strcmp(argv[i], "--gs-threads") && (i + 1 < argc)) gsThreads = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--interlaced")) interlaced = true;
		else if(!std::strcmp(argv[i], "--frameskip") && (i + 1 < argc)) frameSkip = std::atoi(argv[++i]);
	}

	const char* home = std::getenv("HOME");
	std::string dataPath = std::string(home ? home : ".") + "/.local/share/VitaPS2";

	CEmuSession::CONFIG config;
	config.dataPath = dataPath;
	config.resourcesPath = ".";
	config.limitFrameRate = false;
	config.rasterizerThreads = gsThreads;
	config.interlacedRendering = interlaced;
	config.frameSkip = frameSkip;

	try
	{
		CEmuSession session(config);
		session.Boot(bootPath);

		auto start = std::chrono::steady_clock::now();
		uint64_t serial = 0;
		std::vector<uint32_t> pixels;
		uint32_t width = 0, height = 0;
		uint64_t presented = 0;
		while(session.GetVmFrameCount() < targetFrames)
		{
			if(session.GetFrames().Fetch(serial, pixels, width, height)) presented++;
			if(std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSeconds)) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		session.Pause();
		session.GetFrames().Fetch(serial, pixels, width, height);

		double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		uint64_t vmFrames = session.GetVmFrameCount();
		std::printf("vm frames: %llu, presented frames: %llu, %.2fs (%.1f fps)\n",
		            (unsigned long long)vmFrames, (unsigned long long)presented, seconds, vmFrames / seconds);

		if(width == 0)
		{
			std::fprintf(stderr, "no frame was presented\n");
			return 1;
		}
		WritePpm(outPath, pixels, width, height);
		std::printf("wrote %ux%u frame to %s\n", width, height, outPath.c_str());
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
