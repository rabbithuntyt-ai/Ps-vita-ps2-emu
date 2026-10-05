#pragma once

// Per-game default settings shipped with the emulator, keyed by disc serial
// (as read from SYSTEM.CNF, e.g. "SLUS-20228"). The user's own per-game
// choices override them.
//
// File format (INI):
//   [SLUS-20228]
//   name=Silent Hill 2
//   threaded_vu1=1
//   ee_cycle_rate=90

#include <map>
#include <string>

class CGameProfiles
{
public:
	using PROFILE = std::map<std::string, std::string>;

	bool Load(const std::string& path);
	void Parse(const std::string& text);

	// nullptr when the game has no profile.
	const PROFILE* Find(const std::string& serial) const;

	size_t GetCount() const
	{
		return m_profiles.size();
	}

	// Serials are compared ignoring case and '-', '_', '.' (SLUS-20228,
	// SLUS_202.28 and slus20228 are the same game).
	static std::string NormalizeSerial(const std::string&);

private:
	std::map<std::string, PROFILE> m_profiles;
};
