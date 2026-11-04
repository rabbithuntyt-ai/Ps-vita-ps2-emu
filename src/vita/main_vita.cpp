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
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <map>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/power.h>
#include <vitaGL.h>

#include "AutoCycleRate.h"
#include "AutoTuner.h"
#ifndef VITAPS2_BUILD
#define VITAPS2_BUILD "dev"
#endif
#include "CpuScreen.h"
#include "EmuSession.h"
#include "EmuProfile.h"
#include "MpegHle.h"
#include "MpegShadow.h"
#include "Ps2Const.h"
#include "ee/Ee_SubSystem.h"
#include "Jitter.h"
#include "Log.h"
#include "GameProfiles.h"
#include "JitCacheSetup.h"
#include "Theme.h"
#include "GSH_Hardware.h"
#include "Gfx.h"
#include "GsBenchmark.h"
#include "GsHardwareBenchmark.h"
#include "JitMemory.h"
#include "PH_Vita.h"
#include "PS2VM.h"
#include "ee/IPU.h"
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
	constexpr const char* STATES_PATH = "ux0:data/VitaPS2/states";
	constexpr const char* JIT_CACHE_PATH = "ux0:data/VitaPS2/jitcache";
	constexpr const char* LOG_PATH = "ux0:data/VitaPS2/log.txt";
	constexpr const char* BENCHMARK_PATH = "ux0:data/VitaPS2/benchmark.txt";
	// Scripted runs (e.g. from Vita Lab): see ReadAutostart.
	constexpr const char* AUTOSTART_PATH = "ux0:data/VitaPS2/autostart.txt";
	constexpr const char* AUTOSTART_DONE_PATH = "ux0:data/VitaPS2/autostart.done";
	constexpr const char* PERF_PATH = "ux0:data/VitaPS2/perf.csv";
	uint64_t g_presentUs = 0; //frontend frame drawing + vglSwapBuffers (perf.csv)

	// autostart.txt boots a game directly, for scripted tests:
	//   game=ux0:data/VitaPS2/games/game.iso
	//   seconds=60              (optional: quit the app after this long)
	//   ee_cycle_rate=100 ...   (optional: any per-game setting, not saved)
	// The file is renamed to autostart.done when read, so a crash doesn't
	// loop. While it runs, perf.csv gets one line of measurements per second.
	struct AUTOSTART
	{
		std::string game;
		uint32_t seconds = 0;
		std::vector<std::pair<std::string, std::string>> settings;
	};
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
		// Every screen (browser, menus, game) draws through here: keep the
		// Vita awake while the app is in front: DEFAULT counts as user activity
		// (no screen dimming or screen off, no suspend). DISABLE_AUTO_SUSPEND alone
		// still lets the screen turn off.
		sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
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
		bool safeJit = false;    //no NEON/VFP register allocation in the JIT
		bool threadedSpu = true; //SPU2 audio mixing on the second core
		bool jitCache = true;    //keep compiled code between runs (ux0:data/VitaPS2/jitcache)
		bool movieHle = false;   //native movie decoding (libmpeg 2.2 games, docs/HLE_MPEG.md)
	};

	std::string SettingsPathFor(const std::string& gamePath)
	{
		auto slash = gamePath.find_last_of('/');
		return std::string(SETTINGS_PATH) + "/" + gamePath.substr(slash + 1) + ".ini";
	}

	// One save state per game.
	std::string StatePath(const std::string& gamePath)
	{
		auto slash = gamePath.find_last_of('/');
		return std::string(STATES_PATH) + "/" + gamePath.substr(slash + 1) + ".state";
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
		else if(key == "safe_jit") settings.safeJit = value != 0;
		else if(key == "threaded_spu") settings.threadedSpu = value != 0;
		else if(key == "jit_cache") settings.jitCache = value != 0;
		else if(key == "mpeg_hle") settings.movieHle = value != 0;
	}

	// What the first-boot tuner found for a game (CAutoTuner).
	std::string AutoTunePathFor(const std::string& gamePath)
	{
		auto slash = gamePath.find_last_of('/');
		return std::string(SETTINGS_PATH) + "/" + gamePath.substr(slash + 1) + ".auto.ini";
	}

	bool ApplySettingsFile(GAME_SETTINGS& settings, const std::string& path)
	{
		std::ifstream file(path);
		if(!file) return false;
		std::string line;
		while(std::getline(file, line))
		{
			auto eq = line.find('=');
			if(eq == std::string::npos) continue;
			ApplySetting(settings, line.substr(0, eq), line.substr(eq + 1));
		}
		return true;
	}

	// Defaults, then the bundled per-game profile, then what the first-boot
	// tuner found, then the player's choices.
	GAME_SETTINGS LoadSettings(const std::string& gamePath, const CGameProfiles::PROFILE* profile, bool& tuned, bool& playerFile)
	{
		GAME_SETTINGS settings;
		if(profile)
		{
			for(const auto& [key, value] : *profile) ApplySetting(settings, key, value);
		}
		tuned = ApplySettingsFile(settings, AutoTunePathFor(gamePath));
		playerFile = ApplySettingsFile(settings, SettingsPathFor(gamePath));
		return settings;
	}

	void SaveAutoTune(const std::string& gamePath, const CAutoTuner& tuner)
	{
		std::ofstream file(AutoTunePathFor(gamePath));
		file << "# first-boot tuning: " << tuner.GetFinalFps() << " emulated fps\n"
		     << "threaded_vu1=" << (tuner.GetChoice().threadedVu1 ? 1 : 0) << "\n"
		     << "frame_skip=" << tuner.GetChoice().frameSkip << "\n";
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
		     << "threaded_vu1=" << (settings.threadedVu1 ? 1 : 0) << "\n"
		     << "safe_jit=" << (settings.safeJit ? 1 : 0) << "\n"
		     << "threaded_spu=" << (settings.threadedSpu ? 1 : 0) << "\n";
	}

	// eeCycleRate 0 (Auto) runs at the rate the auto controller picked.
	CEmuSession::SPEED_HACKS ToSpeedHacks(const GAME_SETTINGS& settings, const CAutoCycleRate& autoRate)
	{
		CEmuSession::SPEED_HACKS hacks;
		hacks.eeCycleRatePercent = (settings.eeCycleRate == 0) ? autoRate.GetRate() : settings.eeCycleRate;
		hacks.interlacedRendering = settings.interlaced;
		hacks.frameSkip = settings.frameSkip;
		hacks.threadedVu1 = settings.threadedVu1;
		hacks.threadedSpu = settings.threadedSpu;
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
			ITEM_SPU_THREAD,
			ITEM_SAFE_JIT,
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
				case ITEM_SPU_THREAD: settings.threadedSpu = !settings.threadedSpu; break;
				case ITEM_SAFE_JIT: settings.safeJit = !settings.safeJit; break;
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
				std::snprintf(lines[ITEM_SPU_THREAD], 96, "Audio on its own core: %s  (faster; turn off if sound glitches)", settings.threadedSpu ? "On" : "Off");
				std::snprintf(lines[ITEM_SAFE_JIT], 96, "Safe JIT: %s  (slower; try if a game hangs - applies on restart)", settings.safeJit ? "On" : "Off");
				std::snprintf(lines[ITEM_QUIT], 96, "Quit to game list");

				BeginFrame();
				DrawScreen(screen, settings.stretch);
				Gfx::Rect(90, 60, 780, 440, Gfx::Rgba(10, 12, 24, 220));
				Gfx::Text(120, 100, COLOR_ACCENT, 1.1f, "Paused");
				for(int i = 0; i < ITEM_COUNT; i++)
				{
					int y = 135 + i * 27;
					if(i == selected) Gfx::Rect(110, y - 21, 740, 27, COLOR_SELECTION);
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

	// Runs the GS benchmark (software renderer, then GPU renderer) on the
	// device and shows/saves the results.
	void RunBenchmarkScreen()
	{
		std::vector<std::string> lines;
		auto draw = [&](const char* status) {
			BeginFrame();
			Gfx::Text(30, 45, COLOR_ACCENT, 1.2f, "GS benchmark (Mpix/s)  build " VITAPS2_BUILD);
			for(size_t i = 0; i < lines.size(); i++)
			{
				Gfx::Text(30, 80 + static_cast<int>(i) * 20, COLOR_WHITE, 0.7f, lines[i].c_str());
			}
			Gfx::Text(30, 525, COLOR_GREY, 0.8f, status);
			EndFrame();
		};

		FILE* file = std::fopen(BENCHMARK_PATH, "w");
		if(file) std::fprintf(file, "build %s\n", VITAPS2_BUILD);
		auto addHeader = [&](const char* header) {
			lines.push_back(header);
			if(file) std::fprintf(file, "%s\n", header);
			draw("Running... (about 20 seconds)");
		};
		auto addResult = [&](const GS_BENCHMARK_RESULT& result) {
			char line[128];
			std::snprintf(line, sizeof(line), "%-46s %7.1f  (%5.1f frames/s)", result.name.c_str(), result.mpixelsPerSecond, result.framesPerSecond);
			lines.push_back(line);
			if(file) std::fprintf(file, "%s\n", line);
			std::printf("benchmark: %s\n", line);
			draw("Running... (about 20 seconds)");
		};
		for(uint32_t threads : {1u, 3u})
		{
			char header[64];
			std::snprintf(header, sizeof(header), "-- software renderer, %u thread(s) --", threads);
			addHeader(header);
			RunGsBenchmark(1.0, threads, addResult);
		}
		addHeader("-- GPU renderer (what games use by default) --");
		RunGsHardwareBenchmark(1.0, addResult);
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
		const int visible = 12;
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

			float seconds = static_cast<float>(sceKernelGetProcessTimeWide() % 100000000000ULL) / 1000000.0f;
			BeginFrame();
			Theme::Browser(games, selected, scroll, visible, seconds, "PlayStation 2 emulator  build " VITAPS2_BUILD, GAMES_PATH);
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

	void RunGame(const std::string& path, const AUTOSTART* autostart = nullptr)
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
		bool tuned = false, playerFile = false;
		GAME_SETTINGS settings = LoadSettings(path, profile, tuned, playerFile);
		// First boot of a game the player has not configured: try the speed
		// settings that help some games and hurt others (scripted runs only with
		// auto_tune=1, so experiments keep their settings).
		bool autoTune = !tuned && !playerFile;
		std::FILE* perfFile = nullptr;
		if(autostart)
		{
			for(const auto& [key, value] : autostart->settings) ApplySetting(settings, key, value);
			bool tuneRequested = std::any_of(autostart->settings.begin(), autostart->settings.end(),
			                                 [](const auto& setting) { return (setting.first == "auto_tune") && (setting.second == "1"); });
			autoTune = autoTune && tuneRequested;
			perfFile = std::fopen(PERF_PATH, "w");
			if(perfFile)
			{
				std::fprintf(perfFile, "t,vm_fps,out_fps,ee_idle,ee_rate");
				for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++) std::fprintf(perfFile, ",ps2_%s", EmuProfile::GetSectionName(i));
				std::fprintf(perfFile, ",spu_full_waits,spu_read_waits,ipu_idec,ipu_bdec,ipu_out_kb,ipu_stalls,audio_underruns,audio_tempo");
				std::fprintf(perfFile, ",jit_blocks,jit_writes,jit_write_ms,jit_alloc_ms,jit_free_ranges,jit_translate_ms,jit_finish_ms,jit_install_ms,jit_guest_instr,jit_code_kb,jit_opt_ms,jit_alloc_regs_ms,jit_codegen_ms,jit_statements");
				std::fprintf(perfFile, ",gpu_pump_ms,gpu_gl_ms,gpu_tex_ms,gpu_xfer_ms,gpu_disp_ms,present_ms,gpu_draws,gpu_tex_up,gpu_rt_up,gpu_rt_dl,up_gl_ms,up_kpix,dl_gl_ms,dl_kpix,f_targets_new,f_xfer_in,f_xfer_out,f_xfer_local,rt_deferred,rt_skip_rows,xfer_same,idle_intc_gs_t1_ipu_dstat_chcr_vifgif,idle_chcr_d0_x_d1_x_d2_x_d3_d4_d5_d6_d8_d9,vif1_kqw,vif1_empty,vif_vu_waits,vif_vu_giveups,unpack_k_s32_s16_s8_x_v2_32_16_8_x_v3_32_16_8_x_v4_32_16_8_5,unpack_k_masked,unpack_k_mode,unpack_k_cycle,threads\n");
			}
		}
		const uint64_t runStart = sceKernelGetProcessTimeWide();
		uint32_t perfSpuFull = 0, perfSpuRead = 0;
		CIPU::STATS perfIpu;
		VITAJIT_STATS perfJit = VitaJit_GetStats();
		uint64_t perfJitPhases[5] = {EmuProfile::g_jit.translateUs.load(), EmuProfile::g_jit.finishUs.load(), EmuProfile::g_jit.installUs.load(), EmuProfile::g_jit.guestInstructions.load(), EmuProfile::g_jit.codeBytes.load()};
		uint32_t perfIdle[EmuProfile::IDLE_SOURCE_COUNT] = {};
		uint32_t perfIdleChcr[12] = {};
		uint64_t perfVif[4] = {};
		uint32_t perfUnpack[16] = {}, perfUnpackFlags[3] = {};
		bool eeHotMovie = false;
		uint32_t eeHotSeconds = 0;
		CMpegShadow mpegShadow; //movie HLE shadow mode (autostart key mpeg_shadow=1)
		CMpegHle mpegHle;       //movie HLE replace mode (autostart key mpeg_hle=1)
		std::future<bool> stateOperation; //save or load state in progress
		bool stateIsLoad = false;
		bool pendingStateLoad = false; //autostart load_state=1
		unsigned int stateRetries = 0;  //saves refused during an IPU command
		std::string stateMessage;
		uint64_t stateMessageUntil = 0;
		uint64_t perfCodeGen[4] = {Jitter::g_compileStats.optimizeUs.load(), Jitter::g_compileStats.allocateUs.load(), Jitter::g_compileStats.generateUs.load(), Jitter::g_compileStats.statements.load()};
		CGSH_Hardware::GPU_TIMES perfGpu = CGSH_Hardware::GetTimes();
		uint64_t perfPresentUs = g_presentUs;
		// Before anything is compiled for this game.
		CEmuSession::SetSafeJit(settings.safeJit);
		std::printf("safe jit: %s\n", settings.safeJit ? "on" : "off");
		CAutoCycleRate autoRate;
		CAutoTuner::CHOICE tuneStart;
		tuneStart.threadedVu1 = settings.threadedVu1;
		tuneStart.frameSkip = settings.frameSkip;
		CAutoTuner tuner(tuneStart);
		if(!autoTune) tuner.Cancel();
		std::printf("first-boot tuning: %s\n", autoTune ? "on" : (tuned ? "done before" : "off (player settings)"));
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
		// Cores: 0 = EE/IOP (the bottleneck, alone), 1 = UI/GPU, 2 = VU1 +
		// SPU2 audio mixing + audio output.
		config.emuThreadInit = []() { sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x10000 /* SCE_KERNEL_CPU_MASK_USER_0 */); };
		config.vu1ThreadInit = []() { sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x40000 /* SCE_KERNEL_CPU_MASK_USER_2 */); };
		// SPU2 mixing used to share core 1 with the UI/GPU thread: at equal
		// priority a woken mixer waited until that thread blocked, and the
		// register reads and audio DMA that wait for the mixer took ~7ms each
		// (a quarter of the emulation thread during movies). A higher priority
		// fixed that but took the core from the GS (GS waits went 0 -> 14%).
		config.spuThreadInit = []() { sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x40000 /* SCE_KERNEL_CPU_MASK_USER_2 */); };
		config.profilerThreadInit = []() { sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x40000 /* SCE_KERNEL_CPU_MASK_USER_2 */); };
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
			// Code compiled in earlier runs of this game (after SetSafeJit: the
			// code generator's modes are part of the cache's fingerprint).
			if(settings.jitCache)
			{
				auto slash = path.find_last_of('/');
				std::string cachePath = std::string(JIT_CACHE_PATH) + "/" + path.substr(slash + 1) + ".jit";
				bool opened = JitCacheSetup::Open(cachePath, "app0:eboot.bin");
				std::printf("jit cache %s: %s\n", cachePath.c_str(), opened ? JitCacheSetup::Describe().c_str() : "not available");
			}
			session->Boot(path);
			if(settings.movieHle)
			{
				// Native movie decoding (libmpeg 2.2 sceMpegGetPicture), decoder on
				// core 2 (VU1 has little to do while the EE does not decode).
				mpegHle.Enable(session->GetVm()->m_ee->m_ram, "ux0:data/VitaPS2/mpeghle.txt", []() {
					sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x40000 /* SCE_KERNEL_CPU_MASK_USER_2 */);
				});
			}
			// Scripted runs: gs_diag=N logs N flips (buffers, targets) to gsdiag.txt.
			if(gpu && autostart)
			{
				for(const auto& [key, value] : autostart->settings)
				{
					if((key == "load_state") && (value == "1"))
					{
						// Loaded once the machine has run a little (a load right at boot,
						// before the GS and the game are up, crashed).
						pendingStateLoad = true;
						continue;
					}
					if(key == "mpeg_shadow")
					{
						// Silent Hill 2 (SLUS-20228): sceMpeg context at 0x1213080 (docs/HLE_MPEG.md).
						if(value == "1") mpegShadow.Enable(session->GetVm()->m_ee->m_ram, 0x1213080, "ux0:data/VitaPS2/mpegshadow.txt");
						continue;
					}
					if(key != "gs_diag") continue;
					if(auto gs = GetHardwareGs(session.get())) gs->SetDiagnosticsFile("ux0:data/VitaPS2/gsdiag.txt", std::atoi(value.c_str()));
				}
			}
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

		// Hang diagnostics: when the game stops presenting, sample where the
		// EE runs and log the hottest addresses plus IOP/interrupt state.
		uint64_t lastOutput = sceKernelGetProcessTimeWide();
		std::map<uint32_t, uint32_t> stallPcs;
		std::string stallSummary;
		std::string stallDetail; // code around the hot loop and EE registers
		// Display sources per second (GPU renderer): explains black/flashing frames.
		CGSH_Hardware::DISPLAY_STATS displayTotals, displayLastSecond;
		std::string profileLine; // emulation thread time per subsystem

		while(true)
		{
			uint32_t buttons = pad ? pad->Poll() : 0;
			uint32_t pressed = buttons & ~previousButtons;
			previousButtons = buttons;
			if(autostart && autostart->seconds && (sceKernelGetProcessTimeWide() - runStart >= autostart->seconds * 1000000ULL))
			{
				std::printf("autostart: %u seconds done\n", autostart->seconds);
				break;
			}
			if((buttons & SCE_CTRL_SELECT) && (pressed & SCE_CTRL_START))
			{
				uint32_t previousRate = settings.eeCycleRate;
				bool previousVu1 = settings.threadedVu1;
				uint32_t previousSkip = settings.frameSkip;
				bool quit = RunPauseMenu(*session, settings, screen, autoRate);
				// The player's choice wins over first-boot tuning.
				if(!tuner.IsDone() && ((settings.threadedVu1 != previousVu1) || (settings.frameSkip != previousSkip)))
				{
					tuner.Cancel();
					std::printf("first-boot tuning: stopped (player settings)\n");
				}
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
			// Save states: SELECT + R saves, SELECT + TRIANGLE loads (one per game).
			if((buttons & SCE_CTRL_SELECT) && (pressed & (SCE_CTRL_R1 | SCE_CTRL_TRIANGLE)) && !stateOperation.valid())
			{
				stateIsLoad = (pressed & SCE_CTRL_TRIANGLE) != 0;
				stateOperation = stateIsLoad ? session->GetVm()->LoadState(StatePath(path)) : session->GetVm()->SaveState(StatePath(path));
			}
			if(pendingStateLoad && !stateOperation.valid() && (session->GetVmFrameCount() >= 120))
			{
				pendingStateLoad = false;
				stateIsLoad = true;
				stateOperation = session->GetVm()->LoadState(StatePath(path));
			}
			// The VM saves and loads on its own thread; this thread keeps pumping
			// the GS meanwhile (state operations need it).
			if(stateOperation.valid() && (stateOperation.wait_for(std::chrono::seconds(0)) == std::future_status::ready))
			{
				bool ok = stateOperation.get();
				if(!ok && !stateIsLoad && (stateRetries < 120))
				{
					// Refused while the IPU runs a command (movies): try again.
					stateRetries++;
					stateOperation = session->GetVm()->SaveState(StatePath(path));
					continue;
				}
				stateRetries = 0;
				if(ok && stateIsLoad && gpu) GetHardwareGs(session.get())->DiscardGpuCopies();
				stateMessage = std::string(stateIsLoad ? "State loaded" : "State saved") + (ok ? "" : " - FAILED");
				stateMessageUntil = sceKernelGetProcessTimeWide() + 2000000;
				std::printf("%s: %s\n", stateMessage.c_str(), StatePath(path).c_str());
				newContent = true;
			}

			// Cutscenes and movies run without input: keep the Vita from
			// dimming and suspending while a game runs.
			sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

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
				mpegShadow.Poll();
				if(flipped)
				{
					presented++;
					newContent = true;
					lastOutput = sceKernelGetProcessTimeWide();
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
					lastOutput = sceKernelGetProcessTimeWide();
					Gfx::UploadFrame(pixels.data(), width, height);
					screen.texture = Gfx::FrameTexture();
					screen.textureWidth = screen.textureHeight = Gfx::FRAME_TEXTURE_SIZE;
					screen.x = screen.y = 0;
					screen.width = std::min(width, Gfx::FRAME_TEXTURE_SIZE);
					screen.height = std::min(height, Gfx::FRAME_TEXTURE_SIZE);
				}
			}

			uint64_t now = sceKernelGetProcessTimeWide();
			const uint64_t stalledFor = now - lastOutput;
			if(stalledFor > 3000000)
			{
				if(stallPcs.size() < 256) stallPcs[session->GetDebugState().eePc]++;
			}
			else if(!stallPcs.empty() || !stallSummary.empty())
			{
				std::printf("stall over\n");
				stallPcs.clear();
				stallSummary.clear();
				stallDetail.clear();
			}
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
				// Emulation thread time not spent running the EE: compiling, waiting
				// for the GS or the SPU thread. A slower EE does not help there.
				float stallShare = 0.0f;
				{
					auto profile = session->TakeProfile();
					stallShare = profile.share[EmuProfile::SECTION_JIT] + profile.share[EmuProfile::SECTION_GS_WAIT] +
					             profile.share[EmuProfile::SECTION_SPU_SYNC] + profile.share[EmuProfile::SECTION_SPU_READ];
					if(perfFile)
					{
						uint32_t eeRateNow = (settings.eeCycleRate == 0) ? autoRate.GetRate() : settings.eeCycleRate;
						std::fprintf(perfFile, "%.1f,%.2f,%.2f,%.3f,%u", (now - runStart) / 1000000.0, vmFps, presentFps,
						             session->GetEeIdleRatio(), eeRateNow);
						for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++) std::fprintf(perfFile, ",%.3f", profile.share[i]);
						uint32_t full = session->GetVm()->GetSpuFullSyncCount(), read = session->GetVm()->GetSpuReadAccessCount();
						const auto& ipu = CIPU::GetStats();
						std::fprintf(perfFile, ",%u,%u,%u,%u,%u,%u,%u,%.2f,", full - perfSpuFull, read - perfSpuRead,
						             ipu.commands[1] - perfIpu.commands[1], ipu.commands[2] - perfIpu.commands[2],
						             static_cast<unsigned int>((ipu.outBytes - perfIpu.outBytes) / 1024), ipu.outStalls - perfIpu.outStalls,
						             CSH_Vita::GetUnderruns(), CSH_Vita::GetTempo());
						auto jit = VitaJit_GetStats();
						std::fprintf(perfFile, "%u,%u,%.1f,%.1f,%u,", jit.allocs - perfJit.allocs, jit.writes - perfJit.writes,
						             (jit.writeUs - perfJit.writeUs) / 1000.0, (jit.allocUs - perfJit.allocUs) / 1000.0, jit.freeRanges);
						const auto& jitPhases = EmuProfile::g_jit;
						uint64_t jitNow[5] = {jitPhases.translateUs.load(), jitPhases.finishUs.load(), jitPhases.installUs.load(), jitPhases.guestInstructions.load(), jitPhases.codeBytes.load()};
						std::fprintf(perfFile, "%.1f,%.1f,%.1f,%u,%u,", (jitNow[0] - perfJitPhases[0]) / 1000.0, (jitNow[1] - perfJitPhases[1]) / 1000.0, (jitNow[2] - perfJitPhases[2]) / 1000.0,
						             static_cast<unsigned int>(jitNow[3] - perfJitPhases[3]), static_cast<unsigned int>((jitNow[4] - perfJitPhases[4]) / 1024));
						std::copy(std::begin(jitNow), std::end(jitNow), std::begin(perfJitPhases));
						const auto& codeGenStats = Jitter::g_compileStats;
						uint64_t codeGenNow[4] = {codeGenStats.optimizeUs.load(), codeGenStats.allocateUs.load(), codeGenStats.generateUs.load(), codeGenStats.statements.load()};
						std::fprintf(perfFile, "%.1f,%.1f,%.1f,%u,", (codeGenNow[0] - perfCodeGen[0]) / 1000.0, (codeGenNow[1] - perfCodeGen[1]) / 1000.0, (codeGenNow[2] - perfCodeGen[2]) / 1000.0, static_cast<unsigned int>(codeGenNow[3] - perfCodeGen[3]));
						std::copy(std::begin(codeGenNow), std::end(codeGenNow), std::begin(perfCodeGen));
						perfJit = jit;
						if(gpu)
						{
							const auto& times = CGSH_Hardware::GetTimes();
							auto frame = GetHardwareGs(session.get())->GetLastFrameStats();
							std::fprintf(perfFile, "%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%u,%u,%u,%u,", (times.pumpUs - perfGpu.pumpUs) / 1000.0,
							             (times.stageUs[0] - perfGpu.stageUs[0]) / 1000.0, (times.stageUs[1] - perfGpu.stageUs[1]) / 1000.0,
							             (times.stageUs[2] - perfGpu.stageUs[2]) / 1000.0, (times.stageUs[3] - perfGpu.stageUs[3]) / 1000.0,
							             (g_presentUs - perfPresentUs) / 1000.0, frame.drawCalls, frame.textureUploads, frame.targetUploads, frame.targetDownloads);
							std::fprintf(perfFile, "%.1f,%u,%.1f,%u,", (times.uploadGlUs - perfGpu.uploadGlUs) / 1000.0, static_cast<unsigned int>((times.uploadPixels - perfGpu.uploadPixels) / 1000),
							             (times.downloadGlUs - perfGpu.downloadGlUs) / 1000.0, static_cast<unsigned int>((times.downloadPixels - perfGpu.downloadPixels) / 1000));
							std::fprintf(perfFile, "%u,%u,%u,%u,%u,%u,%u,", frame.targetsCreated, frame.transfersIn, frame.transfersOut, frame.transfersLocal,
							             frame.deferredUploads, frame.skippedUploadRows, frame.unchangedTransfers);
							perfGpu = times;
							perfPresentUs = g_presentUs;
						}
						else
						{
							std::fprintf(perfFile, ",,,,,,,,,,,,,,,,,,");
						}
						perfSpuFull = full;
						perfSpuRead = read;
						// EE hotspots of MPEG playback: from the first second with
						// BDEC activity on, written every 10 seconds.
						if(!eeHotMovie && (ipu.commands[2] != perfIpu.commands[2]))
						{
							eeHotMovie = true;
							session->ResetEeHotspots();
						}
						if(eeHotMovie && ((++eeHotSeconds % 10) == 0))
						{
							if(FILE* hot = std::fopen("ux0:data/VitaPS2/eehot.txt", "w"))
							{
								session->WriteEeHotspots(hot, 80);
								std::fclose(hot);
							}
						}
						// The code behind the hotspots, for disassembly on a PC.
						if(eeHotMovie && (eeHotSeconds == 30) && autostart && (autostart->settings.size() != 0))
						{
							for(const auto& [key, value] : autostart->settings)
							{
								if((key != "dump_ee_ram") || (value != "1")) continue;
								if(FILE* dump = std::fopen("ux0:data/VitaPS2/eeram.bin", "wb"))
								{
									std::fwrite(session->GetVm()->m_ee->m_ram, 1, PS2::EE_RAM_SIZE, dump);
									std::fclose(dump);
								}
								// The MPEG input path at the same moment: DMA4 (to IPU),
								// DMA3 (from IPU) and the IPU registers.
								if(FILE* regs = std::fopen("ux0:data/VitaPS2/eeregs.txt", "w"))
								{
									auto& ee = *session->GetVm()->m_ee;
									static const uint32_t dmacRegs[] = {0x1000B000, 0x1000B010, 0x1000B020, 0x1000B030, 0x1000B400, 0x1000B410, 0x1000B420, 0x1000B430, 0x1000E010};
									static const char* dmacNames[] = {"D3_CHCR", "D3_MADR", "D3_QWC", "D3_TADR", "D4_CHCR", "D4_MADR", "D4_QWC", "D4_TADR", "D_STAT"};
									for(unsigned int i = 0; i < 9; i++) std::fprintf(regs, "%s %08X\n", dmacNames[i], ee.m_dmac.GetRegister(dmacRegs[i]));
									static const uint32_t ipuRegs[] = {0x10002000, 0x10002004, 0x10002010, 0x10002020, 0x10002030};
									static const char* ipuNames[] = {"IPU_CMD", "IPU_CMD_HI", "IPU_CTRL", "IPU_BP", "IPU_TOP"};
									for(unsigned int i = 0; i < 5; i++) std::fprintf(regs, "%s %08X\n", ipuNames[i], ee.m_ipu.GetRegister(ipuRegs[i]));
									std::fprintf(regs, "EE_PC %08X\n", ee.m_EE.m_State.nPC);
									std::fclose(regs);
								}
							}
						}
						perfIpu = ipu;
						for(unsigned int i = 0; i < EmuProfile::IDLE_SOURCE_COUNT; i++)
						{
							uint32_t count = EmuProfile::g_idleTriggers[i].load();
							std::fprintf(perfFile, "%u%s", count - perfIdle[i], (i + 1 < EmuProfile::IDLE_SOURCE_COUNT) ? ";" : ",");
							perfIdle[i] = count;
						}
						for(unsigned int i = 0; i < 12; i++)
						{
							uint32_t count = EmuProfile::g_idleChcr[i].load();
							std::fprintf(perfFile, "%u%s", count - perfIdleChcr[i], (i + 1 < 12) ? ";" : ",");
							perfIdleChcr[i] = count;
						}
						{
							const auto& vif = EmuProfile::g_vif;
							uint64_t vifNow[4] = {vif.vif1Qw.load(), vif.vif1Empty.load(), vif.vewStalls.load(), vif.vewGiveUps.load()};
							std::fprintf(perfFile, "%.1f,%u,%u,%u,", (vifNow[0] - perfVif[0]) / 1000.0, static_cast<unsigned int>(vifNow[1] - perfVif[1]),
							             static_cast<unsigned int>(vifNow[2] - perfVif[2]), static_cast<unsigned int>(vifNow[3] - perfVif[3]));
							std::copy(std::begin(vifNow), std::end(vifNow), std::begin(perfVif));
							for(unsigned int i = 0; i < 16; i++)
							{
								uint32_t count = EmuProfile::g_unpackValues[i].load();
								std::fprintf(perfFile, "%.1f%s", (count - perfUnpack[i]) / 1000.0, (i + 1 < 16) ? ";" : ",");
								perfUnpack[i] = count;
							}
							uint32_t flags[3] = {EmuProfile::g_unpackMasked.load(), EmuProfile::g_unpackMode.load(), EmuProfile::g_unpackCycle.load()};
							std::fprintf(perfFile, "%.1f,%.1f,%.1f,", (flags[0] - perfUnpackFlags[0]) / 1000.0, (flags[1] - perfUnpackFlags[1]) / 1000.0,
							             (flags[2] - perfUnpackFlags[2]) / 1000.0);
							std::copy(std::begin(flags), std::end(flags), std::begin(perfUnpackFlags));
						}
						for(const auto& usage : threadUsage) std::fprintf(perfFile, "%s=%.1f;", usage.name.c_str(), usage.cpuPercent);
						std::fprintf(perfFile, "\n");
						std::fflush(perfFile);
					}
					// Largest first.
					unsigned int order[EmuProfile::SECTION_COUNT];
					for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++) order[i] = i;
					std::sort(order, order + EmuProfile::SECTION_COUNT,
					          [&](unsigned int a, unsigned int b) { return profile.share[a] > profile.share[b]; });
					char text[160];
					int length = std::snprintf(text, sizeof(text), "PS2 thread:");
					for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT && length < 140; i++)
					{
						float share = profile.share[order[i]];
						if(share < 0.01f) break;
						length += std::snprintf(text + length, sizeof(text) - length, " %s %.0f%%", EmuProfile::GetSectionName(order[i]), share * 100.0f);
					}
					profileLine = text;
					std::printf("%s\n", text);
				}
				if(gpu)
				{
					auto totals = GetHardwareGs(session.get())->GetDisplayStats();
					displayLastSecond = totals;
					displayLastSecond.fromTarget -= displayTotals.fromTarget;
					displayLastSecond.fromMemory -= displayTotals.fromMemory;
					displayLastSecond.skipped -= displayTotals.skipped;
					displayLastSecond.disabled -= displayTotals.disabled;
					displayTotals = totals;
					std::printf("display: target %u memory %u skipped %u off %u  buf %05X w %u psm %X y %u\n", displayLastSecond.fromTarget,
					            displayLastSecond.fromMemory, displayLastSecond.skipped, displayLastSecond.disabled, displayLastSecond.bufPtr,
					            displayLastSecond.bufWidth, displayLastSecond.psm, displayLastSecond.offsetY);
				}
				for(const auto& usage : threadUsage)
				{
					std::printf("cpu %-18s %5.1f%%\n", usage.name.c_str(), usage.cpuPercent);
				}
				float eeIdle = session->GetEeIdleRatio();
				std::printf("fps vm %.1f out %.1f ee-idle %.0f%%\n", vmFps, presentFps, eeIdle * 100.0f);
				if((settings.eeCycleRate == 0) && autoRate.Update(eeIdle, vmFps, targetFps, stallShare))
				{
					std::printf("auto ee cycle rate: %u%%\n", autoRate.GetRate());
					session->SetSpeedHacks(ToSpeedHacks(settings, autoRate));
				}
				if(!tuner.IsDone())
				{
					if(tuner.Update(vmFps, targetFps, stallShare))
					{
						settings.threadedVu1 = tuner.GetChoice().threadedVu1;
						settings.frameSkip = tuner.GetChoice().frameSkip;
						std::printf("first-boot tuning: trying threaded_vu1=%d frame_skip=%u\n", settings.threadedVu1 ? 1 : 0, settings.frameSkip);
						session->SetSpeedHacks(ToSpeedHacks(settings, autoRate));
					}
					if(tuner.IsDone())
					{
						SaveAutoTune(path, tuner);
						// A settings file saved before tuning finished holds the old values.
						if(std::ifstream(SettingsPathFor(path))) SaveSettings(path, settings);
						std::printf("first-boot tuning: done, threaded_vu1=%d frame_skip=%u (%.1f fps)\n", settings.threadedVu1 ? 1 : 0,
						            settings.frameSkip, tuner.GetFinalFps());
					}
				}
				if(!stallPcs.empty())
				{
					std::vector<std::pair<uint32_t, uint32_t>> hottest(stallPcs.begin(), stallPcs.end());
					std::sort(hottest.begin(), hottest.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
					auto debug = session->GetDebugState();
					char text[320];
					int length = std::snprintf(text, sizeof(text), "no output %us: EE pc %08X ra %08X  IOP pc %08X ra %08X thr %d  INTC %X/%X  DMAC %08X  VU1 %04X pc %04X %ums  EE hot:",
					                           static_cast<unsigned int>(stalledFor / 1000000), debug.eePc, debug.eeRa, debug.iopPc,
					                           debug.iopRa, debug.iopThread, debug.intcStat, debug.intcMask, debug.dmacStat,
					                           debug.vu1Start, debug.vu1Pc, debug.vu1RunMs);
					for(size_t i = 0; i < std::min<size_t>(hottest.size(), 4) && length < 290; i++)
					{
						length += std::snprintf(text + length, sizeof(text) - length, " %08X(%u)", hottest[i].first, hottest[i].second);
					}
					stallSummary = text;
					std::printf("%s\n", text);
					// Code of the loop the EE spins in (hot addresses near the
					// hottest one) and the EE registers, to see what it waits for.
					uint32_t hot = hottest[0].first, lo = hot, hi = hot;
					for(size_t i = 1; i < std::min<size_t>(hottest.size(), 6); i++)
					{
						uint32_t pc = hottest[i].first;
						if((pc + 0x100 > hot) && (pc < hot + 0x100))
						{
							lo = std::min(lo, pc);
							hi = std::max(hi, pc);
						}
					}
					lo = (lo - 0x10) & ~0x1Fu;
					hi = std::min(hi + 0x14, lo + 24 * 4 - 4);
					std::string detail;
					char line[128];
					for(uint32_t address = lo; address <= hi; address += 32)
					{
						int n = std::snprintf(line, sizeof(line), "%08X:", address);
						for(uint32_t a = address; (a < address + 32) && (a <= hi); a += 4)
							n += std::snprintf(line + n, sizeof(line) - n, " %08X", session->ReadEeWord(a));
						detail += line;
						detail += "\n";
					}
					static const char* names[4] = {"r0-7  ", "r8-15 ", "r16-23", "r24-31"};
					for(int row = 0; row < 4; row++)
					{
						int n = std::snprintf(line, sizeof(line), "%s:", names[row]);
						for(int r = row * 8; r < row * 8 + 8; r++)
							n += std::snprintf(line + n, sizeof(line) - n, " %08X", debug.eeGpr[r]);
						detail += line;
						detail += "\n";
					}
					std::snprintf(line, sizeof(line), "IPU ctrl %08X bp %08X  D4(to IPU) %X madr %08X qwc %X  D3(from) %X qwc %X", debug.ipuCtrl,
					              debug.ipuBp, debug.d4Chcr, debug.d4Madr, debug.d4Qwc, debug.d3Chcr, debug.d3Qwc);
					detail += line;
					detail += "\n";
					for(const auto& warning : CLog::GetInstance().GetRecentWarnings())
					{
						detail += warning.substr(0, 100);
						detail += "\n";
					}
					stallDetail = detail;
					std::printf("%s", detail.c_str());
					stallPcs.clear();
				}
				newContent |= settings.showStats;
			}

			// Redrawing an unchanged screen costs GPU time the GS needs.
			if(!newContent && (now >= stateMessageUntil) && (now - lastPresent < 100000))
			{
				if(!gpu) sceKernelDelayThread(2000); //the GPU path waits in Pump()
				continue;
			}
			newContent = false;
			lastPresent = now;

			const uint64_t presentStart = sceKernelGetProcessTimeWide();
			BeginFrame();
			DrawScreen(screen, settings.stretch);
			if(now < stateMessageUntil)
			{
				Gfx::Rect(0, SCREEN_HEIGHT - 40, 360, 40, Gfx::Rgba(0, 0, 0, 180));
				Gfx::Text(12, SCREEN_HEIGHT - 14, COLOR_WHITE, 0.8f, stateMessage.c_str());
			}
			if(settings.showStats)
			{
				int lines = 7 + static_cast<int>(threadUsage.size());
				Gfx::Rect(0, 0, 640, 10 + lines * 20, Gfx::Rgba(0, 0, 0, 160));
				std::string ipuLine;
				Gfx::Textf(8, 20, COLOR_WHITE, 0.7f, "VM %.1f fps  out %.1f fps  %ux%u  " VITAPS2_BUILD, vmFps, presentFps, screen.width, screen.height);
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
				{
					//SPU thread waits per second (full syncs / register reads waiting on the mixer)
					static uint64_t spuRateTime = 0;
					static uint32_t spuLastFull = 0, spuLastRead = 0, spuFullRate = 0, spuReadRate = 0;
					uint64_t now = sceKernelGetProcessTimeWide();
					uint32_t fullSyncs = session->GetVm()->GetSpuFullSyncCount();
					uint32_t readWaits = session->GetVm()->GetSpuReadAccessCount();
					if(now - spuRateTime >= 1000000)
					{
						spuFullRate = fullSyncs - spuLastFull;
						spuReadRate = readWaits - spuLastRead;
						spuLastFull = fullSyncs;
						spuLastRead = readWaits;
						spuRateTime = now;
					}
					Gfx::Textf(8, 80, COLOR_GREY, 0.7f, "Audio tempo %.0f%%  underruns %u  spu waits/s %u+%u", CSH_Vita::GetTempo() * 100.0f,
					           CSH_Vita::GetUnderruns(), spuFullRate, spuReadRate);
				}
				{
					//IPU (movie decoder) activity per second: commands, output, DMA3 stalls
					static uint64_t ipuRateTime = 0;
					static CIPU::STATS ipuLast, ipuRate;
					const auto& ipuNow = CIPU::GetStats();
					uint64_t now = sceKernelGetProcessTimeWide();
					if(now - ipuRateTime >= 1000000)
					{
						for(int c = 0; c < 16; c++) ipuRate.commands[c] = ipuNow.commands[c] - ipuLast.commands[c];
						ipuRate.outBytes = ipuNow.outBytes - ipuLast.outBytes;
						ipuRate.outStalls = ipuNow.outStalls - ipuLast.outStalls;
						ipuLast = ipuNow;
						ipuRateTime = now;
					}
					if(ipuRate.outBytes || ipuRate.commands[1] || ipuRate.commands[2] || ipuRate.commands[3])
					{
						char text[160];
						std::snprintf(text, sizeof(text), "IPU/s: idec %u bdec %u vdec %u fdec %u csc %u  out %u KB  stalls %u",
						              ipuRate.commands[1], ipuRate.commands[2], ipuRate.commands[3], ipuRate.commands[4],
						              ipuRate.commands[7], static_cast<unsigned int>(ipuRate.outBytes / 1024), ipuRate.outStalls);
						ipuLine = text;
					}
				}
				if(gpu)
				{
					const auto& d = displayLastSecond;
					Gfx::Textf(8, 100, COLOR_GREY, 0.7f, "Display/s: gpu %u  mem %u  skip %u  off %u  buf %05X w%u psm%X y%u", d.fromTarget,
					           d.fromMemory, d.skipped, d.disabled, d.bufPtr, d.bufWidth, d.psm, d.offsetY);
				}
				if(!stallSummary.empty())
				{
					// Long line: wrap it under the overlay.
					std::string wrapped = stallSummary;
					for(size_t at = 90; at < wrapped.size(); at += 91) wrapped.insert(at, "\n");
					Gfx::Text(8, 290, COLOR_WARN, 0.6f, wrapped.c_str());
					Gfx::Text(8, 345, COLOR_WARN, 0.6f, stallDetail.c_str());
				}
				Gfx::Text(8, 120, COLOR_ACCENT, 0.7f, profileLine.c_str());
				Gfx::Text(8, 140, COLOR_ACCENT, 0.7f, "CPU per thread (100% = one core):");
				int line = 0;
				for(const auto& usage : threadUsage)
				{
					uint32_t color = (usage.cpuPercent > 90.0f) ? COLOR_WARN : COLOR_GREY;
					Gfx::Textf(16, 160 + line * 20, color, 0.7f, "%-18s %5.1f%%", usage.name.c_str(), usage.cpuPercent);
					line++;
				}
				if(!ipuLine.empty())
				{
					Gfx::Text(8, 160 + line * 20, COLOR_ACCENT, 0.7f, ipuLine.c_str());
				}
			}
			EndFrame();
			g_presentUs += sceKernelGetProcessTimeWide() - presentStart;
		}

		mpegHle.Disable();
		if(settings.jitCache)
		{
			std::printf("%s\n", JitCacheSetup::Describe().c_str());
			JitCacheSetup::Close();
		}
		mpegShadow.Disable();
		session.reset();
		sessionPtr = nullptr;
		if(perfFile) std::fclose(perfFile);
	}

	bool ReadAutostart(AUTOSTART& autostart)
	{
		std::ifstream file(AUTOSTART_PATH);
		if(!file) return false;
		std::string line;
		while(std::getline(file, line))
		{
			while(!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
			auto eq = line.find('=');
			if(eq == std::string::npos) continue;
			std::string key = line.substr(0, eq), value = line.substr(eq + 1);
			if(key == "game") autostart.game = value;
			else if(key == "seconds") autostart.seconds = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
			else autostart.settings.emplace_back(key, value);
		}
		file.close();
		sceIoRemove(AUTOSTART_DONE_PATH);
		sceIoRename(AUTOSTART_PATH, AUTOSTART_DONE_PATH);
		return !autostart.game.empty();
	}
}

int main()
{
	sceIoMkdir(DATA_PATH, 0777);
	// Everything printed (ours and Play!'s) goes to a log file for bug reports.
	if(std::freopen(LOG_PATH, "w", stdout)) setvbuf(stdout, nullptr, _IOLBF, 0);
	if(std::freopen(LOG_PATH, "a", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
	std::printf("VitaPS2 %s starting\n", VITAPS2_BUILD);
	// Aborts (uncaught exceptions) leave no crash dump: record why.
	std::set_terminate([]() {
		if(FILE* file = std::fopen("ux0:data/VitaPS2/crash.txt", "w"))
		{
			const char* what = "no exception";
			if(auto exception = std::current_exception())
			{
				try
				{
					std::rethrow_exception(exception);
				}
				catch(const std::exception& e)
				{
					what = e.what();
				}
				catch(...)
				{
					what = "unknown exception type";
				}
			}
			std::fprintf(file, "VitaPS2 %s terminate: %s\n", VITAPS2_BUILD, what);
			std::fclose(file);
		}
		std::abort();
	});
	ThreadProfiler::RegisterCurrentThread("UI/GPU");
	// Core 1: keep core 0 free for the EE/IOP thread (the bottleneck).
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x20000 /* SCE_KERNEL_CPU_MASK_USER_1 */);

	// Maximum clocks. Only raise them: an overclocking plugin (PSVshell,
	// LOLIcon) may already run the CPU at 500 MHz, which must be kept.
	if(scePowerGetArmClockFrequency() < 444) scePowerSetArmClockFrequency(444);
	if(scePowerGetBusClockFrequency() < 222) scePowerSetBusClockFrequency(222);
	if(scePowerGetGpuClockFrequency() < 222) scePowerSetGpuClockFrequency(222);
	if(scePowerGetGpuXbarClockFrequency() < 166) scePowerSetGpuXbarClockFrequency(166);
	std::printf("clocks: cpu %d MHz, bus %d, gpu %d, xbar %d\n", scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
	            scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

	sceIoMkdir(GAMES_PATH, 0777);
	sceIoMkdir(SETTINGS_PATH, 0777);
	sceIoMkdir(STATES_PATH, 0777);
	sceIoMkdir(JIT_CACHE_PATH, 0777);

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
	Theme::Init();

	try
	{
		CEmuSession::SetDataPaths(DATA_PATH, "app0:");
	}
	catch(const std::exception& e)
	{
		ShowMessage("Initialization failed", e.what());
	}

	{
		AUTOSTART autostart;
		if(ReadAutostart(autostart))
		{
			std::printf("autostart: %s for %u s\n", autostart.game.c_str(), autostart.seconds);
			RunGame(autostart.game, &autostart);
			if(autostart.seconds)
			{
				Gfx::Shutdown();
				sceKernelExitProcess(0);
				return 0;
			}
		}
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
