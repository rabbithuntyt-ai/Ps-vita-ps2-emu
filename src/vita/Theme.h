#pragma once

// VitaStation2 look: an animated deep blue background, the wordmark header,
// glowing selections and button hints. Artwork comes from
// tools/make_branding.py (app0:brand/*.a8 alpha masks); without it every
// element falls back to plain text and rectangles.

#include <cstdint>
#include <string>
#include <vector>

namespace Theme
{
	enum BUTTON
	{
		BUTTON_CROSS,
		BUTTON_CIRCLE,
		BUTTON_TRIANGLE,
		BUTTON_SQUARE,
	};

	// directory: where the artwork is (app0:brand on the Vita).
	void Init(const char* directory = "app0:brand");

	// The game list screen: background, header, rows, scroll bar, button hints.
	void Browser(const std::vector<std::string>& games, int selected, int scroll, int visible, float seconds, const char* headerText,
	             const char* gamesPath);

	// seconds: animation time (slow drifting light).
	void Background(float seconds);
	// Wordmark at the top left and a small line of text at the top right.
	void Header(const char* rightText);
	// A list row; 'selected' rows glow, 'seconds' makes the glow breathe.
	void Row(float x, float y, float width, float height, bool selected, float seconds);
	// Button icon + label; returns the x after it.
	float Hint(float x, float baseline, BUTTON button, const char* label);
	// Text hint without an icon (START / SELECT), same style; returns the x after it.
	float TextHint(float x, float baseline, const char* key, const char* label);
	// A translucent panel (menus over the game picture).
	void Panel(float x, float y, float width, float height);

	extern const uint32_t TEXT;
	extern const uint32_t TEXT_DIM;
	extern const uint32_t ACCENT;
}
