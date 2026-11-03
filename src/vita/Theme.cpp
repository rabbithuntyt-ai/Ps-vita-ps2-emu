#include "Theme.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <vector>
#include "Gfx.h"

namespace
{
	struct MASK
	{
		GLuint texture = 0;
		uint32_t width = 0, height = 0;
	};

	MASK g_wordmark, g_digit, g_glow, g_buttons;

	MASK LoadMask(const char* path)
	{
		MASK mask;
		std::FILE* file = std::fopen(path, "rb");
		if(!file) return mask;
		uint32_t size[2] = {};
		if((std::fread(size, sizeof(size), 1, file) == 1) && (size[0] > 0) && (size[1] > 0) && (size[0] <= 2048) && (size[1] <= 2048))
		{
			std::vector<uint8_t> alpha(static_cast<size_t>(size[0]) * size[1]);
			if(std::fread(alpha.data(), 1, alpha.size(), file) == alpha.size())
			{
				mask.texture = Gfx::CreateMask(size[0], size[1], alpha.data());
				mask.width = size[0];
				mask.height = size[1];
			}
		}
		std::fclose(file);
		return mask;
	}

	// Brand colors (tools/make_branding.py).
	constexpr uint32_t TOP = Gfx::Rgba(14, 30, 92, 255);
	constexpr uint32_t BOTTOM = Gfx::Rgba(4, 7, 24, 255);
	constexpr uint32_t BLUE = Gfx::Rgba(40, 110, 255, 255);
	constexpr uint32_t CYAN = Gfx::Rgba(110, 200, 255, 255);
	constexpr uint32_t WHITE = Gfx::Rgba(240, 244, 255, 255);

	uint32_t WithAlpha(uint32_t color, float alpha)
	{
		uint32_t a = static_cast<uint32_t>(std::fmax(0.0f, std::fmin(1.0f, alpha)) * 255.0f);
		return (color & 0x00FFFFFF) | (a << 24);
	}

	// Soft light: the wordmark glow mask blown up, nearly transparent.
	void Light(float cx, float cy, float width, float height, uint32_t color, float alpha)
	{
		if(!g_glow.texture) return;
		Gfx::Mask(g_glow.texture, cx - width / 2, cy - height / 2, width, height, WithAlpha(color, alpha));
	}
}

const uint32_t Theme::TEXT = WHITE;
const uint32_t Theme::TEXT_DIM = Gfx::Rgba(140, 160, 210, 255);
const uint32_t Theme::ACCENT = CYAN;

void Theme::Init(const char* directory)
{
	std::string dir(directory);
	g_wordmark = LoadMask((dir + "/wordmark.a8").c_str());
	g_digit = LoadMask((dir + "/digit.a8").c_str());
	g_glow = LoadMask((dir + "/glow.a8").c_str());
	g_buttons = LoadMask((dir + "/buttons.a8").c_str());
	std::printf("theme: %s\n", g_wordmark.texture ? "artwork loaded" : "no artwork (text only)");
}

void Theme::Background(float seconds)
{
	Gfx::GradientRect(0, 0, 960, 544, TOP, BOTTOM);
	// Two lights drifting slowly (a full cycle takes about a minute).
	float a = seconds * 0.11f, b = seconds * 0.07f + 2.0f;
	Light(480 + std::sin(a) * 260, 300 + std::cos(a * 0.7f) * 60, 1400, 700, BLUE, 0.22f);
	Light(480 + std::sin(b) * 320, 180 + std::cos(b * 1.3f) * 80, 1000, 500, CYAN, 0.10f);
	// Darker floor for the button bar.
	Gfx::GradientRect(0, 470, 960, 74, Gfx::Rgba(0, 0, 0, 0), Gfx::Rgba(0, 0, 10, 140));
}

void Theme::Header(const char* rightText)
{
	if(g_wordmark.texture)
	{
		// The masks carry padding for the glow; place the letters at (36, 18).
		float x = 36 - g_wordmark.height * 0.42f, y = 10 - g_wordmark.height * 0.30f;
		Gfx::Mask(g_glow.texture, x, y, static_cast<float>(g_glow.width), static_cast<float>(g_glow.height), WithAlpha(BLUE, 0.9f));
		Gfx::Mask(g_wordmark.texture, x, y, static_cast<float>(g_wordmark.width), static_cast<float>(g_wordmark.height), WHITE);
		Gfx::Mask(g_digit.texture, x, y, static_cast<float>(g_digit.width), static_cast<float>(g_digit.height), CYAN);
	}
	else
	{
		Gfx::Text(36, 52, WHITE, 1.4f, "VitaStation2");
	}
	if(rightText) Gfx::Text(924 - Gfx::TextWidth(0.65f, rightText), 46, TEXT_DIM, 0.65f, rightText);
	// Divider fading out to the right.
	Gfx::GradientRect(36, 76, 888, 1, WithAlpha(CYAN, 0.55f), WithAlpha(CYAN, 0.55f));
}

void Theme::Row(float x, float y, float width, float height, bool selected, float seconds)
{
	if(!selected)
	{
		Gfx::Rect(x, y + height - 1, width, 1, Gfx::Rgba(255, 255, 255, 14));
		return;
	}
	float pulse = 0.75f + 0.25f * std::sin(seconds * 3.0f);
	Light(x + width * 0.3f, y + height / 2, width * 0.9f, height * 3.0f, BLUE, 0.35f * pulse);
	Gfx::GradientRect(x, y, width, height, Gfx::Rgba(50, 110, 255, 120), Gfx::Rgba(30, 70, 200, 90));
	Gfx::Rect(x, y, 4, height, WithAlpha(CYAN, 0.6f + 0.4f * pulse));
	Gfx::Rect(x, y, width, 1, Gfx::Rgba(150, 200, 255, 70));
}

float Theme::Hint(float x, float baseline, BUTTON button, const char* label)
{
	static const uint32_t colors[] = {
	    Gfx::Rgba(124, 178, 232, 255), //cross
	    Gfx::Rgba(255, 102, 102, 255), //circle
	    Gfx::Rgba(64, 226, 160, 255),  //triangle
	    Gfx::Rgba(255, 105, 248, 255), //square
	};
	if(g_buttons.texture)
	{
		// The atlas holds 4 icons of height x height.
		float size = 22.0f;
		float s0 = button * 0.25f;
		Gfx::MaskRegion(g_buttons.texture, x, baseline - size + 5, size, size, s0, 0, s0 + 0.25f, 1, colors[button]);
		x += size + 6;
	}
	else
	{
		static const char* names[] = {"X", "O", "/\\", "[]"};
		Gfx::Text(x, baseline, colors[button], 0.7f, names[button]);
		x += Gfx::TextWidth(0.7f, names[button]) + 6;
	}
	Gfx::Text(x, baseline, TEXT_DIM, 0.65f, label);
	return x + Gfx::TextWidth(0.65f, label) + 26;
}

float Theme::TextHint(float x, float baseline, const char* key, const char* label)
{
	float w = Gfx::TextWidth(0.55f, key) + 12;
	Gfx::Rect(x, baseline - 15, w, 20, Gfx::Rgba(255, 255, 255, 40));
	Gfx::Text(x + 6, baseline, TEXT, 0.55f, key);
	x += w + 6;
	Gfx::Text(x, baseline, TEXT_DIM, 0.65f, label);
	return x + Gfx::TextWidth(0.65f, label) + 26;
}

void Theme::Panel(float x, float y, float width, float height)
{
	Gfx::GradientRect(x, y, width, height, Gfx::Rgba(10, 22, 70, 225), Gfx::Rgba(4, 8, 28, 235));
	Gfx::Rect(x, y, width, 1, WithAlpha(CYAN, 0.5f));
}

void Theme::Browser(const std::vector<std::string>& games, int selected, int scroll, int visible, float seconds, const char* headerText,
                    const char* gamesPath)
{
	int count = static_cast<int>(games.size());
	Background(seconds);
	Header(headerText);
	const float rowHeight = 32, listTop = 92;
	for(int i = scroll; i < std::min(count, scroll + visible); i++)
	{
		float y = listTop + (i - scroll) * rowHeight;
		bool isSelected = (i == selected);
		Row(28, y, 904, rowHeight - 4, isSelected, seconds);
		// Name without the extension; the format as a small tag on the right.
		std::string name = games[i];
		std::string format;
		auto dot = name.find_last_of('.');
		if((dot != std::string::npos) && (name.size() - dot <= 5))
		{
			format = name.substr(dot + 1);
			for(auto& c : format) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			name = name.substr(0, dot);
		}
		Gfx::Text(44, y + 21, isSelected ? TEXT : TEXT_DIM, 0.8f, name.c_str());
		if(!format.empty()) Gfx::Text(912 - Gfx::TextWidth(0.55f, format.c_str()), y + 19, TEXT_DIM, 0.55f, format.c_str());
	}
	// Scroll position.
	if(count > visible)
	{
		float trackH = visible * rowHeight - 4;
		float thumbH = std::max(24.0f, trackH * visible / count);
		float thumbY = listTop + (trackH - thumbH) * scroll / std::max(1, count - visible);
		Gfx::Rect(938, listTop, 3, trackH, Gfx::Rgba(255, 255, 255, 25));
		Gfx::Rect(938, thumbY, 3, thumbH, ACCENT);
	}
	if(count <= 1)
	{
		Panel(28, 420, 904, 40);
		Gfx::Textf(44, 446, TEXT, 0.75f, "Copy .iso / .cso / .chd / .elf files to %s", gamesPath);
	}
	float hintX = 36;
	hintX = Hint(hintX, 520, BUTTON_CROSS, "Play");
	hintX = Hint(hintX, 520, BUTTON_TRIANGLE, "Refresh");
	hintX = TextHint(hintX, 520, "SELECT", "GS benchmark");
	TextHint(hintX, 520, "START", "Quit");
	const char* inGame = "In game: SELECT + START for the menu";
	Gfx::Text(924 - Gfx::TextWidth(0.6f, inGame), 520, TEXT_DIM, 0.6f, inGame);
}
