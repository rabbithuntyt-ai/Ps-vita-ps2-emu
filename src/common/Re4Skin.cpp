#include "Re4Skin.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <vector>
#include "Re4Vu1Batch.h"
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


	// The registers the program leaves that do not depend on its vertices (the vertex loop's
	// leftovers do not matter to anything after it: PC experiment --skin-sparse-state, RE4's walk
	// pictures identical)
	VU_AOT_REGS SparseEndState(const uint8_t* mem, uint32_t address, const VU_AOT_REGS& start)
	{
		VU_AOT_REGS sparse = start;
		bool setup = (address == 0x0000) || (address == 0x0050);
		auto loadVf = [&](int reg, uint32_t qword) { std::memcpy(sparse.vf[reg].u, mem + (qword & 0x3FF) * 16, 16); };
		if(setup)
		{
			uint32_t vi5 = (address == 0x0000) ? 0x29B : 0x2E6;
			sparse.vi[5] = vi5;
			uint32_t word[4];
			std::memcpy(word, mem + (vi5 - 3) * 16, 16);
			sparse.vi[3] = word[1] & 0xFFFF;
			sparse.vi[14] = word[2] & 0xFFFF;
			sparse.vi[13] = word[3] & 0xFFFF;
			loadVf(30, 0), loadVf(4, 1), loadVf(15, 2), loadVf(17, 3);
			loadVf(1, 4), loadVf(2, 5), loadVf(5, 6), loadVf(6, 7);
			loadVf(7, sparse.vi[3]), loadVf(13, sparse.vi[3] + 1);
		}
		sparse.vi[9] = start.top;
		sparse.vi[2] = start.top + 0xB4;
		float one = 1.0f;
		uint32_t oneBits;
		std::memcpy(&oneBits, &one, 4);
		std::memcpy(sparse.vf[31].u, mem + ((start.top + 0x20) & 0x3FF) * 16, 16);
		sparse.vf[31].u[0] = sparse.vf[31].u[1] = sparse.vf[31].u[2] = oneBits;
		return sparse;
	}

	Re4Vu1::VEC4 LoadVec(const uint8_t* mem, uint32_t qword)
	{
		Re4Vu1::VEC4 v;
		std::memcpy(&v, mem + (qword & 0x3FF) * 16, 16);
		return v;
	}

	void LoadInts(const uint8_t* mem, uint32_t qword, int32_t out[4])
	{
		std::memcpy(out, mem + (qword & 0x3FF) * 16, 16);
	}

	// gpu_skin: the batch as a world-path batch, every vertex computed here with the program's
	// own arithmetic (Re4SkinModel.cpp: bit exact against the native program): the projected
	// position (float bits), the final colour, the texture numerators and the skip bit
	std::shared_ptr<Re4Vu1::WORLD_BATCH> BuildGpuBatch(const uint8_t* mem, uint32_t address, const VU_AOT_REGS& r)
	{
		bool setup = (address == 0x0000) || (address == 0x0050);
		uint32_t vi5 = (address == 0x0000) ? 0x29B : (address == 0x0050) ? 0x2E6 : (r.vi[5] & 0x3FF);
		int32_t setupWord[4];
		LoadInts(mem, vi5 - 3, setupWord);
		uint32_t vi3 = setup ? (static_cast<uint32_t>(setupWord[1]) & 0xFFFF) : (r.vi[3] & 0x3FF);
		bool swapped = setup ? (setupWord[0] != 0) : (address == 0x07C8);
		uint32_t top = r.top;
		int32_t header[4];
		LoadInts(mem, top + 0x20, header);
		uint32_t count = static_cast<uint32_t>(header[0]) & 0x7FFF;
		uint32_t nreg = (static_cast<uint32_t>(header[1]) >> 28) & 0xF;
		uint32_t flg = (static_cast<uint32_t>(header[1]) >> 26) & 3;
		if((count == 0) || (nreg != 3) || (flg != 0) || ((static_cast<uint32_t>(header[2]) & 0xFFF) != 0x512)) return nullptr;
		Re4Vu1::VEC4 proj[4], clip[4], lightDir, lightColor;
		auto reg = [&r](int i) {
			Re4Vu1::VEC4 v;
			std::memcpy(&v, r.vf[i].u, 16);
			return v;
		};
		if(setup)
		{
			for(int i = 0; i < 4; i++)
			{
				proj[i] = LoadVec(mem, i);
				clip[i] = LoadVec(mem, 4 + i);
			}
			lightDir = LoadVec(mem, vi3);
			lightColor = LoadVec(mem, vi3 + 1);
		}
		else
		{
			proj[0] = reg(30), proj[1] = reg(4), proj[2] = reg(15), proj[3] = reg(17);
			clip[0] = reg(1), clip[1] = reg(2), clip[2] = reg(5), clip[3] = reg(6);
			lightDir = reg(7), lightColor = reg(13);
		}
		Re4Vu1::VEC4 ambient = LoadVec(mem, vi5 - 2);
		ambient.w = 128.0f;
		Re4Vu1::VEC4 material = LoadVec(mem, vi5 - 1);
		float scale = LoadVec(mem, top + 0x20).w;
		float normalScale;
		const uint32_t normalScaleBits = 0x3C0101FE; //the program's I constant (1/127)
		std::memcpy(&normalScale, &normalScaleBits, 4);

		auto batch = Re4Vu1::AcquireBatch();
		batch->skin = true;
		batch->clipSpace = true;
		batch->tagLow = static_cast<uint64_t>(static_cast<uint32_t>(header[0])) | (static_cast<uint64_t>(static_cast<uint32_t>(header[1])) << 32);
		batch->count = count;
		batch->scale = 1.0f;
		for(int c = 0; c < 4; c++)
		{
			batch->projection[c] = {(c == 0) ? 1.0f : 0.0f, (c == 1) ? 1.0f : 0.0f, (c == 2) ? 1.0f : 0.0f, (c == 3) ? 1.0f : 0.0f};
			batch->clip[c] = batch->projection[c];
		}
		batch->colorOffset = {0, 0, 0, 1};
		batch->colorScale = {1, 1, 1, 1};
		batch->vf31 = {0, 0, 0, 0};
		batch->raw.resize(count * 12);
		uint32_t clipHistory = r.clip;
		// Vertices come in runs that share a bone list: its blended matrix is made once per run
		// (the same products and sums in the same order as per vertex)
		uint32_t cachedRecord = ~0U;
		Re4Vu1::VEC4 mat[4] = {};
		for(uint32_t k = 0; k < count; k++)
		{
			int32_t in0[4], in1[4], in2[4];
			LoadInts(mem, top + 0x21 + 3 * k, in0);
			LoadInts(mem, top + 0x22 + 3 * k, in1);
			LoadInts(mem, top + 0x23 + 3 * k, in2);
			// Bone blend
			uint32_t record = top + (static_cast<uint32_t>(in0[3]) & 0xFFFF);
			if(record != cachedRecord)
			{
			cachedRecord = record;
			int32_t indices[4];
			LoadInts(mem, record, indices);
			Re4Vu1::VEC4 w = LoadVec(mem, record + 1);
			float weights[4] = {w.x, w.y, w.z, w.w};
			uint32_t bones = std::min<uint32_t>(static_cast<uint32_t>(indices[3]) & 0xFFFF, 3);
			for(int c = 0; c < 4; c++) mat[c] = {};
			for(uint32_t b = 0; b < bones; b++)
			{
				uint32_t base = vi5 + (static_cast<uint32_t>(indices[b]) & 0xFFFF);
				for(int c = 0; c < 4; c++)
				{
					Re4Vu1::VEC4 col = LoadVec(mem, base + c);
					mat[c].x += col.x * weights[b];
					mat[c].y += col.y * weights[b];
					mat[c].z += col.z * weights[b];
					mat[c].w += col.w * weights[b];
				}
			}
			}
			Re4Vu1::VEC4 pos = {static_cast<float>(in0[0]) * scale, static_cast<float>(in0[1]) * scale, static_cast<float>(in0[2]) * scale, 1.0f};
			Re4Vu1::VEC4 world = Re4Vu1::Transform(mat, pos);
			Re4Vu1::VEC4 v = Re4Vu1::Transform(proj, world);
			Re4Vu1::VEC4 kc = Re4Vu1::Transform(clip, world);
			clipHistory = ((clipHistory << 6) | Re4Vu1::ClipFlags(kc)) & 0xFFFFFF;
			bool skip = ((clipHistory & 0x3FFFF) != 0) || (in1[3] != 0);
			int32_t* out = batch->raw.data() + k * 12;
			std::memcpy(out, &v, 16);
			if(swapped)
				out[4] = in2[1], out[5] = in2[0], out[6] = 1;
			else
				out[4] = in2[0], out[5] = in2[1], out[6] = in2[2];
			out[7] = skip ? 1 : 0;
			Re4Vu1::VEC4 nrm = {static_cast<float>(in1[0]) * normalScale, static_cast<float>(in1[1]) * normalScale, static_cast<float>(in1[2]) * normalScale, 0.0f};
			Re4Vu1::VEC4 n = Re4Vu1::Transform(mat, nrm);
			float d = (lightDir.x * n.x + lightDir.y * n.y) + lightDir.z * n.z;
			if(!(d > 0.0f)) d = 0.0f;
			float color[4] = {(lightColor.x * d + ambient.x) * material.x, (lightColor.y * d + ambient.y) * material.y,
			                  (lightColor.z * d + ambient.z) * material.z, ambient.w * material.w};
			for(int i = 0; i < 4; i++) out[8 + i] = Re4Vu1::Ftoi(std::fmin(color[i], 255.0f), 1.0f);
		}
		Re4Vu1::PrepareBatchEnd(*batch);
		return batch;
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
	if(g_gpuSkin && CVpu::g_customKickSubmit && !g_modelCheck)
	{
		if(auto batch = BuildGpuBatch(vpu.GetVuMemory(), address, r))
		{
			Store(SparseEndState(vpu.GetVuMemory(), address, r), state);
			g_gpuBatches++;
			g_gpuVertices += batch->count;
			vpu.QueueCustomKick(std::move(batch));
			g_stats.runs++;
			return true;
		}
		g_nativeBatches++;
	}
	std::vector<uint8_t> before;
	VU_AOT_REGS start;
	if(g_modelCheck)
	{
		before.assign(vpu.GetVuMemory(), vpu.GetVuMemory() + vpu.GetVuMemorySize());
		start = r;
	}
	if(!Re4SkinProgram(r, vpu.GetVuMemory(), address, Kick, &vpu)) return false;
	if(g_modelCheck) CheckModel(before.data(), vpu.GetVuMemory(), start, address);
	if(g_sparseState)
		Store(SparseEndState(vpu.GetVuMemory(), address, start), state);
	else
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
