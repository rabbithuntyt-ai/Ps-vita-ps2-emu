// ARM code the Vita's JIT makes for VU1 blocks, on a PC: compiles blocks of a VU1 micro
// memory dump (the host runner's --vu1-stats writes vu1micro.bin) with the AArch32 code
// generator and writes the code for arm-linux-gnueabihf-objdump. Nothing is executed.
//
// Usage: vu_arm_dump vu1micro.bin BEGIN END [--out FILE]
//   BEGIN and END: first and last instruction pair of the block (hex, multiples of 8);
//   prints "begin end pairs arm_bytes bytes_per_pair".
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "MIPS.h"
#include "ee/MA_VU.h"
#include "ee/VuBasicBlock.h"
#include "ee/VUShared.h"
#include "MipsJitter.h"
#include "MemStream.h"
#include "Jitter_CodeGen_AArch32.h"
#include "Ps2Const.h"

namespace
{
	class CDumpBlock : public CVuBasicBlock
	{
	public:
		using CVuBasicBlock::CVuBasicBlock;
		void Compile(CMipsJitter* jitter)
		{
			CompileRange(jitter);
		}
	};
}

int main(int argc, char** argv)
{
	if(argc < 4)
	{
		std::fprintf(stderr, "usage: %s vu1micro.bin BEGIN END [--out FILE]\n", argv[0]);
		return 2;
	}
	std::vector<uint8> micro(PS2::MICROMEM1SIZE);
	std::vector<uint8> data(PS2::VUMEM1SIZE);
	if(FILE* file = std::fopen(argv[1], "rb"))
	{
		std::fread(micro.data(), 1, micro.size(), file);
		std::fclose(file);
	}
	else
	{
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	uint32 begin = static_cast<uint32>(std::strtoul(argv[2], nullptr, 16));
	uint32 end = static_cast<uint32>(std::strtoul(argv[3], nullptr, 16)) + 4; //the upper word of the last pair
	std::string out;
	for(int i = 4; i < argc; i++)
	{
		if(!std::strcmp(argv[i], "--out") && (i + 1 < argc)) out = argv[++i];
		else if(!std::strcmp(argv[i], "--no-clamp")) CVuBasicBlock::g_extraCompileHints |= VUShared::COMPILEHINT_NO_CLAMPING;
	}

	// A VU1 context like Ee::CSubSystem's
	CMIPS vu1(MEMORYMAP_ENDIAN_LSBF);
	CMA_VU arch(PS2::VUMEM1SIZE - 1);
	vu1.m_pMemoryMap->InsertReadMap(0x00000000, 0x00003FFF, data.data(), 0x00);
	vu1.m_pMemoryMap->InsertWriteMap(0x00000000, 0x00003FFF, data.data(), 0x00);
	vu1.m_pMemoryMap->InsertInstructionMap(0x00000000, 0x00003FFF, micro.data(), 0x01);
	vu1.m_pArch = &arch;
	vu1.m_pAddrTranslator = CMIPS::TranslateAddress64;
	vu1.m_vuMem = data.data();

	// The Vita app's modes (CEmuSession::SetSafeJit(false))
	Jitter::CCodeGen_AArch32::SetChainedFrames(true);
	Jitter::CCodeGen_AArch32::SetGuestMemoryOps(true);
	Jitter::CCodeGen_AArch32::SetConservativeAllocation(false);

	CDumpBlock block(vu1, begin, end, BLOCK_CATEGORY_PS2_VU);
	Framework::CMemStream stream;
	{
		CMipsJitter jitter(new Jitter::CCodeGen_AArch32());
		jitter.SetStream(&stream);
		jitter.Begin();
		block.Compile(&jitter);
		jitter.End();
	}
	uint32 pairs = ((end - begin) / 8) + 1;
	std::printf("%04X %04X %u %u %.1f\n", begin, end - 4, pairs, static_cast<unsigned int>(stream.GetSize()), static_cast<double>(stream.GetSize()) / pairs);
	if(!out.empty())
	{
		if(FILE* file = std::fopen(out.c_str(), "wb"))
		{
			std::fwrite(stream.GetBuffer(), 1, stream.GetSize(), file);
			std::fclose(file);
		}
	}
	return 0;
}
