#include "GameProfiles.h"
#include <cctype>
#include <fstream>
#include <sstream>

namespace
{
	std::string Trim(const std::string& value)
	{
		size_t begin = value.find_first_not_of(" \t\r\n");
		if(begin == std::string::npos) return std::string();
		size_t end = value.find_last_not_of(" \t\r\n");
		return value.substr(begin, end - begin + 1);
	}
}

std::string CGameProfiles::NormalizeSerial(const std::string& serial)
{
	std::string result;
	for(char c : serial)
	{
		if((c == '-') || (c == '_') || (c == '.') || std::isspace(static_cast<unsigned char>(c))) continue;
		result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
	}
	return result;
}

bool CGameProfiles::Load(const std::string& path)
{
	std::ifstream file(path);
	if(!file) return false;
	std::stringstream buffer;
	buffer << file.rdbuf();
	Parse(buffer.str());
	return true;
}

void CGameProfiles::Parse(const std::string& text)
{
	std::istringstream stream(text);
	std::string line;
	PROFILE* current = nullptr;
	while(std::getline(stream, line))
	{
		line = Trim(line);
		if(line.empty() || (line[0] == '#') || (line[0] == ';')) continue;
		if(line[0] == '[')
		{
			auto close = line.find(']');
			if(close == std::string::npos) continue;
			current = &m_profiles[NormalizeSerial(line.substr(1, close - 1))];
			continue;
		}
		if(!current) continue;
		auto eq = line.find('=');
		if(eq == std::string::npos) continue;
		(*current)[Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
	}
}

const CGameProfiles::PROFILE* CGameProfiles::Find(const std::string& serial) const
{
	auto it = m_profiles.find(NormalizeSerial(serial));
	return (it != m_profiles.end()) ? &it->second : nullptr;
}
