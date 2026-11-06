// GsTransferWrite (patch 0061) against Play!'s per pixel transfer writes
// (CPixelIndexor) on random transfers, fed in random sized pieces.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "GsMemory.h"
#include "TextureCache.h"
#include "gs/GsTransferWrite.h"

namespace
{
	constexpr uint32 RAMSIZE = CGSHandler::RAMSIZE;

	struct TRANSFER
	{
		uint32 pointer, width, dsax, dsay, rrw;
	};

	// Play!'s original loops (GSHandler.cpp before patch 0061).
	template <typename Storage>
	bool ReferenceUnits(uint8* ram, const TRANSFER& t, uint32& rrx, uint32& rry, const typename Storage::Unit* source, uint32 count)
	{
		bool dirty = false;
		CGsPixelFormats::CPixelIndexor<Storage> indexor(ram, t.pointer, t.width);
		for(uint32 i = 0; i < count; i++)
		{
			uint32 x = (rrx + t.dsax) % 2048;
			uint32 y = (rry + t.dsay) % 2048;
			auto pixel = indexor.GetPixelAddress(x, y);
			if(*pixel != source[i])
			{
				*pixel = source[i];
				dirty = true;
			}
			if(++rrx == t.rrw)
			{
				rrx = 0;
				rry++;
			}
		}
		return dirty;
	}

	bool ReferencePsmt4(uint8* ram, const TRANSFER& t, uint32& rrx, uint32& rry, const uint8* source, uint32 bytes)
	{
		bool dirty = false;
		CGsPixelFormats::CPixelIndexorPSMT4 indexor(ram, t.pointer, t.width);
		for(uint32 i = 0; i < bytes; i++)
		{
			uint8 pixels[2] = {static_cast<uint8>(source[i] & 0x0F), static_cast<uint8>(source[i] >> 4)};
			for(auto value : pixels)
			{
				uint32 x = (rrx + t.dsax) % 2048;
				uint32 y = (rry + t.dsay) % 2048;
				if(indexor.GetPixel(x, y) != value)
				{
					indexor.SetPixel(x, y, value);
					dirty = true;
				}
				if(++rrx == t.rrw)
				{
					rrx = 0;
					rry++;
				}
			}
		}
		return dirty;
	}

	std::mt19937 g_random(42);

	// Whole PSMT4 pages (the page row fast path): page aligned x, widths of
	// whole pages, page aligned y most of the time.
	TRANSFER RandomPageTransfer()
	{
		TRANSFER t;
		t.pointer = (g_random() % 0x4000) * 256;
		t.width = 2 * (1 + (g_random() % 8));
		t.dsax = (g_random() % 12) * 128;
		t.dsay = (g_random() % 4 == 0) ? (g_random() % 2048) : (g_random() % 16) * 128;
		t.rrw = 128 * (1 + (g_random() % 4));
		return t;
	}

	TRANSFER RandomTransfer(uint32 maxWidth)
	{
		TRANSFER t;
		t.pointer = (g_random() % 0x4000) * 256; //block aligned, anywhere in GS memory
		t.width = 1 + (g_random() % 16);
		t.dsax = (g_random() % 4 == 0) ? (2040 + g_random() % 8) : (g_random() % 1024); //sometimes wrapping
		t.dsay = g_random() % 2048;
		t.rrw = 1 + (g_random() % maxWidth);
		return t;
	}

	template <typename Storage>
	unsigned int TestUnits(const char* name, std::vector<uint8>& ramA, std::vector<uint8>& ramB)
	{
		typedef typename Storage::Unit Unit;
		unsigned int failures = 0;
		for(unsigned int test = 0; test < 300; test++)
		{
			TRANSFER t = RandomTransfer(300);
			uint32 total = t.rrw * (1 + g_random() % 40);
			std::vector<Unit> data(total);
			for(auto& value : data) value = static_cast<Unit>((g_random() % 4 == 0) ? 0 : g_random()); //repeats test the dirty flag
			uint32 rrxA = 0, rryA = 0, rrxB = 0, rryB = 0, done = 0;
			while(done < total)
			{
				uint32 piece = std::min<uint32>(total - done, 1 + g_random() % 700);
				bool dirtyA = ReferenceUnits<Storage>(ramA.data(), t, rrxA, rryA, data.data() + done, piece);
				GsTransferWrite::POSITION position{rrxB, rryB, t.dsax, t.dsay, t.rrw};
				bool dirtyB = GsTransferWrite::WriteUnits<Storage>(ramB.data(), RAMSIZE, t.pointer, t.width, position, data.data() + done, piece);
				if((dirtyA != dirtyB) || (rrxA != rrxB) || (rryA != rryB)) failures++;
				done += piece;
			}
			if(std::memcmp(ramA.data(), ramB.data(), RAMSIZE) != 0)
			{
				failures++;
				ramB = ramA;
			}
		}
		std::printf("%s: %u failures\n", name, failures);
		return failures;
	}

	unsigned int TestPsmt4(std::vector<uint8>& ramA, std::vector<uint8>& ramB, bool pages)
	{
		unsigned int failures = 0;
		for(unsigned int test = 0; test < 300; test++)
		{
			TRANSFER t = pages ? RandomPageTransfer() : RandomTransfer(300);
			t.rrw &= ~1u; //PSMT4 transfers are whole bytes per row
			if(t.rrw == 0) t.rrw = 2;
			uint32 rows = pages ? (1 + g_random() % 300) : (1 + g_random() % 40);
			uint32 bytes = (t.rrw / 2) * rows;
			std::vector<uint8> data(bytes);
			for(auto& value : data) value = static_cast<uint8>((g_random() % 4 == 0) ? 0 : g_random());
			// Some page transfers write the bytes already there (dirty flag).
			if(pages && (test % 5 == 0)) std::fill(data.begin(), data.end(), 0);
			uint32 rrxA = 0, rryA = 0, rrxB = 0, rryB = 0, done = 0;
			while(done < bytes)
			{
				uint32 maxPiece = (pages && (g_random() % 2)) ? 200000 : 700;
				uint32 piece = std::min<uint32>(bytes - done, 1 + g_random() % maxPiece);
				bool dirtyA = ReferencePsmt4(ramA.data(), t, rrxA, rryA, data.data() + done, piece);
				GsTransferWrite::POSITION position{rrxB, rryB, t.dsax, t.dsay, t.rrw};
				bool dirtyB = GsTransferWrite::WritePsmt4(ramB.data(), RAMSIZE, t.pointer, t.width, position, data.data() + done, piece);
				if((dirtyA != dirtyB) || (rrxA != rrxB) || (rryA != rryB)) failures++;
				done += piece;
			}
			if(std::memcmp(ramA.data(), ramB.data(), RAMSIZE) != 0)
			{
				failures++;
				ramB = ramA;
			}
		}
		std::printf("PSMT4%s: %u failures\n", pages ? " (whole pages)" : "", failures);
		return failures;
	}
}

namespace
{
	// "bench": the menu transfers of Silent Hill 2 (512x512 PSMT4, 256x256 PSMCT32).
	void Benchmark()
	{
		std::vector<uint8> ram(RAMSIZE);
		TRANSFER t4{0x100000, 8, 0, 0, 512}, t32{0x200000, 4, 0, 0, 256};
		std::vector<uint8> d4(512 * 512 / 2, 0x5A);
		std::vector<uint32> d32(256 * 256, 0x12345678);
		auto time = [](auto f) {
			auto start = std::chrono::steady_clock::now();
			for(int i = 0; i < 50; i++) f(i);
			return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 50;
		};
		double a = time([&](int i) { uint32 x = 0, y = 0; d4[0] = static_cast<uint8>(i); ReferencePsmt4(ram.data(), t4, x, y, d4.data(), static_cast<uint32>(d4.size())); });
		double b = time([&](int i) { uint32 x = 0, y = 0; d4[0] = static_cast<uint8>(i); GsTransferWrite::POSITION p{x, y, 0, 0, 512}; GsTransferWrite::WritePsmt4(ram.data(), RAMSIZE, t4.pointer, 8, p, d4.data(), static_cast<uint32>(d4.size())); });
		double c = time([&](int i) { uint32 x = 0, y = 0; d32[0] = i; ReferenceUnits<CGsPixelFormats::STORAGEPSMCT32>(ram.data(), t32, x, y, d32.data(), static_cast<uint32>(d32.size())); });
		double d = time([&](int i) { uint32 x = 0, y = 0; d32[0] = i; GsTransferWrite::POSITION p{x, y, 0, 0, 256}; GsTransferWrite::WriteUnits<CGsPixelFormats::STORAGEPSMCT32>(ram.data(), RAMSIZE, t32.pointer, 4, p, d32.data(), static_cast<uint32>(d32.size())); });
		std::printf("PSMT4 512x512: %.3f -> %.3f ms; PSMCT32 256x256: %.3f -> %.3f ms\n", a, b, c, d);
	}
}

// Whole texture decoding in memory order against one texel at a time.
static unsigned int CheckCt32PageDecode()
{
	unsigned int failures = 0;
	std::vector<uint8> ram(CGSHandler::RAMSIZE);
	std::vector<uint16> clut(1024);
	for(size_t i = 0; i < ram.size(); i++) ram[i] = static_cast<uint8>((i * 2654435761u) >> 13);
	CTextureCache cache;
	cache.SetMemory(ram.data(), clut.data());
	const uint32 sizes[][3] = {{512, 512, 8}, {256, 256, 8}, {64, 32, 1}, {128, 64, 2}, {512, 336, 8}, {256, 128, 4}};
	for(const auto& size : sizes)
	{
		CTextureCache::KEY key;
		key.tbp = 0x1C0000;
		key.tbw = size[2];
		key.tpsm = CGSHandler::PSMCT32;
		key.tw = size[0];
		key.th = size[1];
		key.cpsm = 0;
		key.csa = 0;
		key.clutHash = 0;
		key.texa = 0;
		auto texture = cache.Get(key);
		texture->DecodeAll();
		const uint32* texels = texture->Texels();
		for(uint32 y = 0; y < size[1]; y++)
		{
			for(uint32 x = 0; x < size[0]; x++)
			{
				if(texels[y * size[0] + x] != GsMemory::ReadRaw(ram.data(), CGSHandler::PSMCT32, key.tbp, size[2], x, y))
				{
					failures++;
					y = size[1];
					break;
				}
			}
		}
	}
	std::printf("CT32 page decode: %u failures\n", failures);
	return failures;
}

int main(int argc, char** argv)
{
	if((argc > 1) && (std::strcmp(argv[1], "bench") == 0))
	{
		Benchmark();
		return 0;
	}
	std::vector<uint8> ramA(RAMSIZE), ramB;
	for(auto& value : ramA) value = static_cast<uint8>(g_random());
	ramB = ramA;
	unsigned int failures = 0;
	failures += TestUnits<CGsPixelFormats::STORAGEPSMCT32>("PSMCT32", ramA, ramB);
	failures += TestUnits<CGsPixelFormats::STORAGEPSMCT16>("PSMCT16", ramA, ramB);
	failures += TestUnits<CGsPixelFormats::STORAGEPSMCT16S>("PSMCT16S", ramA, ramB);
	failures += TestUnits<CGsPixelFormats::STORAGEPSMT8>("PSMT8", ramA, ramB);
	failures += TestPsmt4(ramA, ramB, false);
	failures += TestPsmt4(ramA, ramB, true);
	failures += CheckCt32PageDecode();
	return (failures == 0) ? 0 : 1;
}
