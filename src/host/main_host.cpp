// Headless host runner: boots a PS2 executable or disc image with the same
// emulation core and software GS renderer used on the Vita, runs for a number
// of emulated frames and writes the last displayed frame to a PPM file.
//
//   vitaps2_host <game.elf|game.iso> [--frames N] [--timeout SECONDS] [--out frame.ppm]
//                [--gs-threads N] [--interlaced] [--frameskip N] [--hw] [--mtvu]
//                [--mtspu] [--jit-cache FILE] [--press FRAME:BUTTONS[:HOLD],...]
//                [--mpeg-hle] [--shot-every N] [--shot-dir DIR] [--stats FILE]
//                [--gs-diag LINES]
//
// --hw renders with the GPU renderer on an offscreen Mesa context, pumped
// from the main thread exactly like the Vita frontend does.
//
// Game debugging without a Vita (same core and renderer as the Vita build):
//   --press      scripted controller input by emulated frame (vblank), e.g.
//                "600:start,900:cross,960:down+cross:30" (HOLD frames, default 8);
//                buttons: up down left right start select cross circle square
//                triangle l1 l2 l3 r1 r2 r3
//   --mpeg-hle   native movie decoding (autostart key mpeg_hle=1 on the Vita),
//                log in mpeghle.txt
//   --shot-every write the displayed frame every N emulated frames
//                (DIR/shot_<frame>.ppm)
//   --stats      one CSV row per emulated second (60 frames): fps, emulation
//                thread shares, EE pc, movie pictures, and with --hw the GPU
//                renderer times and the last flipped frame's GS counters
//   --gs-diag    GPU renderer diagnostics (draws, transfers per flip) in gsdiag.txt
//
// Exit code is 0 when at least one frame was presented.

#include "ee/Tier2.h"
#include "ee/VuBasicBlock.h"
#include "ee/VUShared.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "EmuSession.h"
#include "JitCacheSetup.h"
#include "EmuProfile.h"
#include "ThreadProfiler.h"
#include "PS2VM.h"
#include "PadHandler.h"
#include "ControllerInfo.h"
#include "MpegHle.h"
#include "ee/Vif.h"
#include "Vu1Stats.h"
#include <algorithm>
#include <atomic>
#include <map>
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

	// Controller driven by a script of presses keyed by emulated frame (vblank).
	class CPH_Script : public CPadHandler
	{
	public:
		struct PRESS
		{
			uint64_t frame = 0;
			uint32_t hold = 8;
			std::vector<PS2::CControllerInfo::BUTTON> buttons;
			std::vector<std::pair<PS2::CControllerInfo::BUTTON, uint8>> axes; //lup, ldown, lleft, lright: left stick pushed fully
		};

		explicit CPH_Script(std::vector<PRESS> presses)
		    : m_presses(std::move(presses))
		{
		}

		void Update(uint8* ram) override
		{
			uint64_t frame = m_vblanks++;
			bool held[PS2::CControllerInfo::MAX_BUTTONS] = {};
			uint8 axes[PS2::CControllerInfo::MAX_BUTTONS];
			std::fill(std::begin(axes), std::end(axes), 0x7F);
			for(const auto& press : m_presses)
			{
				if((frame < press.frame) || (frame >= press.frame + press.hold)) continue;
				for(auto button : press.buttons) held[button] = true;
				for(const auto& [axis, value] : press.axes) axes[axis] = value;
			}
			for(auto* listener : m_interfaces)
			{
				for(unsigned int i = 0; i < PS2::CControllerInfo::MAX_BUTTONS; i++)
				{
					auto button = static_cast<PS2::CControllerInfo::BUTTON>(i);
					if(PS2::CControllerInfo::IsAxis(button)) listener->SetAxisState(0, button, axes[i], ram);
					else listener->SetButtonState(0, button, held[i], ram);
				}
			}
		}

	private:
		std::vector<PRESS> m_presses;
		std::atomic<uint64_t> m_vblanks{0};
	};

	// "600:start,900:cross,960:down+cross:30"
	bool ParsePresses(const std::string& text, std::vector<CPH_Script::PRESS>& presses)
	{
		using PS2::CControllerInfo;
		static const std::pair<const char*, CControllerInfo::BUTTON> names[] = {
		    {"up", CControllerInfo::DPAD_UP}, {"down", CControllerInfo::DPAD_DOWN}, {"left", CControllerInfo::DPAD_LEFT},
		    {"right", CControllerInfo::DPAD_RIGHT}, {"start", CControllerInfo::START}, {"select", CControllerInfo::SELECT},
		    {"cross", CControllerInfo::CROSS}, {"circle", CControllerInfo::CIRCLE}, {"square", CControllerInfo::SQUARE},
		    {"triangle", CControllerInfo::TRIANGLE}, {"l1", CControllerInfo::L1}, {"l2", CControllerInfo::L2},
		    {"l3", CControllerInfo::L3}, {"r1", CControllerInfo::R1}, {"r2", CControllerInfo::R2}, {"r3", CControllerInfo::R3}};
		size_t start = 0;
		while(start < text.size())
		{
			size_t end = text.find(',', start);
			if(end == std::string::npos) end = text.size();
			std::string item = text.substr(start, end - start);
			start = end + 1;
			size_t colon = item.find(':');
			if(colon == std::string::npos) return false;
			CPH_Script::PRESS press;
			press.frame = std::strtoull(item.c_str(), nullptr, 10);
			std::string rest = item.substr(colon + 1);
			size_t holdColon = rest.find(':');
			if(holdColon != std::string::npos)
			{
				press.hold = std::atoi(rest.c_str() + holdColon + 1);
				rest.resize(holdColon);
			}
			size_t nameStart = 0;
			while(nameStart <= rest.size())
			{
				size_t plus = rest.find('+', nameStart);
				if(plus == std::string::npos) plus = rest.size();
				std::string name = rest.substr(nameStart, plus - nameStart);
				nameStart = plus + 1;
				bool found = false;
				if(name == "lup") press.axes.emplace_back(CControllerInfo::ANALOG_LEFT_Y, 0x00), found = true;
				if(name == "ldown") press.axes.emplace_back(CControllerInfo::ANALOG_LEFT_Y, 0xFF), found = true;
				if(name == "lleft") press.axes.emplace_back(CControllerInfo::ANALOG_LEFT_X, 0x00), found = true;
				if(name == "lright") press.axes.emplace_back(CControllerInfo::ANALOG_LEFT_X, 0xFF), found = true;
				for(const auto& [buttonName, button] : names)
				{
					if(name != buttonName) continue;
					press.buttons.push_back(button);
					found = true;
				}
				if(!found) return false;
			}
			presses.push_back(press);
		}
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
	bool benchExit = false; //exit from the emulation thread right at the last frame (deterministic for measurements)
	std::string jitCachePath; //persistent JIT cache file (32-bit ARM only)
	std::vector<CPH_Script::PRESS> presses;
	bool mpegHleEnabled = false;
	bool vitaJit = false; // the Vita app's JIT modes (CEmuSession::SetSafeJit(false)): chained frames, indirect dispatch...
	uint64_t shotEvery = 0;
	std::string shotDir = ".";
	std::string statsPath;
	uint32_t gsDiagLines = 0;
	uint64_t hotFrom = ~0ULL; //sample the EE PC from this frame on (--hot-from)
	std::string ramDumpPath;
	std::string loadStatePath; //--load-state: a save state (the Vita's too), loaded at frame 120
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
		else if(!std::strcmp(argv[i], "--bench-exit")) benchExit = true;
		else if(!std::strcmp(argv[i], "--jit-cache") && (i + 1 < argc)) jitCachePath = argv[++i];
		else if(!std::strcmp(argv[i], "--press") && (i + 1 < argc))
		{
			if(!ParsePresses(argv[++i], presses))
			{
				std::fprintf(stderr, "bad --press script: %s\n", argv[i]);
				return 2;
			}
		}
		else if(!std::strcmp(argv[i], "--mpeg-hle")) mpegHleEnabled = true;
		else if(!std::strcmp(argv[i], "--vita-jit")) vitaJit = true;
		else if(!std::strcmp(argv[i], "--no-indirect")) CEmuSession::SetIndirectDispatch(false);
		else if(!std::strcmp(argv[i], "--no-cheap-exits")) CEmuSession::SetCheapExits(false);
		else if(!std::strcmp(argv[i], "--no-pinned")) CEmuSession::SetPinnedFields(false);
		else if(!std::strcmp(argv[i], "--no-fast-ram")) CEmuSession::SetFastRam(false);
		else if(!std::strcmp(argv[i], "--iop-thread")) CPS2VM::g_parallelIop = true; //the IOP on its own thread
		else if(!std::strcmp(argv[i], "--no-stay-in-slice")) Iop::CSubSystem::g_stayInSlice = false;
		else if(!std::strcmp(argv[i], "--no-ee-stay-in-slice")) Ee::CSubSystem::g_stayInSlice = false;
		else if(!std::strcmp(argv[i], "--regions")) CEmuSession::SetRegions(true);
		else if(!std::strcmp(argv[i], "--tier2") || !std::strcmp(argv[i], "--tier2-interpret"))
		{
			//A tier 2 region at every block start (native code on ARM, else interpreted)
			bool interpret = !std::strcmp(argv[i], "--tier2-interpret");
#if defined(__arm__) && !defined(__aarch64__)
			Tier2::g_settings.mode = interpret ? Tier2::MODE_INTERPRET : Tier2::MODE_NATIVE;
#else
			(void)interpret;
			Tier2::g_settings.mode = Tier2::MODE_INTERPRET;
#endif
			Tier2::g_settings.eager = true;
		}
		else if(!std::strcmp(argv[i], "--shot-every") && (i + 1 < argc)) shotEvery = std::strtoull(argv[++i], nullptr, 10);
		else if(!std::strcmp(argv[i], "--shot-dir") && (i + 1 < argc)) shotDir = argv[++i];
		else if(!std::strcmp(argv[i], "--stats") && (i + 1 < argc)) statsPath = argv[++i];
		else if(!std::strcmp(argv[i], "--gs-diag") && (i + 1 < argc)) gsDiagLines = std::atoi(argv[++i]);
		else if(!std::strcmp(argv[i], "--hot-from") && (i + 1 < argc)) hotFrom = std::strtoull(argv[++i], nullptr, 10);
		else if(!std::strcmp(argv[i], "--dump-ram") && (i + 1 < argc)) ramDumpPath = argv[++i];
		else if(!std::strcmp(argv[i], "--load-state") && (i + 1 < argc)) loadStatePath = argv[++i];
		else if(!std::strcmp(argv[i], "--vu1-stats")) EmuProfile::g_vu1Programs.enabled = true; //vu1prog.txt
		else if(!std::strcmp(argv[i], "--vu-old-checks"))
		{
			//VU blocks: per slot stall checks and full flag pipeline walks (A/B of the faster ones)
			CVuBasicBlock::g_combinedStallCheck = false;
			VUShared::g_flagTracking = false;
		}
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
	config.padFactory = [presses]() { return new CPH_Script(presses); };
	if(benchExit)
	{
		config.frameHook = [targetFrames](uint64_t frame, uint64_t eeExecuted) {
			if(frame < targetFrames) return;
			std::printf("EE executed %llu idle 0\n", (unsigned long long)eeExecuted);
			std::fflush(stdout);
			std::_Exit(0);
		};
	}

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

	if(vitaJit) CEmuSession::SetSafeJit(false);
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
		if(!jitCachePath.empty())
		{
			bool opened = JitCacheSetup::Open(jitCachePath, "/proc/self/exe");
			std::printf("jit cache %s: %s\n", jitCachePath.c_str(), opened ? "open" : "not available here");
		}
		session.Boot(bootPath);
		CMpegHle mpegHle;
		if(mpegHleEnabled) mpegHle.Enable(session.GetVm()->m_ee->m_ram, "mpeghle.txt", nullptr);
#ifdef VITAPS2_HAS_GL
		if(hardware && gsDiagLines)
		{
			static_cast<CGSH_Hardware*>(session.GetVm()->GetGSHandler())->SetDiagnosticsFile("gsdiag.txt", gsDiagLines);
		}
#endif

		std::FILE* statsFile = statsPath.empty() ? nullptr : std::fopen(statsPath.c_str(), "w");
		if(statsFile)
		{
			std::fprintf(statsFile, "frame,real_s,vm_fps,presented,ee_pc");
			for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++) std::fprintf(statsFile, ",ps2_%s", EmuProfile::GetSectionName(i));
			std::fprintf(statsFile, ",mpeg_pictures,mpeg_waits");
#ifdef VITAPS2_HAS_GL
			if(hardware)
			{
				std::fprintf(statsFile, ",gpu_pump_ms,gpu_gl_ms,gpu_tex_ms,gpu_xfer_ms,gpu_disp_ms,up_gl_ms,up_kpix,dl_gl_ms,dl_kpix"
				                        ",draws,tex_up,rt_up,rt_dl,up_px,dl_px,targets_new,xfer_in,xfer_out,xfer_local,rt_deferred,rt_skip_rows,xfer_same");
			}
#endif
			std::fprintf(statsFile, "\n");
		}

		ThreadProfiler::Sample();
		auto start = std::chrono::steady_clock::now();
		uint64_t serial = 0;
		std::vector<uint32_t> pixels;
		uint32_t width = 0, height = 0;
		uint64_t presented = 0;
		uint64_t nextShot = shotEvery, nextStats = 60, statsPresented = 0;
		auto statsTime = start;
#ifdef VITAPS2_HAS_GL
		CGSH_Hardware::GPU_TIMES statsGpu = CGSH_Hardware::GetTimes();
#endif
		CMpegHle::STATS statsMpeg;
		std::map<uint32, uint32> hotPcs;
		while(session.GetVmFrameCount() < targetFrames)
		{
			if(session.GetFrames().Fetch(serial, pixels, width, height)) presented++;
			if(std::chrono::steady_clock::now() - start > std::chrono::seconds(timeoutSeconds)) break;
			pump(5);
			if(!loadStatePath.empty() && (session.GetVmFrameCount() >= 120))
			{
				// The VM loads on its own thread; the GS keeps being pumped meanwhile.
				auto loading = session.GetVm()->LoadState(fs::path(loadStatePath));
				while(loading.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) pump(2);
				bool loaded = loading.get();
				std::printf("state %s: %s\n", loadStatePath.c_str(), loaded ? "loaded" : "FAILED");
				std::fflush(stdout);
#ifdef VITAPS2_HAS_GL
				if(loaded && hardware) static_cast<CGSH_Hardware*>(session.GetVm()->GetGSHandler())->DiscardGpuCopies();
#endif
				loadStatePath.clear();
			}
			uint64_t frame = session.GetVmFrameCount();
			if(frame >= hotFrom) hotPcs[session.GetDebugState().eePc]++;
			if(shotEvery && (frame >= nextShot))
			{
				nextShot = frame - (frame % shotEvery) + shotEvery;
				if(width) WritePpm(shotDir + "/shot_" + std::to_string(frame) + ".ppm", pixels, width, height);
			}
			if(statsFile && (frame >= nextStats))
			{
				// One row per emulated second.
				nextStats = frame - (frame % 60) + 60;
				auto now = std::chrono::steady_clock::now();
				double seconds = std::chrono::duration<double>(now - statsTime).count();
				statsTime = now;
				auto profile = session.TakeProfile();
				auto debug = session.GetDebugState();
				std::fprintf(statsFile, "%llu,%.2f,%.1f,%llu,%08X", (unsigned long long)frame, seconds, 60.0 / seconds,
				             (unsigned long long)(presented - statsPresented), debug.eePc);
				statsPresented = presented;
				for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++) std::fprintf(statsFile, ",%.3f", profile.share[i]);
				auto mpegStats = mpegHle.GetStats();
				std::fprintf(statsFile, ",%u,%u", mpegStats.pictures - statsMpeg.pictures, mpegStats.waits - statsMpeg.waits);
				statsMpeg = mpegStats;
#ifdef VITAPS2_HAS_GL
				if(hardware)
				{
					auto times = CGSH_Hardware::GetTimes();
					auto f = static_cast<CGSH_Hardware*>(session.GetVm()->GetGSHandler())->GetLastFrameStats();
					std::fprintf(statsFile, ",%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%.1f,%u", (times.pumpUs - statsGpu.pumpUs) / 1000.0,
					             (times.stageUs[0] - statsGpu.stageUs[0]) / 1000.0, (times.stageUs[1] - statsGpu.stageUs[1]) / 1000.0,
					             (times.stageUs[2] - statsGpu.stageUs[2]) / 1000.0, (times.stageUs[3] - statsGpu.stageUs[3]) / 1000.0,
					             (times.uploadGlUs - statsGpu.uploadGlUs) / 1000.0, static_cast<unsigned int>((times.uploadPixels - statsGpu.uploadPixels) / 1000),
					             (times.downloadGlUs - statsGpu.downloadGlUs) / 1000.0, static_cast<unsigned int>((times.downloadPixels - statsGpu.downloadPixels) / 1000));
					std::fprintf(statsFile, ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u", f.drawCalls, f.textureUploads, f.targetUploads, f.targetDownloads,
					             f.uploadedPixels, f.downloadedPixels, f.targetsCreated, f.transfersIn, f.transfersOut, f.transfersLocal,
					             f.deferredUploads, f.skippedUploadRows, f.unchangedTransfers);
					statsGpu = times;
				}
#endif
				std::fprintf(statsFile, "\n");
				std::fflush(statsFile);
			}
		}
		if(statsFile) std::fclose(statsFile);
		mpegHle.Disable();
		if(EmuProfile::g_vu1Programs.enabled)
		{
			if(std::FILE* file = std::fopen("vu1prog.txt", "w"))
			{
				WriteVu1ProgramStats(file);
				std::fclose(file);
			}
		}
		if(!hotPcs.empty())
		{
			// EE PC samples (one per runner loop) by address, most frequent first.
			std::vector<std::pair<uint32, uint32>> hot(hotPcs.begin(), hotPcs.end());
			std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			uint32 total = 0;
			for(const auto& entry : hot) total += entry.second;
			std::printf("EE hot (%u samples):", total);
			for(size_t i = 0; i < std::min<size_t>(hot.size(), 24); i++) std::printf(" %08X:%u", hot[i].first, hot[i].second);
			std::printf("\n");
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
		std::printf("EE s6 %08X s7 %08X\n", debug.eeGpr[22], debug.eeGpr[23]);
		{
			// DMA and VIF1 state (hangs waiting on a transfer).
			auto& ee = *session.GetVm()->m_ee;
			auto dmac = [&](uint32 address) { return ee.m_dmac.GetRegister(address); };
			std::printf("DMAC CTRL %08X STAT %08X PCR %08X STADR %08X ENABLER %08X\n", dmac(0x1000E000), dmac(0x1000E010), dmac(0x1000E020),
			            dmac(0x1000E040), dmac(0x1000F520));
			const std::pair<const char*, uint32> channels[] = {{"VIF0", 0x10008000}, {"VIF1", 0x10009000}, {"GIF", 0x1000A000},
			                                                   {"fromIPU", 0x1000B000}, {"toIPU", 0x1000B400}};
			for(const auto& [name, base] : channels)
				std::printf("D %-7s CHCR %08X MADR %08X QWC %04X TADR %08X\n", name, dmac(base), dmac(base + 0x10), dmac(base + 0x20), dmac(base + 0x30));
			auto& vif1 = ee.m_vpu1->GetVif();
			std::printf("VIF1 STAT %08X CODE %08X NUM %u TOP %X ITOP %X\n", vif1.GetRegister(0x10003C00), vif1.GetRegister(0x10003C80),
			            vif1.GetRegister(0x10003C60), vif1.GetRegister(0x10003CE0), vif1.GetRegister(0x10003CD0));
			std::printf("GIF STAT %08X active path %u\n", ee.m_gif.GetRegister(0x10003020), ee.m_gif.GetActivePath());
			std::printf("VU1 ready %d running %d threaded %d start %04X pc %04X\n", ee.m_vpu1->IsVuReady() ? 1 : 0, ee.m_vpu1->IsVuRunning() ? 1 : 0,
			            ee.m_vpu1->IsThreaded() ? 1 : 0, ee.m_vpu1->GetProgramStart(), ee.m_vpu1->GetCurrentPc());
			if(!ramDumpPath.empty())
			{
				if(std::FILE* file = std::fopen(ramDumpPath.c_str(), "wb"))
				{
					std::fwrite(ee.m_ram, 1, 0x02000000, file); //retail RAM (32 MB)
					std::fclose(file);
				}
			}
			if(EmuProfile::g_vu1Programs.enabled)
			{
				// VU1 micro memory and data memory, and the microcode disassembled (upper | lower).
				if(std::FILE* file = std::fopen("vu1micro.bin", "wb"))
				{
					std::fwrite(ee.m_microMem1, 1, PS2::MICROMEM1SIZE, file);
					std::fclose(file);
				}
				if(std::FILE* file = std::fopen("vu1mem.bin", "wb"))
				{
					std::fwrite(ee.m_vuMem1, 1, PS2::VUMEM1SIZE, file);
					std::fclose(file);
				}
				if(std::FILE* file = std::fopen("vu1micro.txt", "w"))
				{
					auto& vu1 = ee.m_VU1;
					for(uint32 address = 0; address < PS2::MICROMEM1SIZE; address += 8)
					{
						uint32 lower = *reinterpret_cast<const uint32*>(ee.m_microMem1 + address);
						uint32 upper = *reinterpret_cast<const uint32*>(ee.m_microMem1 + address + 4);
						char upperMnemonic[64] = {}, upperOperands[64] = {}, lowerMnemonic[64] = {}, lowerOperands[64] = {};
						vu1.m_pArch->GetInstructionMnemonic(&vu1, address + 4, upper, upperMnemonic, sizeof(upperMnemonic));
						vu1.m_pArch->GetInstructionOperands(&vu1, address + 4, upper, upperOperands, sizeof(upperOperands));
						vu1.m_pArch->GetInstructionMnemonic(&vu1, address, lower, lowerMnemonic, sizeof(lowerMnemonic));
						vu1.m_pArch->GetInstructionOperands(&vu1, address, lower, lowerOperands, sizeof(lowerOperands));
						std::fprintf(file, "%04X %08X %08X  %-10s %-24s | %-10s %s\n", address, upper, lower, upperMnemonic, upperOperands, lowerMnemonic, lowerOperands);
					}
					std::fclose(file);
				}
			}
		}
		if(!jitCachePath.empty())
		{
			std::printf("%s\n", JitCacheSetup::Describe().c_str());
			JitCacheSetup::Close();
		}
		std::printf("EE executed %llu idle %llu\n", (unsigned long long)session.GetEeExecutedTicks(), (unsigned long long)session.GetEeIdleTicks());
		{
			// Returns to the C++ dispatch loop per 1000 EE instructions (indirect jumps, unlinked blocks).
			double executed = static_cast<double>(std::max<uint64_t>(session.GetEeExecutedTicks(), 1));
			std::printf("dispatch: EE %llu (%.1f per 1000 instructions, %llu slices), IOP %llu (%llu slices)\n",
			            (unsigned long long)EmuProfile::g_dispatches[0], EmuProfile::g_dispatches[0] * 1000.0 / executed,
			            (unsigned long long)EmuProfile::g_executeCalls[0], (unsigned long long)EmuProfile::g_dispatches[1],
			            (unsigned long long)EmuProfile::g_executeCalls[1]);
			std::printf("regions: %u (%llu EE instructions)\n", EmuProfile::g_regions, (unsigned long long)EmuProfile::g_regionInstructions);
			std::printf("tier2: %u regions, %u not built, %llu EE instructions, %u inlined calls, %llu code bytes, %llu us\n", Tier2::g_stats.regions,
			            Tier2::g_stats.failed, (unsigned long long)Tier2::g_stats.guestInstructions, Tier2::g_stats.inlinedCalls,
			            (unsigned long long)Tier2::g_stats.codeBytes, (unsigned long long)Tier2::g_stats.compileUs);
			std::printf("tier2 runs: %llu\n", (unsigned long long)Tier2::g_stats.interpretedRuns);
		}
		session.GetFrames().Fetch(serial, pixels, width, height);
		for(const auto& usage : threadUsage)
		{
			std::printf("cpu %-18s %5.1f%%\n", usage.name.c_str(), usage.cpuPercent);
		}

		double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		uint64_t vmFrames = session.GetVmFrameCount();
		std::printf("vm frames: %llu, presented frames: %llu, %.2fs (%.1f fps)\n",
		            (unsigned long long)vmFrames, (unsigned long long)presented, seconds, vmFrames / seconds);
#ifdef VITAPS2_HAS_GL
		if(hardware) std::printf("game frames (GPU renderer): %u\n", CGSH_Hardware::g_gameFrames);
#endif

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
