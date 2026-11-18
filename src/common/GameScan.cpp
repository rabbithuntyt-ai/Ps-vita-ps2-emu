#include "GameScan.h"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

std::string GameScan::LowerExtension(const std::string& name)
{
	auto dot = name.find_last_of('.');
	if((dot == std::string::npos) || (name.find('/', dot) != std::string::npos)) return std::string();
	std::string extension = name.substr(dot);
	for(auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return extension;
}

std::string GameScan::StripExtension(const std::string& name)
{
	auto dot = name.find_last_of('.');
	return (dot == std::string::npos) ? name : name.substr(0, dot);
}

std::vector<std::string> GameScan::ListFolder(const std::string& folder, std::vector<std::string>* subfolders)
{
	std::vector<std::string> names;
	DIR* dir = opendir(folder.c_str());
	if(!dir) return names;
	while(dirent* entry = readdir(dir))
	{
		std::string name = entry->d_name;
		if(name.empty() || (name[0] == '.')) continue;
		struct stat info = {};
		if(stat((folder + "/" + name).c_str(), &info) != 0) continue;
		if(S_ISDIR(info.st_mode))
		{
			if(subfolders) subfolders->push_back(name);
		}
		else
		{
			names.push_back(name);
		}
	}
	closedir(dir);
	return names;
}

std::vector<std::string> GameScan::CueReferences(const std::string& cuePath)
{
	// FILE "name.bin" BINARY (or FILE name.bin BINARY): the file names, without folders
	std::vector<std::string> files;
	std::FILE* file = std::fopen(cuePath.c_str(), "rb");
	if(!file) return files;
	char line[1024];
	while(std::fgets(line, sizeof(line), file))
	{
		const char* text = line;
		while((*text == ' ') || (*text == '\t')) text++;
		if(std::strncmp(text, "FILE", 4) != 0) continue;
		text += 4;
		while((*text == ' ') || (*text == '\t')) text++;
		std::string name;
		if(*text == '"')
		{
			const char* end = std::strchr(text + 1, '"');
			if(!end) continue;
			name.assign(text + 1, end);
		}
		else
		{
			const char* end = text;
			while(*end && (*end != ' ') && (*end != '\t') && (*end != '\r') && (*end != '\n')) end++;
			name.assign(text, end);
		}
		auto slash = name.find_last_of("/\\");
		if(slash != std::string::npos) name = name.substr(slash + 1);
		if(!name.empty()) files.push_back(name);
	}
	std::fclose(file);
	return files;
}

std::string GameScan::PrepareDiscPath(const std::string& path, const std::string& cueFolder)
{
	if((LowerExtension(path) != ".bin") || cueFolder.empty()) return path;
	// A cue next to it (same name): boot that
	std::string sibling = StripExtension(path) + ".cue";
	if(std::FILE* cue = std::fopen(sibling.c_str(), "rb"))
	{
		std::fclose(cue);
		return sibling;
	}
	// Raw CD sectors start with the sync pattern 00 FF*10 00
	uint8_t head[16] = {};
	std::FILE* file = std::fopen(path.c_str(), "rb");
	if(!file) return path;
	size_t read = std::fread(head, 1, sizeof(head), file);
	std::fclose(file);
	static const uint8_t sync[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
	if((read < sizeof(head)) || (std::memcmp(head, sync, sizeof(sync)) != 0)) return path;
	// Mode byte at 15: 1 = MODE1/2352, 2 = MODE2/2352 (PS2 CDs)
	const char* mode = (head[15] == 1) ? "MODE1/2352" : "MODE2/2352";
	mkdir(cueFolder.c_str(), 0777);
	auto slash = path.find_last_of('/');
	std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
	std::string cuePath = cueFolder + "/" + StripExtension(name) + ".cue";
	std::FILE* cue = std::fopen(cuePath.c_str(), "wb");
	if(!cue) return path;
	std::fprintf(cue, "FILE \"%s\" BINARY\n  TRACK 01 %s\n    INDEX 01 00:00:00\n", path.c_str(), mode);
	std::fclose(cue);
	return cuePath;
}
