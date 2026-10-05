// VitaPS2 - PlayStation 2 emulator frontend for the PlayStation Vita.
//
// Games (.iso/.cso/.chd/.isz/.cue/.mds/.bin) and homebrew (.elf) are read from
// ux0:data/VitaPS2/games. Configuration, memory cards and logs live in
// ux0:data/VitaPS2.
//
// In game:  SELECT + START  -> pause menu (renderer, speed hacks, display, quit)
//           SELECT + L      -> toggle the performance overlay
//
// Settings are remembered per game in ux0:data/VitaPS2/settings.
//
// Everything is drawn with vitaGL. The GPU GS renderer draws PS2 frames into
// GPU render targets on this (the main) thread; menus and overlays use the
// small Gfx layer on the same context. vitaGL compiles shaders at runtime and
// needs libshacccg.suprx (ur0:data/libshacccg.suprx or ur0:data/external/).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <vector>

#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <vitaGL.h>

#include "AutoCycleRate.h"
#include "CpuScreen.h"
#include "EmuSession.h"
#include "GameProfiles.h"
#include "GSH_Hardware.h"
#include "Gfx.h"
#include "GsBenchmark.h"
#include "JitMemory.h"
#include "PH_Vita.h"
#include "PS2VM.h"
#include "SH_Vita.h"
#include "ThreadProfiler.h"

// Memory layout: the newlib heap holds the emulated PS2 (EE/IOP RAM, VU, GS
// RAM, recompiler tables). The JIT code pool is a separate VM block allocated
// before vitaGL creates its pools (render targets and textures in CDRAM plus a
// RAM pool). Requires ATTRIBUTE2=12 in param.sfo.
extern "C"
{
	// Leaves room for the JIT pool (VM block) inside the app's memory budget.
	int _newlib_heap_size_user = 240 * 1024 * 1024;
	unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
}

namespace
{
	constexpr const char* DATA_PATH = "ux0:data/VitaPS2";
	constexpr const char* GAMES_PATH = "ux0:data/VitaPS2/games";
	constexpr const char* SETTINGS_PATH = "ux0:data/VitaPS2/settings";
	constexpr const char* LOG_PATH = "ux0:data/VitaPS2/log.txt";
	constexpr const char* BENCHMARK_PATH = "ux0:data/VitaPS2/benchmark.txt";
	constexpr const char* BUILTIN_TEST = "app0:gs_test.elf";
	constexpr const char* BUILTIN_TEST_NAME = "[Built-in] GS self test";
	constexpr size_t JIT_POOL_MIN = 8 * 1024 * 1024;
	constexpr size_t JIT_POOL_MAX = 48 * 1024 * 1024;
	// System RAM kept out of vitaGL's pools (thread stacks, audio, the shader
	// compiler's own allocations).
	constexpr int GL_RAM_LEFT_FREE = 24 * 1024 * 1024;

	constexpr uint32_t SCREEN_WIDTH = 960, SCREEN_HEIGHT = 544;
	constexpr uint32_t COLOR_WHITE = Gfx::Rgba(255, 255, 255, 255);
	constexpr uint32_t COLOR_GREY = Gfx::Rgba(150, 150, 160, 255);
	constexpr uint32_t COLOR_ACCENT = Gfx::Rgba(90, 160, 255, 255);
	constexpr uint32_t COLOR_WARN = Gfx::Rgba(255, 110, 110, 255);
	constexpr uint32_t COLOR_BG = Gfx::Rgba(16, 18, 28, 255);
	constexpr uint32_t COLOR_SELECTION = Gfx::Rgba(40, 60, 110, 255);

	void BeginFrame()
	{
		Gfx::BeginFrame(SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BG);
	}

	void EndFrame()
	{
		Gfx::Flush();
		vglSwapBuffers(GL_FALSE);
	}

	bool HasShaderCompiler()
	{
		SceIoStat stat;
		return (sceIoGetstat("ur0:data/libshacccg.suprx", &stat) >= 0) ||
		       (sceIoGetstat("ur0:data/external/libshacccg.suprx", &stat) >= 0);
	}

	struct INPUT
	{
		uint32_t held = 0;
		uint32_t pressed = 0;
	};

	INPUT ReadInput(uint32_t& previous)
	{
		SceCtrlData pad = {};
		sceCtrlPeekBufferPositive(0, &pad, 1);
		INPUT input;
		input.held = pad.buttons;
		input.pressed = pad.buttons & ~previous;
		previous = pad.buttons;
		return input;
	}

	void ShowMessage(const std::string& title, const std::string& body)
	{
		uint32_t previous = ~0u;
		while(true)
		{
			auto input = ReadInput(previous);
			if(input.pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) break;
			BeginFrame();
			Gfx::Text(40, 70, COLOR_ACCENT, 1.3f, title.c_str());
			Gfx::Text(40, 130, COLOR_WHITE, 0.85f, body.c_str());
			Gfx::Text(40, 510, COLOR_GREY, 0.8f, "Press X to continue");
			EndFrame();
		}
	}

	struct GAME_SETTINGS
	{
		uint32_t eeCycleRate = 100; //0: auto
		bool interlaced = false;
		uint32_t frameSkip = 0;
		bool showStats = true;
		bool stretch = false; //fill the 16:9 screen instead of 4:3
		bool softwareRenderer = false;
		bool threadedVu1 = true; //VU1 microprograms on the third core
	};

	std::string SettingsPathFor(const std::string& gamePath)
	{
		auto slash = gamePath.find_last_of('/');
		return std::string(SETTINGS_PATH) + "/" + gamePath.substr(slash + 1) + ".ini";
	}

	void ApplySetting(GAME_SETTINGS& settings, const std::string& key, const std::string& text)
	{
		uint32_t value = static_cast<uint32_t>(std::strtoul(text.c_str(), nullptr, 10));
		if(key == "ee_cycle_rate") settings.eeCycleRate = value;
		else if(key == "interlaced") settings.interlaced = value != 0;
		else if(key == "frame_skip") settings.frameSkip = value;
		else if(key == "show_stats") settings.showStats = value != 0;
		else if(key == "stretch") settings.stretch = value != 0;
		else if(key == "software_renderer") settings.softwareRenderer = value != 0;
		else if(key == "threaded_vu1") settings.threadedVu1 = value != 0;
	}

	// Defaults, then the bundled per-game profile, then the player's choices.
	GAME_SETTINGS LoadSettings(const std::string& gamePath, const CGameProfiles::PROFILE* profile)
	{
		GAME_SETTINGS settings;
		if(profile)
		{
			for(const auto& [key, value] : *profile) ApplySetting(settings, key, value);
		}
		std::ifstream file(SettingsPathFor(gamePath));
		std::string line;
		while(std::getline(file, line))
		{
			auto eq = line.find('=');
			if(eq == std::string::npos) continue;
			ApplySetting(settings, line.substr(0, eq), line.substr(eq + 1));
		}
		return settings;
	}

	void SaveSettings(const std::string& gamePath, const GAME_SETTINGS& settings)
	{
		std::ofstream file(SettingsPathFor(gamePath));
		file << "ee_cycle_rate=" << settings.eeCycleRate << "\n"
		     << "interlaced=" << (settings.interlaced ? 1 : 0) << "\n"
		     << "frame_skip=" << settings.frameSkip << "\n"
		     << "show_stats=" << (settings.showStats ? 1 : 0) << "\n"
		     << "stretch=" << (settings.stretch ? 1 : 0) << "\n"
		     << "software_renderer=" << (settings.softwareRenderer ? 1 : 0) << "\n"
		     << "threaded_vu1=" << (settings.threadedVu1 ? 1 : 0) << "\n";
	}

	// eeCycleRate 0 (Auto) runs at the rate the auto controller picked.
	CEmuSession::SPEED_HACKS ToSpeedHacks(const GAME_SETTINGS& settings, const CAutoCycleRate& autoRate)
	{
		CEmuSession::SPEED_HACKS hacks;
		hacks.eeCycleRatePercent = (settings.eeCycleRate == 0) ? autoRate.GetRate() : settings.eeCycleRate;
		hacks.interlacedRendering = settings.interlaced;
		hacks.frameSkip = settings.frameSkip;
		hacks.threadedVu1 = settings.threadedVu1;
		return hacks;
	}

	// The emulated display: a region of a GL texture.
	struct SCREEN
	{
		GLuint texture = 0;
		uint32_t textureWidth = 0, textureHeight = 0;
		uint32_t x = 0, y = 0, width = 0, height = 0;
	};

	void DrawScreen(const SCREEN& screen, bool stretch)
	{
		if(!screen.texture || (screen.width == 0) || (screen.height == 0)) return;
		float dstH = static_cast<float>(SCREEN_HEIGHT);
		float dstW = stretch ? static_cast<float>(SCREEN_WIDTH) : dstH * 4.0f / 3.0f;
		Gfx::Image(screen.texture, screen.textureWidth, screen.textureHeight, static_cast<float>(screen.x),
		           static_cast<float>(screen.y), static_cast<float>(screen.width), static_cast<float>(screen.height),
		           (SCREEN_WIDTH - dstW) / 2.0f, 0, dstW, dstH, true);
	}

	// Returns true if the player chose to quit to the game list.
	bool RunPauseMenu(CEmuSession& session, GAME_SETTINGS& settings, const SCREEN& screen, const CAutoCycleRate& autoRate)
	{
		static const uint32_t eeRates[] = {0, 50, 60, 75, 90, 100, 130};
		constexpr int eeRateCount = sizeof(eeRates) / sizeof(eeRates[0]);
		enum ITEM
		{
			ITEM_RESUME,
			ITEM_EE_RATE,
			ITEM_INTERLACED,
			ITEM_FRAMESKIP,
			ITEM_STRETCH,
			ITEM_STATS,
			ITEM_RENDERER,
			ITEM_VU1_THREAD,
			ITEM_QUIT,
			ITEM_COUNT
		};
		session.Pause();
		int selected = 0;
		uint32_t previous = ~0u;
		bool quit = false;
		while(true)
		{
			auto input = ReadInput(previous);
			if(input.pressed & SCE_CTRL_DOWN) selected = (selected + 1) % ITEM_COUNT;
			if(input.pressed & SCE_CTRL_UP) selected = (selected + ITEM_COUNT - 1) % ITEM_COUNT;
			int delta = (input.pressed & SCE_CTRL_RIGHT) ? 1 : (input.pressed & SCE_CTRL_LEFT) ? -1 : 0;
			if(input.pressed & SCE_CTRL_CROSS) delta = 1;
			if(input.pressed & SCE_CTRL_CIRCLE) break;
			if(delta != 0)
			{
				switch(selected)
				{
				case ITEM_RESUME:
					if(input.pressed & SCE_CTRL_CROSS) goto done;
					break;
				case ITEM_EE_RATE:
				{
					int index = 0;
					for(int i = 0; i < eeRateCount; i++)
						if(eeRates[i] == settings.eeCycleRate) index = i;
					index = std::clamp(index + delta, 0, eeRateCount - 1);
					settings.eeCycleRate = eeRates[index];
				}
				break;
				case ITEM_INTERLACED: settings.interlaced = !settings.interlaced; break;
				case ITEM_FRAMESKIP: settings.frameSkip = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(settings.frameSkip) + delta, 0, 3)); break;
				case ITEM_STRETCH: settings.stretch = !settings.stretch; break;
				case ITEM_STATS: settings.showStats = !settings.showStats; break;
				case ITEM_RENDERER: settings.softwareRenderer = !settings.softwareRenderer; break;
				case ITEM_VU1_THREAD: settings.threadedVu1 = !settings.threadedVu1; break;
				case ITEM_QUIT:
					if(input.pressed & SCE_CTRL_CROSS)
					{
						quit = true;
						goto done;
					}
					break;
				}
			}

			{
				char lines[ITEM_COUNT][96];
				std::snprintf(lines[ITEM_RESUME], 96, "Resume");
				if(settings.eeCycleRate == 0)
					std::snprintf(lines[ITEM_EE_RATE], 96, "EE cycle rate: Auto, now %u%%  (lowered while the EE is the bottleneck)", autoRate.GetRate());
				else
					std::snprintf(lines[ITEM_EE_RATE], 96, "EE cycle rate: %u%%  (lower = faster, may slow game logic)", settings.eeCycleRate);
				std::snprintf(lines[ITEM_INTERLACED], 96, "Interlaced rendering: %s  (software renderer)", settings.interlaced ? "On" : "Off");
				std::snprintf(lines[ITEM_FRAMESKIP], 96, "Frame skip: %u", settings.frameSkip);
				std::snprintf(lines[ITEM_STRETCH], 96, "Aspect: %s", settings.stretch ? "Stretch 16:9" : "4:3");
				std::snprintf(lines[ITEM_STATS], 96, "Performance overlay: %s", settings.showStats ? "On" : "Off");
				std::snprintf(lines[ITEM_RENDERER], 96, "Renderer: %s  (applies when the game restarts)", settings.softwareRenderer ? "Software" : "GPU");
				std::snprintf(lines[ITEM_VU1_THREAD], 96, "VU1 on its own core: %s  (faster 3D; turn off if a game glitches)", settings.threadedVu1 ? "On" : "Off");
				std::snprintf(lines[ITEM_QUIT], 96, "Quit to game list");

				BeginFrame();
				DrawScreen(screen, settings.stretch);
				Gfx::Rect(90, 60, 780, 440, Gfx::Rgba(10, 12, 24, 220));
				Gfx::Text(120, 100, COLOR_ACCENT, 1.1f, "Paused");
				for(int i = 0; i < ITEM_COUNT; i++)
				{
					int y = 145 + i * 32;
					if(i == selected) Gfx::Rect(110, y - 24, 740, 32, COLOR_SELECTION);
					Gfx::Text(125, y, (i == selected) ? COLOR_WHITE : COLOR_GREY, 0.8f, lines[i]);
				}
				Gfx::Text(120, 480, COLOR_GREY, 0.7f, "LEFT/RIGHT: change   X: select   O: resume");
				EndFrame();
			}
		}
	done:
		session.SetSpeedHacks(ToSpeedHacks(settings, autoRate));
		if(!quit) session.Resume();
		return quit;
	}

	std::vector<std::string> ScanGames()
	{
		std::vector<std::string> games;
		if(DIR* dir = opendir(GAMES_PATH))
		{
			while(dirent* entry = readdir(dir))
			{
				std::string name = entry->d_name;
				std::string path = std::string(GAMES_PATH) + "/" + name;
				if(CEmuSession::IsBootableExecutable(path) || CEmuSession::IsBootableDiscImage(path))
				{
					games.push_back(name);
				}
			}
			closedir(dir);
		}
		std::sort(games.begin(), games.end());
		games.insert(games.begin(), BUILTIN_TEST_NAME);
		return games;
	}

	// Runs the software renderer benchmark on the device and shows/saves the results.
	void RunBenchmarkScreen()
	{
		std::vector<std::string> lines;
		auto draw = [&](const char* status) {
			BeginFrame();
			Gfx::Text(30, 45, COLOR_ACCENT, 1.2f, "Software GS benchmark (Mpix/s)");
			for(size_t i = 0; i < lines.size(); i++)
			{
				Gfx::Text(30, 80 + static_cast<int>(i) * 20, COLOR_WHITE, 0.7f, lines[i].c_str());
			}
			Gfx::Text(30, 525, COLOR_GREY, 0.8f, status);
			EndFrame();
		};

		FILE* file = std::fopen(BENCHMARK_PATH, "w");
		for(uint32_t threads : {1u, 2u, 3u})
		{
			char header[64];
			std::snprintf(header, sizeof(header), "-- %u rasterizer thread(s) --", threads);
			lines.push_back(header);
			if(file) std::fprintf(file, "%s\n", header);
			draw("Running... (about 20 seconds)");
			RunGsBenchmark(1.0, threads, [&](const GS_BENCHMARK_RESULT& result) {
				char line[128];
				std::snprintf(line, sizeof(line), "%-46s %7.1f  (%5.1f frames/s)", result.name.c_str(), result.mpixelsPerSecond, result.framesPerSecond);
				lines.push_back(line);
				if(file) std::fprintf(file, "%s\n", line);
				std::printf("benchmark: %s\n", line);
				draw("Running... (about 20 seconds)");
			});
		}
		if(file) std::fclose(file);

		uint32_t previous = ~0u;
		while(true)
		{
			auto input = ReadInput(previous);
			if(input.pressed & (SCE_CTRL_CROSS | SCE_CTRL_CIRCLE)) break;
			draw("Saved to ux0:data/VitaPS2/benchmark.txt - press X to go back");
		}
	}

	// Returns the full path of the selected game, or an empty string to quit.
	std::string RunBrowser()
	{
		auto games = ScanGames();
		int selected = 0;
		int scroll = 0;
		const int visible = 15;
		uint32_t previous = ~0u;

		while(true)
		{
			auto input = ReadInput(previous);
			int count = static_cast<int>(games.size());
			if(count > 0)
			{
				if(input.pressed & SCE_CTRL_DOWN) selected = (selected + 1) % count;
				if(input.pressed & SCE_CTRL_UP) selected = (selected + count - 1) % count;
				if(input.pressed & SCE_CTRL_RIGHT) selected = std::min(selected + visible, count - 1);
				if(input.pressed & SCE_CTRL_LEFT) selected = std::max(selected - visible, 0);
				if(input.pressed & SCE_CTRL_CROSS)
				{
					if(games[selected] == BUILTIN_TEST_NAME) return BUILTIN_TEST;
					return std::string(GAMES_PATH) + "/" + games[selected];
				}
			}
			if(input.pressed & SCE_CTRL_TRIANGLE)
			{
				games = ScanGames();
				selected = 0;
			}
			if(input.pressed & SCE_CTRL_START) return std::string();
			if(input.pressed & SCE_CTRL_SELECT)
			{
				RunBenchmarkScreen();
				previous = ~0u;
			}

			if(selected < scroll) scroll = selected;
			if(selected >= scroll + visible) scroll = selected - visible + 1;

			BeginFrame();
			Gfx::Text(30, 45, COLOR_ACCENT, 1.4f, "VitaPS2");
			Gfx::Text(180, 45, COLOR_GREY, 0.8f, "PlayStation 2 emulator (experimental)");
			for(int i = scroll; i < std::min(count, scroll + visible); i++)
			{
				int y = 95 + (i - scroll) * 26;
				if(i == selected) Gfx::Rect(20, y - 20, 920, 26, COLOR_SELECTION);
				Gfx::Text(30, y, (i == selected) ? COLOR_WHITE : COLOR_GREY, 0.8f, games[i].c_str());
			}
			if(count <= 1)
			{
				Gfx::Textf(30, 490, COLOR_WHITE, 0.8f, "Copy .iso/.cso/.chd/.elf files to %s", GAMES_PATH);
			}
			Gfx::Text(30, 530, COLOR_GREY, 0.65f,
			          "X: boot  TRIANGLE: refresh  SELECT: GS benchmark  START: quit  |  In game: SELECT+START menu");
			EndFrame();
		}
	}

	CGSH_Hardware* GetHardwareGs(CEmuSession* session)
	{
		return session ? static_cast<CGSH_Hardware*>(session->GetVm()->GetGSHandler()) : nullptr;
	}

	const CGameProfiles& GetGameProfiles()
	{
		static CGameProfiles profiles;
		static bool loaded = false;
		if(!loaded)
		{
			loaded = true;
			if(profiles.Load("app0:game_profiles.ini"))
				std::printf("game profiles: %u\n", static_cast<unsigned int>(profiles.GetCount()));
		}
		return profiles;
	}

	void RunGame(const std::string& path)
	{
		const std::string discSerial = CEmuSession::GetDiscSerial(path);
		const auto* profile = discSerial.empty() ? nullptr : GetGameProfiles().Find(discSerial);
		std::string gameName;
		if(profile)
		{
			auto name = profile->find("name");
			if(name != profile->end()) gameName = name->second;
		}
		std::printf("booting %s serial '%s' profile %s\n", path.c_str(), discSerial.c_str(),
		            profile ? (gameName.empty() ? "yes" : gameName.c_str()) : "none");
		GAME_SETTINGS settings = LoadSettings(path, profile);
		CAutoCycleRate autoRate;
		// PAL discs (SCES/SLES/SCED...) run at 50 fps.
		const float targetFps = ((discSerial.size() > 2) && (discSerial[2] == 'E' || discSerial[2] == 'e')) ? 50.0f : 60.0f;
		const bool gpu = !settings.softwareRenderer;

		CPH_Vita* pad = nullptr;
		CEmuSession* sessionPtr = nullptr;
		CEmuSession::CONFIG config;
		config.dataPath = DATA_PATH;
		config.resourcesPath = "app0:";
		config.limitFrameRate = true;
		config.padFactory = CPH_Vita::GetFactoryFunction(&pad);
		// Third core: VU1 (the audio thread there is light)
		config.vu1ThreadInit = []() { sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x40000 /* SCE_KERNEL_CPU_MASK_USER_2 */); };
		config.soundFactory = &CSH_Vita::HandlerFactory;
		config.interlacedRendering = settings.interlaced;
		config.frameSkip = settings.frameSkip;
		if(gpu)
		{
			// The GS runs on this thread (it owns the GL context): cores are
			// the EE/IOP/VU thread, this thread and audio.
			config.gsThreaded = false;
			config.gsFactory = CGSH_Hardware::GetFactoryFunction(CGSH_Hardware::OPTIONS());
			config.gsPump = [&sessionPtr]() {
				if(auto gs = GetHardwareGs(sessionPtr)) gs->Pump(2);
			};
			config.gsShutdown = [&sessionPtr]() {
				if(auto gs = GetHardwareGs(sessionPtr)) gs->ReleaseGpu();
			};
		}
		else
		{
			// Vita: 3 cores for apps. EE/IOP/VU (Play! VM thread), GS thread,
			// one extra rasterizer worker.
			config.gsThreaded = true;
			config.rasterizerThreads = 2;
		}

		std::unique_ptr<CEmuSession> session;
		try
		{
			session = std::make_unique<CEmuSession>(config);
			sessionPtr = session.get();
			session->SetSpeedHacks(ToSpeedHacks(settings, autoRate));
			session->Boot(path);
		}
		catch(const std::exception& e)
		{
			session.reset();
			ShowMessage("Failed to boot", e.what());
			return;
		}

		SCREEN screen;
		std::vector<uint32_t> pixels;
		uint64_t serial = 0;
		uint32_t previousButtons = ~0u;

		uint64_t statsTime = sceKernelGetProcessTimeWide();
		uint64_t lastPresent = 0;
		bool newContent = true;
		uint64_t statsVmFrames = 0, statsPresented = 0, presented = 0;
		float vmFps = 0, presentFps = 0;
		std::vector<ThreadProfiler::SAMPLE> threadUsage;
		ThreadProfiler::Sample();

		while(true)
		{
			uint32_t buttons = pad ? pad->Poll() : 0;
			uint32_t pressed = buttons & ~previousButtons;
			previousButtons = buttons;
			if((buttons & SCE_CTRL_SELECT) && (pressed & SCE_CTRL_START))
			{
				uint32_t previousRate = settings.eeCycleRate;
				bool quit = RunPauseMenu(*session, settings, screen, autoRate);
				if((settings.eeCycleRate == 0) && (previousRate != 0))
				{
					autoRate.Reset();
					session->SetSpeedHacks(ToSpeedHacks(settings, autoRate));
				}
				SaveSettings(path, settings);
				if(quit) break;
				previousButtons = ~0u;
				newContent = true;
				continue;
			}
			if((buttons & SCE_CTRL_SELECT) && (pressed & SCE_CTRL_L1))
			{
				settings.showStats = !settings.showStats;
				SaveSettings(path, settings);
				newContent = true;
			}

			if(gpu)
			{
				// Run the GS until the game presents a frame, but keep the UI
				// responsive when it does not.
				auto gs = GetHardwareGs(session.get());
				uint64_t start = sceKernelGetProcessTimeWide();
				bool flipped = false;
				while(!flipped && (sceKernelGetProcessTimeWide() - start < 16000))
				{
					flipped = gs->Pump(4);
				}
				if(flipped)
				{
					presented++;
					newContent = true;
				}
				auto display = gs->GetDisplayTexture();
				screen.texture = display.texture;
				screen.textureWidth = display.textureWidth;
				screen.textureHeight = display.textureHeight;
				screen.x = display.x;
				screen.y = display.y;
				screen.width = display.width;
				screen.height = display.height;
			}
			else
			{
				uint32_t width = 0, height = 0;
				if(session->GetFrames().Fetch(serial, pixels, width, height))
				{
					presented++;
					newContent = true;
					Gfx::UploadFrame(pixels.data(), width, height);
					screen.texture = Gfx::FrameTexture();
					screen.textureWidth = screen.textureHeight = Gfx::FRAME_TEXTURE_SIZE;
					screen.x = screen.y = 0;
					screen.width = std::min(width, Gfx::FRAME_TEXTURE_SIZE);
					screen.height = std::min(height, Gfx::FRAME_TEXTURE_SIZE);
				}
			}

			uint64_t now = sceKernelGetProcessTimeWide();
			if(now - statsTime >= 1000000)
			{
				float seconds = static_cast<float>(now - statsTime) / 1000000.0f;
				uint64_t vmFrames = session->GetVmFrameCount();
				vmFps = static_cast<float>(vmFrames - statsVmFrames) / seconds;
				presentFps = static_cast<float>(presented - statsPresented) / seconds;
				statsVmFrames = vmFrames;
				statsPresented = presented;
				statsTime = now;
				threadUsage = ThreadProfiler::Sample();
				for(const auto& usage : threadUsage)
				{
					std::printf("cpu %-18s %5.1f%%\n", usage.name.c_str(), usage.cpuPercent);
				}
				float eeIdle = session->GetEeIdleRatio();
				std::printf("fps vm %.1f out %.1f ee-idle %.0f%%\n", vmFps, presentFps, eeIdle * 100.0f);
				if((settings.eeCycleRate == 0) && autoRate.Update(eeIdle, vmFps, targetFps))
				{
					std::printf("auto ee cycle rate: %u%%\n", autoRate.GetRate());
					session->SetSpeedHacks(ToSpeedHacks(settings, autoRate));
				}
				newContent |= settings.showStats;
			}

			// Redrawing an unchanged screen costs GPU time the GS needs.
			if(!newContent && (now - lastPresent < 100000))
			{
				if(!gpu) sceKernelDelayThread(2000); //the GPU path waits in Pump()
				continue;
			}
			newContent = false;
			lastPresent = now;

			BeginFrame();
			DrawScreen(screen, settings.stretch);
			if(settings.showStats)
			{
				int lines = 5 + static_cast<int>(threadUsage.size());
				Gfx::Rect(0, 0, 400, 10 + lines * 20, Gfx::Rgba(0, 0, 0, 160));
				Gfx::Textf(8, 20, COLOR_WHITE, 0.7f, "VM %.1f fps  out %.1f fps  %ux%u", vmFps, presentFps, screen.width, screen.height);
				uint32_t eeRate = (settings.eeCycleRate == 0) ? autoRate.GetRate() : settings.eeCycleRate;
				Gfx::Textf(8, 40, COLOR_GREY, 0.7f, "EE idle %.0f%%  rate %u%%%s  JIT %u/%u KB  %s", session->GetEeIdleRatio() * 100.0f,
				           eeRate, (settings.eeCycleRate == 0) ? " auto" : "",
				           static_cast<unsigned int>(VitaJit_GetUsedBytes() / 1024),
				           static_cast<unsigned int>(VitaJit_GetCapacity() / 1024),
				           discSerial.empty() ? "" : discSerial.c_str());
				if(gpu)
				{
					auto stats = GetHardwareGs(session.get())->GetLastFrameStats();
					Gfx::Textf(8, 60, COLOR_GREY, 0.7f, "GPU draws %u  tex %u  rt dl %u up %u  ~blend %u", stats.drawCalls,
					           stats.textureUploads, stats.targetDownloads, stats.targetUploads, stats.approximateBlends);
				}
				else
				{
					Gfx::Text(8, 60, COLOR_GREY, 0.7f, "Software renderer");
				}
				Gfx::Textf(8, 80, COLOR_GREY, 0.7f, "Audio tempo %.0f%%  underruns %u", CSH_Vita::GetTempo() * 100.0f,
				           CSH_Vita::GetUnderruns());
				Gfx::Text(8, 100, COLOR_ACCENT, 0.7f, "CPU per thread (100% = one core):");
				int line = 0;
				for(const auto& usage : threadUsage)
				{
					uint32_t color = (usage.cpuPercent > 90.0f) ? COLOR_WARN : COLOR_GREY;
					Gfx::Textf(16, 120 + line * 20, color, 0.7f, "%-18s %5.1f%%", usage.name.c_str(), usage.cpuPercent);
					line++;
				}
			}
			EndFrame();
		}

		session.reset();
		sessionPtr = nullptr;
	}
}

int main()
{
	sceIoMkdir(DATA_PATH, 0777);
	// Everything printed (ours and Play!'s) goes to a log file for bug reports.
	if(std::freopen(LOG_PATH, "w", stdout)) setvbuf(stdout, nullptr, _IOLBF, 0);
	if(std::freopen(LOG_PATH, "a", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
	std::printf("VitaPS2 starting\n");
	ThreadProfiler::RegisterCurrentThread("UI/GPU");

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

	sceIoMkdir(GAMES_PATH, 0777);
	sceIoMkdir(SETTINGS_PATH, 0777);

	// Before vitaGL, which takes most of the remaining memory for its pools.
	if(!VitaJit_Init(JIT_POOL_MIN, JIT_POOL_MAX))
	{
		std::string details = VitaJit_GetDiagnostics() +
		                      "\nIf the error is 0x80020??? check that \"Unsafe Homebrew\" is enabled\n"
		                      "in HENkaku Settings. Please report this screen with log.txt.";
		CpuScreen_ShowMessage("Cannot allocate JIT memory", details);
		sceKernelExitProcess(0);
		return 0;
	}

	if(!HasShaderCompiler())
	{
		CpuScreen_ShowMessage("Missing libshacccg.suprx",
		                      "VitaPS2 draws with vitaGL, which needs the runtime shader compiler.\n"
		                      "Extract it with ShaRKBR33D (or SharkF00D) so that this file exists:\n"
		                      "  ur0:data/libshacccg.suprx\n\n"
		                      "Many Vita ports need it; you only have to do this once.");
		sceKernelExitProcess(0);
		return 0;
	}

	SceKernelFreeMemorySizeInfo memInfo = {};
	memInfo.size = sizeof(memInfo);
	sceKernelGetFreeMemorySize(&memInfo);
	std::printf("free memory before vitaGL: user %u KB, cdram %u KB, phycont %u KB\n", memInfo.size_user / 1024,
	            memInfo.size_cdram / 1024, memInfo.size_phycont / 1024);
	// Compiled fixed-function shaders are cached so they are only built once.
	sceIoMkdir("ux0:data/VitaPS2/shaders", 0777);
	vglSetShaderCachePath("ux0:data/VitaPS2/shaders/");
	vglInitExtended(0x80000, SCREEN_WIDTH, SCREEN_HEIGHT, GL_RAM_LEFT_FREE, SCE_GXM_MULTISAMPLE_NONE);
	vglWaitVblankStart(GL_TRUE);
	if(!Gfx::Init())
	{
		CpuScreen_ShowMessage("Graphics initialization failed", "vitaGL could not create the UI textures.\nPlease report this with log.txt.");
		sceKernelExitProcess(0);
		return 0;
	}

	try
	{
		CEmuSession::SetDataPaths(DATA_PATH, "app0:");
	}
	catch(const std::exception& e)
	{
		ShowMessage("Initialization failed", e.what());
	}

	while(true)
	{
		auto path = RunBrowser();
		if(path.empty()) break;
		RunGame(path);
	}

	Gfx::Shutdown();
	sceKernelExitProcess(0);
	return 0;
}
