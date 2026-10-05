// VitaPS2 - PlayStation 2 emulator frontend for the PlayStation Vita.
//
// Games (.iso/.cso/.chd/.isz/.cue/.mds/.bin) and homebrew (.elf) are read from
// ux0:data/VitaPS2/games. Configuration, memory cards and logs live in
// ux0:data/VitaPS2.
//
// In game:  SELECT + START  -> pause menu (speed hacks, display, quit)
//           SELECT + L      -> toggle the performance overlay
//
// Settings are remembered per game in ux0:data/VitaPS2/settings.

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
#include <psp2/power.h>
#include <vita2d.h>

#include "EmuSession.h"
#include "GsBenchmark.h"
#include "JitMemory.h"
#include "PH_Vita.h"
#include "SH_Vita.h"

// Memory layout: the newlib heap holds the emulated PS2 (EE/IOP RAM, VU, GS
// RAM, recompiler tables). The JIT code pool is a separate VM block. Textures
// live in CDRAM through vita2d. Requires ATTRIBUTE2=12 in param.sfo.
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

	constexpr unsigned int COLOR_WHITE = RGBA8(255, 255, 255, 255);
	constexpr unsigned int COLOR_GREY = RGBA8(150, 150, 160, 255);
	constexpr unsigned int COLOR_ACCENT = RGBA8(90, 160, 255, 255);
	constexpr unsigned int COLOR_BG = RGBA8(16, 18, 28, 255);

	vita2d_pgf* g_font = nullptr;

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
			vita2d_start_drawing();
			vita2d_clear_screen();
			vita2d_pgf_draw_text(g_font, 40, 70, COLOR_ACCENT, 1.4f, title.c_str());
			vita2d_pgf_draw_text(g_font, 40, 130, COLOR_WHITE, 1.0f, body.c_str());
			vita2d_pgf_draw_text(g_font, 40, 510, COLOR_GREY, 0.9f, "Press X to continue");
			vita2d_end_drawing();
			vita2d_swap_buffers();
		}
	}

	struct GAME_SETTINGS
	{
		uint32_t eeCycleRate = 100;
		bool interlaced = false;
		uint32_t frameSkip = 0;
		bool showStats = true;
		bool stretch = false; //fill the 16:9 screen instead of 4:3
	};

	std::string SettingsPathFor(const std::string& gamePath)
	{
		auto slash = gamePath.find_last_of('/');
		return std::string(SETTINGS_PATH) + "/" + gamePath.substr(slash + 1) + ".ini";
	}

	GAME_SETTINGS LoadSettings(const std::string& gamePath)
	{
		GAME_SETTINGS settings;
		std::ifstream file(SettingsPathFor(gamePath));
		std::string line;
		while(std::getline(file, line))
		{
			auto eq = line.find('=');
			if(eq == std::string::npos) continue;
			std::string key = line.substr(0, eq);
			uint32_t value = static_cast<uint32_t>(std::strtoul(line.c_str() + eq + 1, nullptr, 10));
			if(key == "ee_cycle_rate") settings.eeCycleRate = value;
			else if(key == "interlaced") settings.interlaced = value != 0;
			else if(key == "frame_skip") settings.frameSkip = value;
			else if(key == "show_stats") settings.showStats = value != 0;
			else if(key == "stretch") settings.stretch = value != 0;
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
		     << "stretch=" << (settings.stretch ? 1 : 0) << "\n";
	}

	CEmuSession::SPEED_HACKS ToSpeedHacks(const GAME_SETTINGS& settings)
	{
		CEmuSession::SPEED_HACKS hacks;
		hacks.eeCycleRatePercent = settings.eeCycleRate;
		hacks.interlacedRendering = settings.interlaced;
		hacks.frameSkip = settings.frameSkip;
		return hacks;
	}

	void DrawScreen(vita2d_texture* screen, uint32_t width, uint32_t height, bool stretch)
	{
		if(width == 0 || height == 0) return;
		float dstH = 544.0f;
		float dstW = stretch ? 960.0f : dstH * 4.0f / 3.0f;
		vita2d_draw_texture_part_scale(screen, (960.0f - dstW) / 2.0f, 0, 0, 0, width, height, dstW / width, dstH / height);
	}

	// Returns true if the player chose to quit to the game list.
	bool RunPauseMenu(CEmuSession& session, GAME_SETTINGS& settings, vita2d_texture* screen, uint32_t width, uint32_t height)
	{
		static const uint32_t eeRates[] = {50, 60, 75, 90, 100, 130};
		enum ITEM
		{
			ITEM_RESUME,
			ITEM_EE_RATE,
			ITEM_INTERLACED,
			ITEM_FRAMESKIP,
			ITEM_STRETCH,
			ITEM_STATS,
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
					for(int i = 0; i < 6; i++)
						if(eeRates[i] == settings.eeCycleRate) index = i;
					index = std::clamp(index + delta, 0, 5);
					settings.eeCycleRate = eeRates[index];
				}
				break;
				case ITEM_INTERLACED: settings.interlaced = !settings.interlaced; break;
				case ITEM_FRAMESKIP: settings.frameSkip = static_cast<uint32_t>(std::clamp<int>(static_cast<int>(settings.frameSkip) + delta, 0, 3)); break;
				case ITEM_STRETCH: settings.stretch = !settings.stretch; break;
				case ITEM_STATS: settings.showStats = !settings.showStats; break;
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
				std::snprintf(lines[ITEM_EE_RATE], 96, "EE cycle rate: %u%%  (lower = faster, may slow game logic)", settings.eeCycleRate);
				std::snprintf(lines[ITEM_INTERLACED], 96, "Interlaced rendering: %s  (half the GS work)", settings.interlaced ? "On" : "Off");
				std::snprintf(lines[ITEM_FRAMESKIP], 96, "Frame skip: %u", settings.frameSkip);
				std::snprintf(lines[ITEM_STRETCH], 96, "Aspect: %s", settings.stretch ? "Stretch 16:9" : "4:3");
				std::snprintf(lines[ITEM_STATS], 96, "Performance overlay: %s", settings.showStats ? "On" : "Off");
				std::snprintf(lines[ITEM_QUIT], 96, "Quit to game list");

				vita2d_start_drawing();
				vita2d_clear_screen();
				DrawScreen(screen, width, height, settings.stretch);
				vita2d_draw_rectangle(120, 90, 720, 360, RGBA8(10, 12, 24, 220));
				vita2d_pgf_draw_text(g_font, 150, 130, COLOR_ACCENT, 1.2f, "Paused");
				for(int i = 0; i < ITEM_COUNT; i++)
				{
					int y = 180 + i * 34;
					if(i == selected) vita2d_draw_rectangle(140, y - 24, 680, 32, RGBA8(40, 60, 110, 255));
					vita2d_pgf_draw_text(g_font, 155, y, (i == selected) ? COLOR_WHITE : COLOR_GREY, 0.95f, lines[i]);
				}
				vita2d_pgf_draw_text(g_font, 150, 435, COLOR_GREY, 0.8f, "LEFT/RIGHT: change   X: select   O: resume");
				vita2d_end_drawing();
				vita2d_swap_buffers();
			}
		}
	done:
		session.SetSpeedHacks(ToSpeedHacks(settings));
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

	// Runs the renderer benchmark on the device and shows/saves the results.
	void RunBenchmarkScreen()
	{
		std::vector<std::string> lines;
		auto draw = [&](const char* status) {
			vita2d_start_drawing();
			vita2d_clear_screen();
			vita2d_pgf_draw_text(g_font, 30, 45, COLOR_ACCENT, 1.3f, "GS benchmark (Mpix/s)");
			for(size_t i = 0; i < lines.size(); i++)
			{
				vita2d_pgf_draw_text(g_font, 30, 90 + static_cast<int>(i) * 28, COLOR_WHITE, 0.9f, lines[i].c_str());
			}
			vita2d_pgf_draw_text(g_font, 30, 520, COLOR_GREY, 0.9f, status);
			vita2d_end_drawing();
			vita2d_swap_buffers();
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
		const int visible = 16;
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

			vita2d_start_drawing();
			vita2d_clear_screen();
			vita2d_pgf_draw_text(g_font, 30, 45, COLOR_ACCENT, 1.5f, "VitaPS2");
			vita2d_pgf_draw_text(g_font, 190, 45, COLOR_GREY, 0.9f, "PlayStation 2 emulator (experimental)");
			if(games.empty())
			{
				vita2d_pgf_draw_textf(g_font, 30, 110, COLOR_WHITE, 1.0f, "No games found. Copy .iso/.cso/.chd/.elf files to\n%s", GAMES_PATH);
			}
			for(int i = scroll; i < std::min(count, scroll + visible); i++)
			{
				int y = 95 + (i - scroll) * 26;
				if(i == selected)
				{
					vita2d_draw_rectangle(20, y - 20, 920, 26, RGBA8(40, 60, 110, 255));
				}
				vita2d_pgf_draw_text(g_font, 30, y, (i == selected) ? COLOR_WHITE : COLOR_GREY, 1.0f, games[i].c_str());
			}
			vita2d_pgf_draw_text(g_font, 30, 530, COLOR_GREY, 0.85f,
			                     "X: boot  TRIANGLE: refresh  SELECT: GS benchmark  START: quit  |  In game: SELECT+START menu");
			vita2d_end_drawing();
			vita2d_swap_buffers();
		}
	}

	void RunGame(const std::string& path)
	{
		CPH_Vita* pad = nullptr;
		CEmuSession::CONFIG config;
		config.dataPath = DATA_PATH;
		config.resourcesPath = "app0:";
		config.limitFrameRate = true;
		config.gsThreaded = true;
		// Vita: 3 cores for apps. Core usage: EE/IOP/VU (Play! VM thread), GS
		// thread, one extra rasterizer worker.
		config.rasterizerThreads = 2;
		config.padFactory = CPH_Vita::GetFactoryFunction(&pad);
		config.soundFactory = &CSH_Vita::HandlerFactory;

		GAME_SETTINGS settings = LoadSettings(path);
		config.interlacedRendering = settings.interlaced;
		config.frameSkip = settings.frameSkip;

		std::unique_ptr<CEmuSession> session;
		try
		{
			session = std::make_unique<CEmuSession>(config);
			auto hacks = ToSpeedHacks(settings);
			session->SetSpeedHacks(hacks);
			session->Boot(path);
		}
		catch(const std::exception& e)
		{
			ShowMessage("Failed to boot", e.what());
			return;
		}

		vita2d_texture* screen = vita2d_create_empty_texture(1024, 1024);
		vita2d_texture_set_filters(screen, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
		uint32_t stride = vita2d_texture_get_stride(screen) / 4;
		auto texels = reinterpret_cast<uint32_t*>(vita2d_texture_get_datap(screen));

		std::vector<uint32_t> pixels;
		uint32_t width = 0, height = 0;
		uint64_t serial = 0;
		uint32_t previousButtons = ~0u;

		uint64_t statsTime = sceKernelGetProcessTimeWide();
		uint64_t statsVmFrames = 0, statsPresented = 0, presented = 0;
		float vmFps = 0, presentFps = 0;

		while(true)
		{
			uint32_t buttons = pad ? pad->Poll() : 0;
			uint32_t pressed = buttons & ~previousButtons;
			previousButtons = buttons;
			if((buttons & SCE_CTRL_SELECT) && (pressed & SCE_CTRL_START))
			{
				bool quit = RunPauseMenu(*session, settings, screen, width, height);
				SaveSettings(path, settings);
				if(quit) break;
				previousButtons = ~0u;
				continue;
			}
			if((buttons & SCE_CTRL_SELECT) && (pressed & SCE_CTRL_L1))
			{
				settings.showStats = !settings.showStats;
				SaveSettings(path, settings);
			}

			if(session->GetFrames().Fetch(serial, pixels, width, height))
			{
				presented++;
				width = std::min<uint32_t>(width, 1024);
				height = std::min<uint32_t>(height, 1024);
				for(uint32_t y = 0; y < height; y++)
				{
					std::memcpy(texels + y * stride, pixels.data() + y * width, width * 4);
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
			}

			vita2d_start_drawing();
			vita2d_clear_screen();
			DrawScreen(screen, width, height, settings.stretch);
			if(settings.showStats)
			{
				vita2d_draw_rectangle(0, 0, 360, 82, RGBA8(0, 0, 0, 160));
				vita2d_pgf_draw_textf(g_font, 8, 22, COLOR_WHITE, 0.8f, "VM %.1f fps  out %.1f fps  %ux%u",
				                      vmFps, presentFps, width, height);
				vita2d_pgf_draw_textf(g_font, 8, 46, COLOR_GREY, 0.8f, "GS %.1f ms/frame  EE idle %.0f%%",
				                      session->GetGsRasterMicros() / 1000.0f, session->GetEeIdleRatio() * 100.0f);
				vita2d_pgf_draw_textf(g_font, 8, 70, COLOR_GREY, 0.8f, "JIT %u/%u KB",
				                      static_cast<unsigned int>(VitaJit_GetUsedBytes() / 1024),
				                      static_cast<unsigned int>(VitaJit_GetCapacity() / 1024));
			}
			vita2d_end_drawing();
			vita2d_swap_buffers();
		}

		session.reset();
		vita2d_wait_rendering_done();
		vita2d_free_texture(screen);
	}
}

int main()
{
	sceIoMkdir(DATA_PATH, 0777);
	// Everything printed (ours and Play!'s) goes to a log file for bug reports.
	if(std::freopen(LOG_PATH, "w", stdout)) setvbuf(stdout, nullptr, _IOLBF, 0);
	if(std::freopen(LOG_PATH, "a", stderr)) setvbuf(stderr, nullptr, _IONBF, 0);
	std::printf("VitaPS2 starting\n");

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

	vita2d_init();
	vita2d_set_clear_color(COLOR_BG);
	g_font = vita2d_load_default_pgf();

	sceIoMkdir(DATA_PATH, 0777);
	sceIoMkdir(GAMES_PATH, 0777);
	sceIoMkdir(SETTINGS_PATH, 0777);

	if(!VitaJit_Init(JIT_POOL_MIN, JIT_POOL_MAX))
	{
		std::string details = VitaJit_GetDiagnostics() +
		                      "\nIf the error is 0x80020??? check that \"Unsafe Homebrew\" is enabled\n"
		                      "in HENkaku Settings. Please report this screen with log.txt.";
		ShowMessage("Cannot allocate JIT memory", details);
		vita2d_fini();
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

	vita2d_free_pgf(g_font);
	vita2d_fini();
	sceKernelExitProcess(0);
	return 0;
}
