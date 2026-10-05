// VitaPS2 - PlayStation 2 emulator frontend for the PlayStation Vita.
//
// Games (.iso/.cso/.chd/.isz/.cue/.mds/.bin) and homebrew (.elf) are read from
// ux0:data/VitaPS2/games. Configuration, memory cards and logs live in
// ux0:data/VitaPS2.
//
// In game:  SELECT + START  -> back to the game list
//           SELECT + L      -> toggle the performance overlay

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <vector>

#include <psp2/ctrl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include <vita2d.h>

#include "EmuSession.h"
#include "JitMemory.h"
#include "PH_Vita.h"
#include "SH_Vita.h"

// Memory layout: the newlib heap holds the emulated PS2 (EE/IOP RAM, VU, GS
// RAM, recompiler tables). The JIT code pool is a separate VM block. Textures
// live in CDRAM through vita2d. Requires ATTRIBUTE2=12 in param.sfo.
extern "C"
{
	int _newlib_heap_size_user = 288 * 1024 * 1024;
	unsigned int sceUserMainThreadStackSize = 1 * 1024 * 1024;
}

namespace
{
	constexpr const char* DATA_PATH = "ux0:data/VitaPS2";
	constexpr const char* GAMES_PATH = "ux0:data/VitaPS2/games";
	constexpr size_t JIT_POOL_SIZE = 40 * 1024 * 1024;

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
		return games;
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
				if(input.pressed & SCE_CTRL_CROSS) return std::string(GAMES_PATH) + "/" + games[selected];
			}
			if(input.pressed & SCE_CTRL_TRIANGLE)
			{
				games = ScanGames();
				selected = 0;
			}
			if(input.pressed & SCE_CTRL_START) return std::string();

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
			                     "X: boot   TRIANGLE: refresh   START: quit   In game: SELECT+START = menu, SELECT+L = stats");
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
		config.padFactory = CPH_Vita::GetFactoryFunction(&pad);
		config.soundFactory = &CSH_Vita::HandlerFactory;

		std::unique_ptr<CEmuSession> session;
		try
		{
			session = std::make_unique<CEmuSession>(config);
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
		bool showStats = true;
		uint32_t previousButtons = ~0u;

		uint64_t statsTime = sceKernelGetProcessTimeWide();
		uint64_t statsVmFrames = 0, statsPresented = 0, presented = 0;
		float vmFps = 0, presentFps = 0;

		while(true)
		{
			uint32_t buttons = pad ? pad->Poll() : 0;
			uint32_t pressed = buttons & ~previousButtons;
			previousButtons = buttons;
			if((buttons & SCE_CTRL_SELECT) && (buttons & SCE_CTRL_START)) break;
			if((buttons & SCE_CTRL_SELECT) && (pressed & SCE_CTRL_L1)) showStats = !showStats;

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
			if(width != 0 && height != 0)
			{
				// Fit to the 960x544 screen keeping a 4:3 picture.
				float dstH = 544.0f;
				float dstW = dstH * 4.0f / 3.0f;
				vita2d_draw_texture_part_scale(screen, (960.0f - dstW) / 2.0f, 0, 0, 0, width, height,
				                               dstW / width, dstH / height);
			}
			if(showStats)
			{
				vita2d_draw_rectangle(0, 0, 330, 58, RGBA8(0, 0, 0, 160));
				vita2d_pgf_draw_textf(g_font, 8, 22, COLOR_WHITE, 0.8f, "VM %.1f fps  out %.1f fps  %ux%u",
				                      vmFps, presentFps, width, height);
				vita2d_pgf_draw_textf(g_font, 8, 46, COLOR_GREY, 0.8f, "JIT %u/%u KB",
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

	if(!VitaJit_Init(JIT_POOL_SIZE))
	{
		ShowMessage("Cannot allocate JIT memory",
		            "VitaPS2 needs unsafe homebrew to be enabled.\n"
		            "Open HENkaku Settings and enable \"Unsafe Homebrew\".");
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
