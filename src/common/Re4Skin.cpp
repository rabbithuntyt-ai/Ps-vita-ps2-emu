#include "Re4Skin.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>
#include "MIPS.h"
#include "VuAot.h"
#include "ee/Vif.h"
#include "ee/Vpu.h"

// Re4Skin.gen.cpp
extern const uint32_t Re4SkinProgram_code[];
extern const uint32_t Re4SkinProgram_begin, Re4SkinProgram_end;
int Re4SkinProgram(VU_AOT_REGS& r, uint8_t* mem, uint32_t entry, VU_AOT_KICK kick, void* ctx);

Re4Skin::STATS Re4Skin::g_stats;

namespace
{
	bool IsEntry(uint32_t address)
	{
		return (address == 0x0000) || (address == 0x0050) || (address == 0x0498) || (address == 0x07C8);
	}

	bool CodeMatches(const uint8_t* micro)
	{
		return std::memcmp(micro + Re4SkinProgram_begin, Re4SkinProgram_code, Re4SkinProgram_end - Re4SkinProgram_begin) == 0;
	}

	uint32_t LatestClip(const MIPSSTATE& state)
	{
		return state.pipeClip.values[(state.pipeClip.index - 1) & (FLAG_PIPELINE_SLOTS - 1)];
	}

	uint32_t EffectiveQ(const MIPSSTATE& state)
	{
		return (state.pipeQ.counter != 0) ? state.pipeQ.heldValue : state.nCOP2Q;
	}

	void Load(const MIPSSTATE& state, uint32_t top, VU_AOT_REGS& r)
	{
		for(int i = 0; i < 32; i++) std::memcpy(r.vf[i].u, state.nCOP2[i].nV, 16);
		std::memcpy(r.acc.u, state.nCOP2A.nV, 16);
		for(int i = 0; i < 16; i++) r.vi[i] = state.nCOP2VI[i];
		r.vi[0] = 0;
		r.q.u = EffectiveQ(state);
		r.i.u = state.nCOP2I;
		r.clip = LatestClip(state);
		r.top = top;
	}

	void Store(const VU_AOT_REGS& r, MIPSSTATE& state)
	{
		for(int i = 1; i < 32; i++) std::memcpy(state.nCOP2[i].nV, r.vf[i].u, 16);
		std::memcpy(state.nCOP2A.nV, r.acc.u, 16);
		for(int i = 1; i < 16; i++) state.nCOP2VI[i] = r.vi[i];
		state.nCOP2Q = r.q.u;
		state.pipeQ.heldValue = r.q.u;
		state.pipeQ.counter = 0;
		state.nCOP2I = r.i.u;
		// The newest clip flags, ready (the next program starts at pipe time 0)
		for(unsigned int i = 0; i < FLAG_PIPELINE_SLOTS; i++)
		{
			state.pipeClip.values[i] = r.clip;
			state.pipeClip.pipeTimes[i] = 0;
		}
		state.nCOP2CF = r.clip;
	}

	void Kick(void* ctx, uint32_t address)
	{
		static_cast<CVpu*>(ctx)->ProcessXgKick(address);
	}

	// --skin-verify
	struct VERIFY
	{
		bool armed = false;
		uint32_t address = 0;
		VU_AOT_REGS start;
		std::vector<uint8_t> memory;
		uint64_t badMemory = 0, badVf = 0, badVi = 0, badAcc = 0, badQ = 0, badClip = 0, badKick = 0, notFinished = 0;
		std::string examples;
		int exampleCount = 0;
	};
	VERIFY g_verify;

	struct KICKS
	{
		std::vector<uint32_t> addresses;
	};

	void RecordKick(void* ctx, uint32_t address)
	{
		static_cast<KICKS*>(ctx)->addresses.push_back(address);
	}

	void Example(const char* format, ...)
	{
		auto& v = g_verify;
		if(v.exampleCount >= 40) return;
		v.exampleCount++;
		char text[256];
		va_list args;
		va_start(args, format);
		std::vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		v.examples += text;
	}
}

bool Re4Skin::Run(CVpu& vpu, uint32_t address)
{
	if(!IsEntry(address)) return false;
	if(!CodeMatches(vpu.GetMicroMemory()))
	{
		g_stats.refused++;
		return false;
	}
	auto& state = vpu.GetContext().m_State;
	VU_AOT_REGS r;
	Load(state, vpu.GetVif().GetTOP(), r);
	if(!Re4SkinProgram(r, vpu.GetVuMemory(), address, Kick, &vpu)) return false;
	Store(r, state);
	g_stats.runs++;
	return true;
}

void Re4Skin::VerifyHook(CVpu& vpu, uint32_t address, bool before)
{
	auto& v = g_verify;
	auto& state = vpu.GetContext().m_State;
	if(before)
	{
		v.armed = IsEntry(address) && CodeMatches(vpu.GetMicroMemory());
		if(!v.armed) return;
		v.address = address;
		Load(state, vpu.GetVif().GetTOP(), v.start);
		v.memory.assign(vpu.GetVuMemory(), vpu.GetVuMemory() + vpu.GetVuMemorySize());
		return;
	}
	if(!v.armed) return;
	v.armed = false;
	g_stats.verified++;
	if(vpu.IsVuRunning())
	{
		v.notFinished++;
		return;
	}
	VU_AOT_REGS native = v.start;
	KICKS kicks;
	Re4SkinProgram(native, v.memory.data(), v.address, RecordKick, &kicks);
	VU_AOT_REGS jit;
	Load(state, v.start.top, jit);
	bool bad = false;
	const uint8_t* jitMemory = vpu.GetVuMemory();
	for(uint32_t offset = 0; offset < v.memory.size(); offset += 4)
	{
		if(std::memcmp(jitMemory + offset, v.memory.data() + offset, 4) != 0)
		{
			if(!bad || (v.exampleCount < 20))
			{
				uint32_t a, b;
				std::memcpy(&a, jitMemory + offset, 4);
				std::memcpy(&b, v.memory.data() + offset, 4);
				Example("program %04X top %03X: memory qword %03X.%c jit %08X native %08X\n", v.address, v.start.top, offset >> 4, "xyzw"[(offset >> 2) & 3], a, b);
			}
			if(!bad) v.badMemory++;
			bad = true;
			break;
		}
	}
	for(int i = 1; i < 32; i++)
	{
		for(int l = 0; l < 4; l++)
		{
			if(jit.vf[i].u[l] != native.vf[i].u[l])
			{
				Example("program %04X: VF%d.%c jit %08X native %08X\n", v.address, i, "xyzw"[l], jit.vf[i].u[l], native.vf[i].u[l]);
				v.badVf++;
				bad = true;
				i = 32;
				break;
			}
		}
	}
	for(int i = 1; i < 16; i++)
	{
		if(jit.vi[i] != native.vi[i])
		{
			Example("program %04X: VI%d jit %08X native %08X\n", v.address, i, jit.vi[i], native.vi[i]);
			v.badVi++;
			bad = true;
			break;
		}
	}
	if(std::memcmp(jit.acc.u, native.acc.u, 16) != 0)
	{
		v.badAcc++;
		bad = true;
	}
	if(jit.q.u != native.q.u)
	{
		Example("program %04X: Q jit %08X native %08X\n", v.address, jit.q.u, native.q.u);
		v.badQ++;
		bad = true;
	}
	if(jit.clip != native.clip)
	{
		Example("program %04X: clip jit %08X native %08X\n", v.address, jit.clip, native.clip);
		v.badClip++;
		bad = true;
	}
	if(kicks.addresses.size() != 1)
	{
		v.badKick++;
		bad = true;
	}
	if(bad) g_stats.mismatched++;
}

void Re4Skin::WriteReport(const std::string& path)
{
	auto& v = g_verify;
	std::FILE* file = std::fopen(path.c_str(), "w");
	if(!file) return;
	std::fprintf(file, "skinning programs compared %llu, with differences %llu\n", (unsigned long long)g_stats.verified, (unsigned long long)g_stats.mismatched);
	std::fprintf(file, "memory %llu vf %llu vi %llu acc %llu q %llu clip %llu kicks %llu unfinished %llu\n",
	             (unsigned long long)v.badMemory, (unsigned long long)v.badVf, (unsigned long long)v.badVi, (unsigned long long)v.badAcc,
	             (unsigned long long)v.badQ, (unsigned long long)v.badClip, (unsigned long long)v.badKick, (unsigned long long)v.notFinished);
	std::fputs(v.examples.c_str(), file);
	std::fclose(file);
}
