// Renders UI elements with Gfx on an offscreen Mesa context and checks the
// result (text, rectangles, frame presentation). Pass a path to also write the
// screen as a PPM.

#include <GL/osmesa.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "Gfx.h"

namespace
{
	constexpr uint32_t WIDTH = 960, HEIGHT = 544;
	int g_failures = 0;

	// OSMesa's buffer is bottom-up.
	uint32_t Pixel(const std::vector<uint32_t>& screen, uint32_t x, uint32_t y)
	{
		return screen[(HEIGHT - 1 - y) * WIDTH + x] & 0x00FFFFFF;
	}

	void Check(bool condition, const char* what)
	{
		if(!condition)
		{
			std::printf("  FAIL %s\n", what);
			g_failures++;
		}
	}
}

int main(int argc, char** argv)
{
	const int attribs[] = {OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_PROFILE, OSMESA_COMPAT_PROFILE, 0};
	OSMesaContext context = OSMesaCreateContextAttribs(attribs, nullptr);
	std::vector<uint32_t> screen(WIDTH * HEIGHT);
	if(!context || !OSMesaMakeCurrent(context, screen.data(), GL_UNSIGNED_BYTE, WIDTH, HEIGHT))
	{
		std::printf("could not create an OSMesa context\n");
		return 1;
	}
	Check(Gfx::Init(), "init");

	// A 64x32 "emulated frame": left half red, right half blue.
	std::vector<uint32_t> frame(64 * 32);
	for(uint32_t y = 0; y < 32; y++)
		for(uint32_t x = 0; x < 64; x++)
			frame[x + y * 64] = (x < 32) ? 0xFF0000FF : 0xFFFF0000;
	Gfx::UploadFrame(frame.data(), 64, 32);

	const uint32_t bg = Gfx::Rgba(16, 18, 28, 255);
	Gfx::BeginFrame(WIDTH, HEIGHT, bg);
	Gfx::Image(Gfx::FrameTexture(), Gfx::FRAME_TEXTURE_SIZE, Gfx::FRAME_TEXTURE_SIZE, 0, 0, 64, 32, 600, 300, 256, 128, false);
	Gfx::Rect(120, 90, 300, 100, Gfx::Rgba(40, 60, 110, 255));
	Gfx::Rect(100, 400, 200, 50, Gfx::Rgba(255, 255, 255, 128));
	Gfx::Text(130, 130, Gfx::Rgba(255, 255, 255, 255), 1.0f, "Paused");
	Gfx::Textf(130, 170, Gfx::Rgba(150, 150, 160, 255), 0.8f, "EE cycle rate: %u%%", 100u);
	Gfx::Text(30, 45, Gfx::Rgba(90, 160, 255, 255), 1.5f, "VitaPS2\nsecond line");
	Gfx::Flush();
	glFinish();

	Check(Pixel(screen, 5, 5) == (bg & 0xFFFFFF), "background");
	Check(Pixel(screen, 400, 180) == 0x6E3C28, "opaque rectangle");
	uint32_t half = Pixel(screen, 290, 440);
	Check(((half & 0xFF) > 120) && ((half & 0xFF) < 150), "translucent rectangle");
	Check(Pixel(screen, 610, 310) == 0x0000FF, "frame left half");
	Check(Pixel(screen, 840, 420) == 0xFF0000, "frame right half");

	// Some white text pixels inside "Paused", none above it.
	uint32_t textPixels = 0, strayPixels = 0;
	for(uint32_t y = 112; y < 132; y++)
		for(uint32_t x = 130; x < 130 + 6 * 12; x++)
			if(Pixel(screen, x, y) == 0xFFFFFF) textPixels++;
	for(uint32_t y = 95; y < 108; y++)
		for(uint32_t x = 130; x < 130 + 6 * 12; x++)
			if(Pixel(screen, x, y) != 0x6E3C28) strayPixels++;
	Check(textPixels > 50, "text drawn");
	Check(strayPixels == 0, "text placed under its baseline");
	Check(Gfx::TextWidth(1.0f, "abc\nlonger") == 6 * 12, "text width");
	Check(glGetError() == GL_NO_ERROR, "GL errors");

	if(argc > 1)
	{
		if(FILE* file = std::fopen(argv[1], "wb"))
		{
			std::fprintf(file, "P6\n%u %u\n255\n", WIDTH, HEIGHT);
			for(uint32_t y = 0; y < HEIGHT; y++)
				for(uint32_t x = 0; x < WIDTH; x++)
				{
					uint32_t c = Pixel(screen, x, y);
					uint8_t rgb[3] = {static_cast<uint8_t>(c), static_cast<uint8_t>(c >> 8), static_cast<uint8_t>(c >> 16)};
					std::fwrite(rgb, 1, 3, file);
				}
			std::fclose(file);
		}
	}

	Gfx::Shutdown();
	OSMesaDestroyContext(context);
	std::printf("%s\n", g_failures ? "FAILED" : "PASSED");
	return g_failures ? 1 : 0;
}
