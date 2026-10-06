// Headless host runner: boots a PS2 executable or disc image with the same
// emulation core and software GS renderer used on the Vita, runs for a number
// of emulated frames and writes the last displayed frame to a PPM file.
//
//   vitaps2_host <game.elf|game.iso> [--frames N] [--timeout SECONDS] [--out frame.ppm]
//                [--gs-threads N] [--interlaced] [--frameskip N] [--hw] [--mtvu]
//
// --hw renders with the GPU renderer on an offscreen Mesa context, pumped
// from the main thread exactly like the Vita frontend does.
//
// Exit code is 0 when at least one frame was presented.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "EmuSession.h"
#include "EmuProfile.h"
#include "ThreadProfiler.h"
#include "PS2VM.h"
#ifdef VITAPS2_HAS_GL
#include <GL/osmesa.h>
#include "GSH_Hardware.h"
#endif

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
	bool hardware = false;
	bool threadedVu1 = false;
	bool threadedSpu = false;
	for(int i = 2; i < argc; i++)
	{
		if(!std::strcmp(argv[i], "--frames") && (i + 1 < argc)) targetFrames = std::strtoull(argv[++i], nullptr, 10);
		else if(!std::strcmp(argv[i], "--timeout") && (i + 1 < argc)) timeoutSeconds = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--out") && (i + 1 < argc)) outPath = argv[++i];
		else if(!std::strcmp(argv[i], "--gs-threads") && (i + 1 < argc)) gsThreads = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--interlaced")) interlaced = true;
		else if(!std::strcmp(argv[i], "--frameskip") && (i + 1 < argc)) frameSkip = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--hw")) hardware = true;
		else if(!std::strcmp(argv[i], "--mtvu")) threadedVu1 = true;
		else if(!std::strcmp(argv[i], "--mtspu")) threadedSpu = true;
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

	// The GPU renderer runs on this thread.
	CEmuSession* sessionPtr = nullptr;
	std::function<void(uint32_t)> pump = [](uint32_t timeoutMs) { std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs)); };
#ifdef VITAPS2_HAS_GL
	OSMesaContext glContext = nullptr;
	std::vector<uint32_t> glWindow(16 * 16);
	if(hardware)
	{
		const int attribs[] = {OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_PROFILE, OSMESA_COMPAT_PROFILE, 0};
		glContext = OSMesaCreateContextAttribs(attribs, nullptr);
		if(!glContext || !OSMesaMakeCurrent(glContext, glWindow.data(), GL_UNSIGNED_BYTE, 16, 16))
		{
			std::fprintf(stderr, "could not create an OSMesa context\n");
			return 1;
		}
		CGSH_Hardware::OPTIONS options;
		options.readbackFrames = true;
		options.frameSink = [&sessionPtr](std::vector<uint32>& pixels, uint32 width, uint32 height) {
			if(sessionPtr) sessionPtr->GetFrames().Publish(pixels, width, height);
		};
		config.gsFactory = CGSH_Hardware::GetFactoryFunction(options);
		auto hwGs = [&sessionPtr]() -> CGSH_Hardware* {
			return sessionPtr ? static_cast<CGSH_Hardware*>(sessionPtr->GetVm()->GetGSHandler()) : nullptr;
		};
		pump = [hwGs](uint32_t timeoutMs) {
			if(auto gs = hwGs()) gs->Pump(timeoutMs);
			else std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
		};
		config.gsPump = [pump]() { pump(2); };
		config.gsShutdown = [hwGs]() {
			if(auto gs = hwGs()) gs->ReleaseGpu();
		};
	}
#else
	if(hardware)
	{
		std::fprintf(stderr, "built without the GPU renderer\n");
		return 2;
	}
#endif

	try
	{
		CEmuSession session(config);
		sessionPtr = &session;
		if(threadedVu1 || threadedSpu)
		{
			auto hacks = session.GetSpeedHacks();
			hacks.threadedVu1 = threadedVu1;
			hacks.threadedSpu = threadedSpu;
			session.SetSpeedHacks(hacks);
		}
		session.Boot(bootPath);

		ThreadProfiler::Sample();
		auto start = std::chrono::steady_clock::now();
		uint64_t serial = 0;
		std::vector<uint32_t> pixels;
		uint32_t width = 0, height = 0;
		uint64_t presented = 0;
		while(session.GetVmFrameCount() < targetFrames)
		{
			if(session.GetFrames().Fetch(serial, pixels, width, height)) presented++;
			if(std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSeconds)) break;
			pump(5);
		}
		auto threadUsage = ThreadProfiler::Sample();
		auto profile = session.TakeProfile();
		session.Pause();
		std::printf("emulation thread (%u samples):", profile.samples);
		for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++)
			if(profile.share[i] >= 0.005f) std::printf(" %s %.0f%%", EmuProfile::GetSectionName(i), profile.share[i] * 100.0f);
		std::printf("\n");
		auto debug = session.GetDebugState();
		std::printf("EE pc %08X ra %08X  IOP pc %08X ra %08X thread %d  INTC %X/%X  DMAC %08X  VU1 %04X pc %04X %ums\n", debug.eePc,
		            debug.eeRa, debug.iopPc, debug.iopRa, debug.iopThread, debug.intcStat, debug.intcMask, debug.dmacStat,
		            debug.vu1Start, debug.vu1Pc, debug.vu1RunMs);
		session.GetFrames().Fetch(serial, pixels, width, height);
		for(const auto& usage : threadUsage)
		{
			std::printf("cpu %-18s %5.1f%%\n", usage.name.c_str(), usage.cpuPercent);
		}

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
		sessionPtr = nullptr;
	}
	catch(const std::exception& e)
	{
		std::fprintf(stderr, "error: %s\n", e.what());
		return 1;
	}
	return 0;
}
