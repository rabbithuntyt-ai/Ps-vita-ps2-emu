// ARM code the Vita's JIT makes for EE blocks, on a PC: compiles blocks from an EE
// RAM dump (dump_ee_ram=1, or the host runner's --dump-ram) with the AArch32 code
// generator in the Vita app's modes (chained frames, fused guest memory ops,
// sign extension tracking) and writes the code for arm-linux-gnueabihf-objdump.
// Nothing is executed, so it runs on any host. tools/jit_arm_report.py counts
// ARM instructions per EE instruction over a game's hot blocks with it.
//
// Usage: jit_arm_dump eeram.bin ADDRESS [ADDRESS...] [--out DIR] [--unchained]
//   One block per address (to its branch and delay slot, like the EE executor);
//   prints "address end guest_instructions arm_bytes" per block and writes
//   DIR/ADDRESS.bin with the code when --out is given.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "MIPS.h"
#include "COP_SCU.h"
#include "COP_FPU.h"
#include "ee/COP_VU.h"
#include "ee/MA_EE.h"
#include "ee/EeBasicBlock.h"
#include "MipsJitter.h"
#include "MemStream.h"
#include "Jitter_CodeGen_AArch32.h"
#include "offsetof_def.h"
#include "Ps2Const.h"

int main(int argc, char** argv)
{
	if(argc < 3)
	{
		std::fprintf(stderr, "usage: %s eeram.bin ADDRESS [ADDRESS...] [--out DIR] [--unchained]\n", argv[0]);
		return 2;
	}
	std::string outDir;
	bool chained = true;
	bool regions = false;
	uint32 findStart = 0, findEnd = 0;
	std::vector<uint32> addresses;
	for(int i = 2; i < argc; i++)
	{
		if(!std::strcmp(argv[i], "--out") && (i + 1 < argc)) outDir = argv[++i];
		else if(!std::strcmp(argv[i], "--unchained")) chained = false;
		else if(!std::strcmp(argv[i], "--region")) regions = true; //compile a tier 2 region where there is one
		else if(!std::strcmp(argv[i], "--find-regions") && (i + 2 < argc))
		{
			findStart = static_cast<uint32>(std::strtoul(argv[++i], nullptr, 16));
			findEnd = static_cast<uint32>(std::strtoul(argv[++i], nullptr, 16));
		}
		else addresses.push_back(static_cast<uint32>(std::strtoul(argv[i], nullptr, 16)));
	}

	std::vector<uint8> ram(PS2::EE_RAM_SIZE);
	if(FILE* file = std::fopen(argv[1], "rb"))
	{
		size_t read = std::fread(ram.data(), 1, ram.size(), file);
		std::fclose(file);
		if(read != ram.size()) std::fprintf(stderr, "warning: %s is %zu bytes\n", argv[1], read);
	}
	else
	{
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}

	// An EE context like Ee::CSubSystem's, RAM only.
	CMIPS ee(MEMORYMAP_ENDIAN_LSBF, true);
	CMA_EE arch;
	CCOP_SCU scu(MIPS_REGSIZE_64);
	CCOP_FPU fpu(MIPS_REGSIZE_64);
	CCOP_VU vu(MIPS_REGSIZE_64);
	ee.m_pMemoryMap->InsertReadMap(0x00000000, PS2::EE_RAM_SIZE - 1, ram.data(), 0x00);
	ee.m_pMemoryMap->InsertWriteMap(0x00000000, PS2::EE_RAM_SIZE - 1, ram.data(), 0x00);
	ee.m_pMemoryMap->InsertInstructionMap(0x00000000, PS2::EE_RAM_SIZE - 1, ram.data(), 0x00);
	ee.m_pArch = &arch;
	ee.m_pCOP[0] = &scu;
	ee.m_pCOP[1] = &fpu;
	ee.m_pCOP[2] = &vu;
	ee.MapPages(0x00000000, PS2::EE_RAM_SIZE, ram.data());

	// The Vita app's modes (CEmuSession::SetSafeJit(false))
	Jitter::CCodeGen_AArch32::SetChainedFrames(chained);
	Jitter::CCodeGen_AArch32::SetGuestMemoryOps(true);
	Jitter::CCodeGen_AArch32::SetConservativeAllocation(false);
	// JIT_NO_FAST_RAM: page lookups for RAM too (fast_ram=0)
	bool fastRam = (std::getenv("JIT_NO_FAST_RAM") == nullptr);
	ee.m_fastRamBase = ram.data();
	ee.m_fastRamKind = fastRam ? 1 : 0;
	if(std::getenv("JIT_NO_PINNED") == nullptr)
		Jitter::CCodeGen_AArch32::SetPinnedFields(offsetof(CMIPS, m_State.cycleQuota), fastRam ? offsetof(CMIPS, m_fastRamBase) : offsetof(CMIPS, m_pageLookup));
	CMA_MIPSIV::SetSignExtTracking(true);
	Jitter::CCodeGen_AArch32::SetExceptionPoison(offsetof(CMIPS, m_State.nHasException));
	CBasicBlock::EnableMemoryExceptionPoison(chained && Jitter::CCodeGen_AArch32::GetExceptionPoison());

	if(findEnd != 0)
	{
		//Region starts in [findStart, findEnd): the parts of each
		for(uint32 address = findStart; address < findEnd; address += 4)
		{
			auto parts = CEeRegionBlock::Find(ee, address, 32, 256);
			if(parts.empty()) continue;
			std::printf("region %08X:", address);
			for(const auto& part : parts) std::printf(" [%08X-%08X ->%d%s]", part.begin, part.end, part.target, part.unconditional ? " j" : "");
			std::printf("\n");
		}
		return 0;
	}

	for(uint32 start : addresses)
	{
		start &= ~3U;
		if(regions)
		{
			auto parts = CEeRegionBlock::Find(ee, start, 32, 256);
			if(!parts.empty())
			{
				uint32 end = parts.back().end;
				CEeRegionBlock region(ee, parts, BLOCK_CATEGORY_PS2_EE);
				Framework::CMemStream stream;
				{
					CMipsJitter jitter(new Jitter::CCodeGen_AArch32());
					jitter.SetStream(&stream);
					jitter.Begin();
					jitter.SetGuestFastRam(ee.m_fastRamKind);
					region.CompileRange(&jitter);
					jitter.End();
				}
				std::printf("%08X %08X %u %u region\n", start, end, ((end - start) / 4) + 1, static_cast<unsigned int>(stream.GetSize()));
				if(!outDir.empty())
				{
					char path[1024];
					std::snprintf(path, sizeof(path), "%s/%08X.bin", outDir.c_str(), start);
					if(FILE* out = std::fopen(path, "wb"))
					{
						std::fwrite(stream.GetBuffer(), 1, stream.GetSize(), out);
						std::fclose(out);
					}
				}
				continue;
			}
		}
		// The block ends after the first branch's delay slot (CGenericMipsExecutor::PartitionFunction).
		uint32 end = start;
		for(uint32 address = start; address < start + 0x1000; address += 4)
		{
			uint32 opcode = ee.m_pMemoryMap->GetInstruction(address);
			auto type = arch.IsInstructionBranch(&ee, address, opcode);
			end = address;
			if(type == MIPS_BRANCH_NORMAL)
			{
				end = address + 4;
				break;
			}
			if(type == MIPS_BRANCH_NODELAY) break;
		}
		CEeBasicBlock block(ee, start, end, BLOCK_CATEGORY_PS2_EE);
		Framework::CMemStream stream;
		{
			CMipsJitter jitter(new Jitter::CCodeGen_AArch32());
			jitter.SetStream(&stream);
			jitter.Begin();
			jitter.SetGuestFastRam(ee.m_fastRamKind);
			block.CompileRange(&jitter);
			jitter.End();
		}
		std::printf("%08X %08X %u %u\n", start, end, ((end - start) / 4) + 1, static_cast<unsigned int>(stream.GetSize()));
		if(!outDir.empty())
		{
			char path[1024];
			std::snprintf(path, sizeof(path), "%s/%08X.bin", outDir.c_str(), start);
			if(FILE* out = std::fopen(path, "wb"))
			{
				std::fwrite(stream.GetBuffer(), 1, stream.GetSize(), out);
				std::fclose(out);
			}
		}
	}
	return 0;
}
