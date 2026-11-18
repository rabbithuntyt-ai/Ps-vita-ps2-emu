// VU micro program disassembly on a PC (Play's VU reflection tables): for reading game
// programs before writing HLE versions (W3: RE4's skinning program).
//
// Usage: vu_dis micromem.bin [BEGIN END]
//   micromem.bin: a VU1 micro memory dump (16 KB; a save state's micromem1 file).
//   BEGIN, END: byte addresses (hex) of the first and last instruction pair to print.
//   Each line: address, upper instruction, lower instruction, raw words, with I/E/M/D/T bits.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "MIPS.h"
#include "ee/MA_VU.h"
#include "Ps2Const.h"

int main(int argc, char** argv)
{
	if(argc < 2)
	{
		std::fprintf(stderr, "usage: %s micromem.bin [BEGIN END]\n", argv[0]);
		return 1;
	}
	std::FILE* file = std::fopen(argv[1], "rb");
	if(!file)
	{
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	std::vector<uint8> micro(PS2::MICROMEM1SIZE);
	size_t size = std::fread(micro.data(), 1, micro.size(), file);
	std::fclose(file);
	uint32 begin = (argc > 2) ? std::strtoul(argv[2], nullptr, 16) : 0;
	uint32 end = (argc > 3) ? std::strtoul(argv[3], nullptr, 16) : static_cast<uint32>(size - 8);
	CMIPS context(MEMORYMAP_ENDIAN_LSBF, PS2::MICROMEM1SIZE - 1);
	CMA_VU vu(PS2::MICROMEM1SIZE - 1);
	// The lower disassembler reads the pair's upper word (I bit) through the context
	context.m_pMemoryMap->InsertInstructionMap(0x00000000, PS2::MICROMEM1SIZE - 1, micro.data(), 0x00);
	context.m_pMemoryMap->InsertReadMap(0x00000000, PS2::MICROMEM1SIZE - 1, micro.data(), 0x00);
	for(uint32 address = begin & ~7U; (address <= end) && (address + 8 <= size); address += 8)
	{
		uint32 lower = 0, upper = 0;
		std::memcpy(&lower, micro.data() + address, 4);
		std::memcpy(&upper, micro.data() + address + 4, 4);
		char um[64] = {}, uo[96] = {}, lm[64] = {}, lo[96] = {};
		vu.GetInstructionMnemonic(&context, address + 4, upper, um, sizeof(um));
		vu.GetInstructionOperands(&context, address + 4, upper, uo, sizeof(uo));
		bool immediate = (upper & 0x80000000) != 0;
		if(immediate)
		{
			float value = 0;
			std::memcpy(&value, &lower, 4);
			std::snprintf(lm, sizeof(lm), "LOI");
			std::snprintf(lo, sizeof(lo), "%g", value);
		}
		else
		{
			vu.GetInstructionMnemonic(&context, address, lower, lm, sizeof(lm));
			vu.GetInstructionOperands(&context, address, lower, lo, sizeof(lo));
		}
		std::printf("%04X  %-8s %-26s | %-8s %-26s  %08X %08X%s%s%s%s\n", address, um, uo, lm, lo, upper, lower,
		            (upper & 0x40000000) ? " E" : "", (upper & 0x20000000) ? " M" : "", (upper & 0x10000000) ? " D" : "", (upper & 0x08000000) ? " T" : "");
	}
	return 0;
}
