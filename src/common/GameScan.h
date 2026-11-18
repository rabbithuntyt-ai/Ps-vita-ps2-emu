#pragma once

// The game list: games in a folder and one level of subfolders, BIN/CUE pairs shown once.
#include <algorithm>
#include <string>
#include <vector>

namespace GameScan
{
	// Paths relative to 'root' ("game.iso", "Some Game/Some Game.cue") of the files
	// 'isGame' accepts, sorted. A .bin is left out when a .cue in its folder has its name or
	// names it in a FILE line (the .cue is the entry).
	template <typename IsGame>
	std::vector<std::string> Scan(const std::string& root, IsGame isGame);

	// The path to boot for 'path': a lone .bin of raw CD sectors (2352 bytes, sync pattern at
	// its start) gets a one-track cue sheet written into 'cueFolder' (created if needed), which
	// is returned; anything else is returned as it is (a .bin of 2048 byte sectors boots as an
	// ISO). 'cueFolder' empty: no cue is written.
	std::string PrepareDiscPath(const std::string& path, const std::string& cueFolder);

	// Implementation details used by Scan
	std::vector<std::string> ListFolder(const std::string& folder, std::vector<std::string>* subfolders);
	std::vector<std::string> CueReferences(const std::string& cuePath);
	std::string LowerExtension(const std::string& name);
	std::string StripExtension(const std::string& name);
}

template <typename IsGame>
std::vector<std::string> GameScan::Scan(const std::string& root, IsGame isGame)
{
	std::vector<std::string> games;
	auto scanFolder = [&](const std::string& relative, std::vector<std::string>* subfolders) {
		std::string folder = relative.empty() ? root : root + "/" + relative;
		auto names = ListFolder(folder, subfolders);
		// .bin files a cue here stands for: same name, or named in a FILE line
		std::vector<std::string> covered;
		for(const auto& name : names)
		{
			if(LowerExtension(name) != ".cue") continue;
			covered.push_back(StripExtension(name) + ".bin");
			for(const auto& file : CueReferences(folder + "/" + name)) covered.push_back(file);
		}
		for(const auto& name : names)
		{
			std::string path = relative.empty() ? name : relative + "/" + name;
			if(!isGame(root + "/" + path)) continue;
			if(LowerExtension(name) == ".bin")
			{
				bool isCovered = false;
				for(const auto& file : covered)
				{
					if((file.size() == name.size()) && (LowerExtension(file) == ".bin") && (StripExtension(file) == StripExtension(name))) isCovered = true;
				}
				if(isCovered) continue;
			}
			games.push_back(path);
		}
	};
	std::vector<std::string> subfolders;
	scanFolder("", &subfolders);
	for(const auto& subfolder : subfolders) scanFolder(subfolder, nullptr);
	std::sort(games.begin(), games.end());
	return games;
}
