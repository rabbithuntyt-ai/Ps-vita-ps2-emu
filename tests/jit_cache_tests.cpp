// CJitCache (Play patch 0064): file format, reload, invalidation and the
// ARMv7 relocation helpers. Runs on any host (SetForceSupported); the
// end-to-end test with real generated code is jit_cache_ee_test (ARM only).

#include <cstdio>
#include <cstring>
#include <vector>
#include "JitCache.h"

namespace
{
	unsigned int g_failures = 0;

	void Check(bool condition, const char* what)
	{
		if(!condition)
		{
			std::printf("FAIL: %s\n", what);
			g_failures++;
		}
	}

	CJitCache::KEY Key(uint32 begin, uint64 hash)
	{
		CJitCache::KEY key;
		key.category = 0x65650000;
		key.begin = begin;
		key.size = 32;
		key.hash.nD0 = hash;
		key.hash.nD1 = ~hash;
		return key;
	}

	CJitCache::ENTRY Entry(uint8 fill, uint32 relocCount)
	{
		CJitCache::ENTRY entry;
		entry.code.assign(64, fill);
		for(uint32 i = 0; i < relocCount; i++)
		{
			CJitCache::RELOC reloc;
			reloc.offset = i * 8;
			reloc.type = (i % 2) ? CJitCache::RELOC_WORD : CJitCache::RELOC_MOVW_MOVT;
			reloc.range = static_cast<uint8>(i % 2);
			reloc.rangeOffset = 0x100 * i;
			entry.relocs.push_back(reloc);
		}
		entry.linkEntryOffset = 12;
		entry.trampolineOffset[0] = 40;
		return entry;
	}

	bool SameEntry(const CJitCache::ENTRY& a, const CJitCache::ENTRY& b)
	{
		if((a.code != b.code) || (a.relocs.size() != b.relocs.size()) || (a.linkEntryOffset != b.linkEntryOffset)) return false;
		if((a.trampolineOffset[0] != b.trampolineOffset[0]) || (a.trampolineOffset[1] != b.trampolineOffset[1])) return false;
		for(size_t i = 0; i < a.relocs.size(); i++)
		{
			const auto &x = a.relocs[i], &y = b.relocs[i];
			if((x.offset != y.offset) || (x.type != y.type) || (x.range != y.range) || (x.rangeOffset != y.rangeOffset)) return false;
		}
		return true;
	}
}

int main()
{
	CJitCache::SetForceSupported(true);
	auto& cache = CJitCache::GetInstance();
	const char* path = "jit_cache_test.jit";
	std::remove(path);
	std::vector<CJitCache::RANGE> ranges = {{0x81000000, 0x400000}, {0x81800000, 0x100000}};

	// MOVW r0, #0x1234 / MOVT r0, #0x5678 (ARM encodings), relocated and read back.
	{
		uint8 code[8];
		uint32 movw = 0xE3010234, movt = 0xE3450678;
		std::memcpy(code, &movw, 4);
		std::memcpy(code + 4, &movt, 4);
		Check(CJitCache::ReadMovwMovt(code, 0) == 0x56781234, "MOVW/MOVT decode");
		CJitCache::ApplyReloc(code, CJitCache::RELOC_MOVW_MOVT, 0, 0x8104ABCD);
		Check(CJitCache::ReadMovwMovt(code, 0) == 0x8104ABCD, "MOVW/MOVT encode");
		uint32 w, t;
		std::memcpy(&w, code, 4);
		std::memcpy(&t, code + 4, 4);
		Check(((w & 0xFFF0F000) == 0xE3000000) && ((t & 0xFFF0F000) == 0xE3400000), "MOVW/MOVT keep opcode and register");
		CJitCache::ApplyReloc(code, CJitCache::RELOC_WORD, 4, 0xDEADBEEF);
		std::memcpy(&t, code + 4, 4);
		Check(t == 0xDEADBEEF, "word relocation");
	}

	// Classify / Resolve against the ranges.
	{
		Check(cache.Open(path, 42, ranges), "open a new cache");
		uint8 range = 0;
		uint32 offset = 0;
		Check(cache.Classify(0x81000010, range, offset) && (range == 0) && (offset == 0x10), "pointer in the code segment");
		Check(cache.Classify(0x818000F0, range, offset) && (range == 1) && (offset == 0xF0), "pointer in the data segment");
		Check(!cache.Classify(0x90000000, range, offset), "pointer outside the ranges");
		Check(cache.Resolve(1, 0x20) == 0x81800020, "resolve");
	}

	// Store, close, reopen: same entries back.
	{
		cache.Store(Key(0x100000, 1), Entry(0xAA, 3));
		cache.Store(Key(0x100040, 2), Entry(0xBB, 0));
		cache.Store(Key(0x100040, 2), Entry(0xCC, 1)); //duplicate key: ignored
		Check(cache.GetStats().stored == 2, "two entries stored");
		cache.Close();
		Check(cache.Open(path, 42, ranges), "reopen");
		Check(cache.GetStats().loaded == 2, "two entries loaded");
		const auto* a = cache.Find(Key(0x100000, 1));
		const auto* b = cache.Find(Key(0x100040, 2));
		Check(a && SameEntry(*a, Entry(0xAA, 3)), "first entry intact");
		Check(b && SameEntry(*b, Entry(0xBB, 0)), "second entry intact (first store wins)");
		Check(!cache.Find(Key(0x100000, 2)), "different hash: miss");
		Check(!cache.Find(Key(0x100080, 1)), "different address: miss");
		// Appending after reopening keeps the old entries.
		cache.Store(Key(0x200000, 3), Entry(0xDD, 2));
		cache.Close();
		Check(cache.Open(path, 42, ranges) && (cache.GetStats().loaded == 3), "appended entry loaded");
		cache.Close();
	}

	// A record cut short (app closed while writing) drops only that record.
	{
		std::FILE* file = std::fopen(path, "rb");
		std::fseek(file, 0, SEEK_END);
		long size = std::ftell(file);
		std::vector<char> data(size);
		std::fseek(file, 0, SEEK_SET);
		std::fread(data.data(), 1, size, file);
		std::fclose(file);
		file = std::fopen(path, "wb");
		std::fwrite(data.data(), 1, size - 10, file);
		std::fclose(file);
		Check(cache.Open(path, 42, ranges) && (cache.GetStats().loaded == 2), "cut-off record dropped, others kept");
		cache.Store(Key(0x300000, 4), Entry(0xEE, 1));
		cache.Close();
		Check(cache.Open(path, 42, ranges) && (cache.GetStats().loaded == 3), "appends after the last complete record");
		cache.Close();
	}

	// Another build (fingerprint) or other segment sizes: the file starts over.
	{
		Check(cache.Open(path, 43, ranges) && (cache.GetStats().loaded == 0), "other fingerprint: discarded");
		cache.Store(Key(0x100000, 1), Entry(0xAA, 1));
		cache.Close();
		auto other = ranges;
		other[0].size += 4;
		Check(cache.Open(path, 43, other) && (cache.GetStats().loaded == 0), "other segment sizes: discarded");
		cache.Close();
		Check(cache.Open(path, 43, other) && (cache.GetStats().loaded == 0), "the discarded file was rewritten empty");
		cache.Close();
	}

	// Not open: nothing found, nothing stored.
	{
		Check(!cache.IsOpen() && !cache.Find(Key(0x100000, 1)), "closed cache finds nothing");
	}

	std::remove(path);
	std::printf("jit cache: %u failures\n", g_failures);
	return (g_failures == 0) ? 0 : 1;
}
