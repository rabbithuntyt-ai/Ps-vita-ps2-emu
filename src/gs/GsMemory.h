#pragma once

// Raw access to swizzled GS local memory, keyed by pixel storage mode (PSM).
// These are thin wrappers over Play!'s CGsPixelFormats indexors so that the
// software renderer can address any buffer format through one interface.

#include "gs/GSHandler.h"
#include "gs/GsPixelFormats.h"

namespace GsMemory
{
	// bufPtr is in bytes, bufWidth is in units of 64 pixels (the raw register field).
	template <typename Storage>
	inline typename Storage::Unit* Address(uint8* ram, uint32 bufPtr, uint32 bufWidth, uint32 x, uint32 y)
	{
		CGsPixelFormats::CPixelIndexor<Storage> indexor(ram, bufPtr, bufWidth);
		return indexor.GetPixelAddress(x, y);
	}

	// Reads a raw storage unit. 24-bit formats return the full 32-bit word,
	// 4-bit formats return the nibble, 8H/4HL/4HH return the full 32-bit word.
	inline uint32 ReadRaw(uint8* ram, uint32 psm, uint32 bufPtr, uint32 bufWidth, uint32 x, uint32 y)
	{
		x &= 2047;
		y &= 2047;
		switch(psm)
		{
		default:
		case CGSHandler::PSMCT32:
		case CGSHandler::PSMCT24:
		case CGSHandler::PSMCT24_UNK:
		case CGSHandler::PSMCT32_UNK:
		case CGSHandler::PSMT8H:
		case CGSHandler::PSMT4HL:
		case CGSHandler::PSMT4HH:
			return *Address<CGsPixelFormats::STORAGEPSMCT32>(ram, bufPtr, bufWidth, x, y);
		case CGSHandler::PSMZ32:
		case CGSHandler::PSMZ24:
			return *Address<CGsPixelFormats::STORAGEPSMZ32>(ram, bufPtr, bufWidth, x, y);
		case CGSHandler::PSMCT16:
			return *Address<CGsPixelFormats::STORAGEPSMCT16>(ram, bufPtr, bufWidth, x, y);
		case CGSHandler::PSMCT16S:
			return *Address<CGsPixelFormats::STORAGEPSMCT16S>(ram, bufPtr, bufWidth, x, y);
		case CGSHandler::PSMZ16:
			return *Address<CGsPixelFormats::STORAGEPSMZ16>(ram, bufPtr, bufWidth, x, y);
		case CGSHandler::PSMZ16S:
			return *Address<CGsPixelFormats::STORAGEPSMZ16S>(ram, bufPtr, bufWidth, x, y);
		case CGSHandler::PSMT8:
			return *Address<CGsPixelFormats::STORAGEPSMT8>(ram, bufPtr, bufWidth, x, y);
		case CGSHandler::PSMT4:
		{
			CGsPixelFormats::CPixelIndexorPSMT4 indexor(ram, bufPtr, bufWidth);
			return indexor.GetPixel(x, y);
		}
		}
	}

	// Writes a raw storage unit, honoring the bits a format actually owns
	// (24-bit formats keep the upper byte, 8H/4HL/4HH only touch their bits).
	inline void WriteRaw(uint8* ram, uint32 psm, uint32 bufPtr, uint32 bufWidth, uint32 x, uint32 y, uint32 value)
	{
		x &= 2047;
		y &= 2047;
		switch(psm)
		{
		default:
		case CGSHandler::PSMCT32:
		case CGSHandler::PSMCT32_UNK:
			*Address<CGsPixelFormats::STORAGEPSMCT32>(ram, bufPtr, bufWidth, x, y) = value;
			break;
		case CGSHandler::PSMCT24:
		case CGSHandler::PSMCT24_UNK:
		{
			auto pixel = Address<CGsPixelFormats::STORAGEPSMCT32>(ram, bufPtr, bufWidth, x, y);
			*pixel = (*pixel & 0xFF000000) | (value & 0x00FFFFFF);
		}
		break;
		case CGSHandler::PSMT8H:
		{
			auto pixel = Address<CGsPixelFormats::STORAGEPSMCT32>(ram, bufPtr, bufWidth, x, y);
			*pixel = (*pixel & 0x00FFFFFF) | (value << 24);
		}
		break;
		case CGSHandler::PSMT4HL:
		{
			auto pixel = Address<CGsPixelFormats::STORAGEPSMCT32>(ram, bufPtr, bufWidth, x, y);
			*pixel = (*pixel & ~0x0F000000) | ((value & 0xF) << 24);
		}
		break;
		case CGSHandler::PSMT4HH:
		{
			auto pixel = Address<CGsPixelFormats::STORAGEPSMCT32>(ram, bufPtr, bufWidth, x, y);
			*pixel = (*pixel & ~0xF0000000) | ((value & 0xF) << 28);
		}
		break;
		case CGSHandler::PSMZ32:
			*Address<CGsPixelFormats::STORAGEPSMZ32>(ram, bufPtr, bufWidth, x, y) = value;
			break;
		case CGSHandler::PSMZ24:
		{
			auto pixel = Address<CGsPixelFormats::STORAGEPSMZ32>(ram, bufPtr, bufWidth, x, y);
			*pixel = (*pixel & 0xFF000000) | (value & 0x00FFFFFF);
		}
		break;
		case CGSHandler::PSMCT16:
			*Address<CGsPixelFormats::STORAGEPSMCT16>(ram, bufPtr, bufWidth, x, y) = static_cast<uint16>(value);
			break;
		case CGSHandler::PSMCT16S:
			*Address<CGsPixelFormats::STORAGEPSMCT16S>(ram, bufPtr, bufWidth, x, y) = static_cast<uint16>(value);
			break;
		case CGSHandler::PSMZ16:
			*Address<CGsPixelFormats::STORAGEPSMZ16>(ram, bufPtr, bufWidth, x, y) = static_cast<uint16>(value);
			break;
		case CGSHandler::PSMZ16S:
			*Address<CGsPixelFormats::STORAGEPSMZ16S>(ram, bufPtr, bufWidth, x, y) = static_cast<uint16>(value);
			break;
		case CGSHandler::PSMT8:
			*Address<CGsPixelFormats::STORAGEPSMT8>(ram, bufPtr, bufWidth, x, y) = static_cast<uint8>(value);
			break;
		case CGSHandler::PSMT4:
		{
			CGsPixelFormats::CPixelIndexorPSMT4 indexor(ram, bufPtr, bufWidth);
			indexor.SetPixel(x, y, static_cast<uint8>(value & 0xF));
		}
		break;
		}
	}

	inline bool IsPsm16(uint32 psm)
	{
		return (psm == CGSHandler::PSMCT16) || (psm == CGSHandler::PSMCT16S) ||
		       (psm == CGSHandler::PSMZ16) || (psm == CGSHandler::PSMZ16S);
	}

	inline bool IsPsm24(uint32 psm)
	{
		return (psm == CGSHandler::PSMCT24) || (psm == CGSHandler::PSMCT24_UNK) || (psm == CGSHandler::PSMZ24);
	}

	// RGBA5551 <-> RGBA8888 (alpha bit maps to 0x80, as on the GS).
	inline uint32 Color16To32(uint32 c)
	{
		return ((c & 0x001F) << 3) | ((c & 0x03E0) << 6) | ((c & 0x7C00) << 9) | ((c & 0x8000) ? 0x80000000 : 0);
	}

	inline uint32 Color32To16(uint32 c)
	{
		return ((c >> 3) & 0x001F) | ((c >> 6) & 0x03E0) | ((c >> 9) & 0x7C00) | ((c >> 16) & 0x8000);
	}
}
