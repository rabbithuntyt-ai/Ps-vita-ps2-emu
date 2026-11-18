// The game list scan: subfolders, BIN/CUE pairs listed once, lone raw .bin given a cue.
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>
#include "GameScan.h"

namespace
{
	int g_failures = 0;
	void Check(bool condition, const char* what)
	{
		if(!condition)
		{
			std::printf("  FAIL %s\n", what);
			g_failures++;
		}
	}

	void WriteFile(const std::string& path, const void* data, size_t size)
	{
		std::FILE* file = std::fopen(path.c_str(), "wb");
		if(!file) return;
		if(size) std::fwrite(data, 1, size, file);
		std::fclose(file);
	}

	void WriteText(const std::string& path, const char* text)
	{
		WriteFile(path, text, std::strlen(text));
	}

	std::string ReadText(const std::string& path)
	{
		std::string text;
		std::FILE* file = std::fopen(path.c_str(), "rb");
		if(!file) return text;
		char buffer[512];
		size_t read = 0;
		while((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
		std::fclose(file);
		return text;
	}

	bool IsGame(const std::string& path)
	{
		auto ext = GameScan::LowerExtension(path);
		return (ext == ".iso") || (ext == ".cue") || (ext == ".bin") || (ext == ".elf");
	}
}

int main(int argc, char** argv)
{
	std::string root = (argc > 1) ? argv[1] : "game_scan_test_root";
	mkdir(root.c_str(), 0777);
	std::string games = root + "/games";
	// Written later in the test: a leftover from an earlier run would be listed
	std::remove((games + "/Nested/raw.bin").c_str());
	std::remove((root + "/cue/raw.cue").c_str());
	mkdir(games.c_str(), 0777);
	mkdir((games + "/TimeSplitters (USA)").c_str(), 0777);
	mkdir((games + "/Nested").c_str(), 0777);
	mkdir((games + "/Nested/Deeper").c_str(), 0777);

	WriteText(games + "/re4.iso", "x");
	WriteText(games + "/notes.txt", "x");
	// A pair at the top: the cue names a differently cased .bin
	WriteText(games + "/Game A.cue", "FILE \"Game A (Track 1).BIN\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n");
	WriteText(games + "/Game A (Track 1).bin", "x");
	// A pair in a subfolder with the same base name, unquoted FILE line
	WriteText(games + "/TimeSplitters (USA)/TimeSplitters (USA).cue", "FILE TimeSplitters.bin BINARY\n");
	WriteText(games + "/TimeSplitters (USA)/TimeSplitters (USA).bin", "x");
	WriteText(games + "/TimeSplitters (USA)/TimeSplitters.bin", "x");
	// A lone .bin and an ELF in a subfolder, a game two levels down (not scanned)
	WriteText(games + "/Nested/lone.bin", "x");
	WriteText(games + "/Nested/demo.elf", "x");
	WriteText(games + "/Nested/Deeper/deep.iso", "x");

	auto list = GameScan::Scan(games, IsGame);
	std::vector<std::string> expected = {
	    "Game A.cue",
	    "Nested/demo.elf",
	    "Nested/lone.bin",
	    "TimeSplitters (USA)/TimeSplitters (USA).cue",
	    "re4.iso",
	};
	Check(list == expected, "scan lists games one folder deep, each BIN/CUE pair once");
	if(list != expected)
	{
		for(const auto& name : list) std::printf("    got '%s'\n", name.c_str());
	}

	// PrepareDiscPath
	std::string cueFolder = root + "/cue";
	Check(GameScan::PrepareDiscPath(games + "/re4.iso", cueFolder) == games + "/re4.iso", "an ISO boots as it is");
	Check(GameScan::PrepareDiscPath(games + "/TimeSplitters (USA)/TimeSplitters (USA).bin", cueFolder) ==
	          games + "/TimeSplitters (USA)/TimeSplitters (USA).cue",
	      "a .bin with a cue of its name boots the cue");
	Check(GameScan::PrepareDiscPath(games + "/Nested/lone.bin", cueFolder) == games + "/Nested/lone.bin", "a .bin without the sync pattern boots as an ISO");

	// Raw MODE2 sectors: 00 FF*10 00, address, mode 2
	std::vector<unsigned char> sector(2352 * 2, 0);
	for(int s = 0; s < 2; s++)
	{
		unsigned char* head = sector.data() + (s * 2352);
		std::memset(head + 1, 0xFF, 10);
		head[15] = 2;
	}
	std::string raw = games + "/Nested/raw.bin";
	WriteFile(raw, sector.data(), sector.size());
	std::string cue = GameScan::PrepareDiscPath(raw, cueFolder);
	Check(cue == cueFolder + "/raw.cue", "a raw .bin gets a cue in the cue folder");
	std::string text = ReadText(cue);
	Check(text.find("FILE \"" + raw + "\" BINARY") != std::string::npos, "the cue names the .bin by its full path");
	Check(text.find("TRACK 01 MODE2/2352") != std::string::npos, "MODE2/2352 track");
	Check(GameScan::PrepareDiscPath(raw, "") == raw, "no cue folder: no cue");

	auto refs = GameScan::CueReferences(games + "/Game A.cue");
	Check((refs.size() == 1) && (refs[0] == "Game A (Track 1).BIN"), "cue FILE line parsed");

	std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "passed", g_failures);
	return g_failures ? 1 : 0;
}
