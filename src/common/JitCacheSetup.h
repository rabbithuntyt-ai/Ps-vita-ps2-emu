#pragma once

// Opens Play!'s persistent JIT cache (JitCache.h) for a game: finds this
// executable's segments (the relocation bases) and fingerprints the build
// (a hash of the executable file and the code generator's modes), so a cache
// written by another build is discarded instead of loaded.

#include <string>

namespace JitCacheSetup
{
	// cachePath: the game's cache file. executablePath: this program's file
	// (app0:eboot.bin on the Vita, /proc/self/exe on Linux). Returns false
	// when the cache can't be used here (not 32-bit ARM, no segments, I/O).
	bool Open(const std::string& cachePath, const std::string& executablePath);
	void Close();
	// One line: entries loaded, hits, stored, uncacheable, failures.
	std::string Describe();
}
