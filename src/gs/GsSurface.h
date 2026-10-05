#pragma once

// Fast addressing of a swizzled GS buffer. Every PSM stores pixels in 8KB
// pages whose internal layout is described by Play!'s page offset tables, so
// a pixel's address is: base + page index * 8KB + table[y % PH][x % PW].
// Precomputing the per-row part leaves one table load and a few ALU ops per
// pixel, instead of rebuilding an indexor for every access.

#include "gs/GSHandler.h"
#include "gs/GsPixelFormats.h"

struct GS_SURFACE
{
	// Play! builds each format's page table lazily, with a non-atomic flag.
	// Build them all up front so that rasterizer threads only ever read them.
	static void InitTables()
	{
		static const bool initialized = []() {
			CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMCT32>::GetPageOffsets();
			CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMZ32>::GetPageOffsets();
			CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMCT16>::GetPageOffsets();
			CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMCT16S>::GetPageOffsets();
			CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMZ16>::GetPageOffsets();
			CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMZ16S>::GetPageOffsets();
			CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMT8>::GetPageOffsets();
			CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMT4>::GetPageOffsets();
			return true;
		}();
		(void)initialized;
	}

	uint32 base = 0;        //bytes (nibbles for PSMT4)
	uint32 pagesPerRow = 1; //pages per row of the buffer
	const uint32* table = nullptr;
	uint32 pwShift = 6, phShift = 5;
	uint32 pwMask = 63, phMask = 31;
	bool nibbles = false; //PSMT4: table holds nibble offsets

	static constexpr uint32 RAM_MASK = CGSHandler::RAMSIZE - 1;
	static constexpr uint32 PAGE_SHIFT = 13;

	// Returns false for formats that have no page table (handled generically).
	bool Init(uint32 psm, uint32 bufPtr, uint32 bufWidth)
	{
		uint32 pw = 0, ph = 0;
		switch(psm)
		{
		case CGSHandler::PSMCT32:
		case CGSHandler::PSMCT24:
		case CGSHandler::PSMCT32_UNK:
		case CGSHandler::PSMCT24_UNK:
		case CGSHandler::PSMT8H:
		case CGSHandler::PSMT4HL:
		case CGSHandler::PSMT4HH:
			table = CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMCT32>::GetPageOffsets();
			pw = 64, ph = 32;
			break;
		case CGSHandler::PSMZ32:
		case CGSHandler::PSMZ24:
			table = CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMZ32>::GetPageOffsets();
			pw = 64, ph = 32;
			break;
		case CGSHandler::PSMCT16:
			table = CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMCT16>::GetPageOffsets();
			pw = 64, ph = 64;
			break;
		case CGSHandler::PSMCT16S:
			table = CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMCT16S>::GetPageOffsets();
			pw = 64, ph = 64;
			break;
		case CGSHandler::PSMZ16:
			table = CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMZ16>::GetPageOffsets();
			pw = 64, ph = 64;
			break;
		case CGSHandler::PSMZ16S:
			table = CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMZ16S>::GetPageOffsets();
			pw = 64, ph = 64;
			break;
		case CGSHandler::PSMT8:
			table = CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMT8>::GetPageOffsets();
			pw = 128, ph = 64;
			break;
		case CGSHandler::PSMT4:
			table = CGsPixelFormats::CPixelIndexor<CGsPixelFormats::STORAGEPSMT4>::GetPageOffsets();
			pw = 128, ph = 128;
			nibbles = true;
			break;
		default:
			return false;
		}
		pwShift = (pw == 128) ? 7 : 6;
		phShift = (ph == 128) ? 7 : (ph == 64) ? 6 : 5;
		pwMask = pw - 1;
		phMask = ph - 1;
		// Same integer arithmetic as CPixelIndexor (width is in units of 64 pixels).
		pagesPerRow = (bufWidth * 64) / pw;
		base = bufPtr;
		return true;
	}

	// Byte offset of the start of row y's pages (before adding the column page and table offset).
	inline uint32 RowBase(uint32 y) const
	{
		return base + (((y >> phShift) * pagesPerRow) << PAGE_SHIFT);
	}

	inline const uint32* RowTable(uint32 y) const
	{
		return table + ((y & phMask) << pwShift);
	}

	// Byte offset in GS RAM (not valid for nibble surfaces).
	inline uint32 Offset(uint32 rowBase, const uint32* rowTable, uint32 x) const
	{
		return (rowBase + ((x >> pwShift) << PAGE_SHIFT) + rowTable[x & pwMask]) & RAM_MASK;
	}

	inline uint32 Offset(uint32 x, uint32 y) const
	{
		return Offset(RowBase(y), RowTable(y), x);
	}

	// PSMT4: returns the nibble index in GS RAM (byte = n >> 1, high nibble if n & 1).
	inline uint32 NibbleOffset(uint32 x, uint32 y) const
	{
		uint32 pageStart = (base + (((x >> pwShift) + (y >> phShift) * pagesPerRow) << PAGE_SHIFT)) * 2;
		return (pageStart + table[((y & phMask) << pwShift) + (x & pwMask)]) & (RAM_MASK * 2 + 1);
	}

	uint32 FirstPage() const
	{
		return (base >> PAGE_SHIFT) & 511;
	}
};
