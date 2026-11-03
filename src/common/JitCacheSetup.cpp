#include "JitCacheSetup.h"
#include <cstdio>
#include <vector>
#include "JitCache.h"
#include "xxhash.h"
#if defined(__arm__) && !defined(__aarch64__)
#include "Jitter_CodeGen_AArch32.h"
#endif
#if defined(__vita__)
#include <psp2/kernel/modulemgr.h>
#elif defined(__linux__)
#include <cstring>
#include <unistd.h>
#endif

namespace
{
	std::vector<CJitCache::RANGE> GetImageRanges()
	{
		std::vector<CJitCache::RANGE> ranges;
#if defined(__vita__)
		// The module holding this function: its segments (code, data).
		SceUID modules[64];
		SceSize count = 64;
		if(sceKernelGetModuleList(0xFF, modules, &count) < 0) return ranges;
		auto self = reinterpret_cast<uintptr_t>(&GetImageRanges);
		for(SceSize i = 0; i < count; i++)
		{
			SceKernelModuleInfo info = {};
			info.size = sizeof(info);
			if(sceKernelGetModuleInfo(modules[i], &info) < 0) continue;
			bool mine = false;
			for(const auto& segment : info.segments)
			{
				auto start = reinterpret_cast<uintptr_t>(segment.vaddr);
				if((self >= start) && (self < start + segment.memsz)) mine = true;
			}
			if(!mine) continue;
			for(const auto& segment : info.segments)
			{
				if(segment.vaddr && segment.memsz) ranges.push_back({reinterpret_cast<uintptr_t>(segment.vaddr), segment.memsz});
			}
			break;
		}
#elif defined(__linux__)
		// The main program's mappings, from /proc/self/maps (file-backed, then the
		// anonymous one right after them: .bss).
		char exe[512] = {};
		ssize_t length = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
		if(length <= 0) return ranges;
		std::FILE* maps = std::fopen("/proc/self/maps", "r");
		if(!maps) return ranges;
		char line[1024];
		bool previousMine = false;
		while(std::fgets(line, sizeof(line), maps))
		{
			unsigned long start = 0, end = 0;
			char path[600] = {};
			if(std::sscanf(line, "%lx-%lx %*s %*s %*s %*s %599[^\n]", &start, &end, path) < 2) continue;
			bool mine = (std::strcmp(path, exe) == 0);
			if(mine || (previousMine && (path[0] == 0)))
			{
				if(!ranges.empty() && (ranges.back().start + ranges.back().size == start))
					ranges.back().size += end - start;
				else
					ranges.push_back({static_cast<uintptr_t>(start), static_cast<size_t>(end - start)});
			}
			previousMine = mine;
		}
		std::fclose(maps);
#endif
		return ranges;
	}

	uint64 Fingerprint(const std::string& executablePath)
	{
		XXH3_state_t* state = XXH3_createState();
		XXH3_64bits_reset(state);
		if(std::FILE* file = std::fopen(executablePath.c_str(), "rb"))
		{
			std::vector<uint8> buffer(256 * 1024);
			size_t read = 0;
			while((read = std::fread(buffer.data(), 1, buffer.size(), file)) != 0) XXH3_64bits_update(state, buffer.data(), read);
			std::fclose(file);
		}
#if defined(__arm__) && !defined(__aarch64__)
		// Modes that change the generated code without changing the executable.
		uint8 modes[3] = {Jitter::CCodeGen_AArch32::GetConservativeAllocation(), Jitter::CCodeGen_AArch32::GetChainedFrames(),
		                  Jitter::CCodeGen_AArch32::GetGuestMemoryOps()};
		XXH3_64bits_update(state, modes, sizeof(modes));
#endif
		uint64 hash = XXH3_64bits_digest(state);
		XXH3_freeState(state);
		return hash;
	}
}

bool JitCacheSetup::Open(const std::string& cachePath, const std::string& executablePath)
{
	if(!CJitCache::IsSupported()) return false;
	auto ranges = GetImageRanges();
	if(ranges.empty()) return false;
	return CJitCache::GetInstance().Open(cachePath, Fingerprint(executablePath), ranges);
}

void JitCacheSetup::Close()
{
	CJitCache::GetInstance().Close();
}

std::string JitCacheSetup::Describe()
{
	auto stats = CJitCache::GetInstance().GetStats();
	char text[160];
	std::snprintf(text, sizeof(text), "jit cache: %u loaded, %u hits, %u stored, %u uncacheable, %u failures", stats.loaded, stats.hits,
	              stats.stored, stats.uncacheable, stats.failures);
	return text;
}
