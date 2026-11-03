// Renders the VitaStation2 game list (Theme::Browser) on the host through
// OSMesa, as the Vita draws it, into a PPM: a screenshot for review without a
// Vita.
//
//   theme_preview <artwork dir (assets/brand)> <out.ppm>

#include <GL/osmesa.h>
#include <cstdio>
#include <string>
#include <vector>
#include "Gfx.h"
#include "Theme.h"

int main(int argc, char** argv)
{
	const char* artwork = (argc > 1) ? argv[1] : "assets/brand";
	const char* out = (argc > 2) ? argv[2] : "theme_preview.ppm";
	const int width = 960, height = 544;
	const int attribs[] = {OSMESA_FORMAT, OSMESA_RGBA, OSMESA_DEPTH_BITS, 24, OSMESA_PROFILE, OSMESA_COMPAT_PROFILE, 0};
	OSMesaContext context = OSMesaCreateContextAttribs(attribs, nullptr);
	std::vector<uint32_t> buffer(width * height);
	if(!context || !OSMesaMakeCurrent(context, buffer.data(), GL_UNSIGNED_BYTE, width, height))
	{
		std::printf("no OSMesa context\n");
		return 1;
	}
	if(!Gfx::Init()) return 1;
	Theme::Init(artwork);

	std::vector<std::string> games = {"Silent Hill 2 (USA).iso", "Kingdom Hearts (USA).iso", "Final Fantasy XII (USA).chd",
	                                  "Grand Theft Auto - San Andreas (USA).cso", "Tekken 5 (USA).iso", "Dragon Quest VIII (USA).iso",
	                                  "Shadow of the Colossus (USA).iso", "Okami (USA).iso", "Gran Turismo 4 (USA).chd",
	                                  "Star Wars - Battlefront (USA).iso", "Spider-Man 2 (USA).iso", "Devil May Cry 3 (USA).iso",
	                                  "Resident Evil 4 (USA).iso", "Ace Combat 04 (USA).iso"};
	Gfx::BeginFrame(width, height, Gfx::Rgba(0, 0, 0, 255));
	Theme::Browser(games, 2, 0, 12, 4.0f, "PlayStation 2 emulator  build preview", "ux0:data/VitaPS2/games");
	Gfx::Flush();
	glFinish();

	// OSMesa rows are bottom up.
	std::FILE* file = std::fopen(out, "wb");
	if(!file) return 1;
	std::fprintf(file, "P6\n%d %d\n255\n", width, height);
	for(int y = height - 1; y >= 0; y--)
	{
		for(int x = 0; x < width; x++)
		{
			uint32_t p = buffer[y * width + x];
			unsigned char rgb[3] = {static_cast<unsigned char>(p), static_cast<unsigned char>(p >> 8), static_cast<unsigned char>(p >> 16)};
			std::fwrite(rgb, 1, 3, file);
		}
	}
	std::fclose(file);
	std::printf("wrote %s\n", out);
	return 0;
}
