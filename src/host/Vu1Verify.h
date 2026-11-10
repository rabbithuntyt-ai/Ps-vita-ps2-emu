#pragma once

// PC runner --vu1-verify: checks Re4Vu1's description of Resident Evil 4's world vertex
// program against the JIT, batch by batch (inline VU1 only). Writes vu1verify.txt.

#include <cstdio>
#include <string>
#include <vector>
#include "Re4Vu1.h"
#include "ee/Vpu.h"
#include "ee/Vif.h"

namespace Vu1Verify
{
	struct STATE
	{
		bool armed = false;
		uint32 top = 0, vi5 = 0;
		Re4Vu1::VEC4 vf31 = {};
		uint64 batches = 0, vertices = 0, skipped = 0, calls1408 = 0, matched = 0;
		uint64 badSt = 0, badRgba = 0, badXyz = 0;
		uint64 adcConfusion[2][2] = {}; //[predicted][actual]
		uint64 adcCause[8] = {};        //actual ADC by predicted cause bits (restart 1, back face 2, clip 4)
		std::string adcExamples;
		std::string examples;
	};
	inline STATE g_state;

	// The world program's code at 0x1408 (B 0x1040) and the start of its loop.
	inline bool IsWorldProgram(const uint8* micro)
	{
		uint32 at1408 = *reinterpret_cast<const uint32*>(micro + 0x1408);
		uint32 at12B8 = *reinterpret_cast<const uint32*>(micro + 0x12B8 + 4);
		return (at1408 == 0x40000786) && (at12B8 == 0x01FE69BC);
	}

	inline void Hook(CVpu& vpu, uint32 address, bool before)
	{
		auto& s = g_state;
		const uint8* micro = vpu.GetMicroMemory();
		const uint8* mem = vpu.GetVuMemory();
		if(before)
		{
			if(address == 0x1408) s.calls1408++;
			if((address == 0x1408) && IsWorldProgram(micro)) s.matched++;
			s.armed = (address == 0x1408) && IsWorldProgram(micro);
			if(!s.armed) return;
			auto& state = vpu.GetContext().m_State;
			s.top = vpu.GetVif().GetTOP();
			s.vi5 = state.nCOP2VI[5] & 0xFFFF;
			std::memcpy(&s.vf31, &state.nCOP2[31], 16);
			return;
		}
		if(!s.armed) return;
		s.armed = false;
		s.batches++;
		uint32 count = *reinterpret_cast<const uint32*>(mem + ((s.top + 0x20) * 16 & 0x3FFF)) & 0x7FFF;
		uint32 out = s.top + 0xB4;
		std::vector<Re4Vu1::WORLD_VERTEX_OUT> all(count);
		std::vector<uint32> clip(count);
		for(uint32 i = 0; i < count; i++)
		{
			all[i] = Re4Vu1::TransformWorldVertex(mem, s.top, s.vi5, s.vf31, i);
			clip[i] = Re4Vu1::WorldClipFlags(mem, s.top, s.vi5, i);
		}
		for(uint32 i = 0; i < count; i++)
		{
			int32 actualW = *reinterpret_cast<const int32*>(mem + (((out + 3 + 3 * i) * 16 + 12) & 0x3FFF));
			bool actualAdc = (actualW & 0x8000) != 0;
			uint32 cause = 0;
			if(all[i].restart != 0) cause |= 1;
			if(i >= 2)
			{
				const auto &v0 = all[i - 2], &v1 = all[i - 1], &v2 = all[i];
				float ax = v1.screenX - v0.screenX, ay = v1.screenY - v0.screenY;
				float bx = v2.screenX - v0.screenX, by = v2.screenY - v0.screenY;
				float cross = 2.0f * (bx * ay - by * ax) * v2.windingSign;
				if(cross < 0) cause |= 2;
				if(clip[i - 2] | clip[i - 1] | clip[i]) cause |= 4;
			}
			bool predicted = cause != 0;
			s.adcConfusion[predicted ? 1 : 0][actualAdc ? 1 : 0]++;
			if(actualAdc) s.adcCause[cause]++;
			if((predicted != actualAdc) && (s.adcExamples.size() < 4000))
			{
				char text[256];
				std::snprintf(text, sizeof(text), "batch %llu v%u/%u: actual %d cause %u w %08X restart %d clip %X %X %X sign %g\n", static_cast<unsigned long long>(s.batches + 1), i, count, actualAdc ? 1 : 0, cause, actualW, all[i].restart,
				              (i >= 2) ? clip[i - 2] : 0, (i >= 1) ? clip[i - 1] : 0, clip[i], all[i].windingSign);
				s.adcExamples += text;
			}
		}
		for(uint32 i = 0; i < count; i++)
		{
			const auto& expected = all[i];
			float st[4];
			uint32 rgba[4];
			int32 xyz[4];
			std::memcpy(st, mem + (((out + 1 + 3 * i) * 16) & 0x3FFF), 16);
			std::memcpy(rgba, mem + (((out + 2 + 3 * i) * 16) & 0x3FFF), 16);
			std::memcpy(xyz, mem + (((out + 3 + 3 * i) * 16) & 0x3FFF), 16);
			s.vertices++;
			bool stOk = true, rgbaOk = true, xyzOk = true;
			for(int k = 0; k < 4; k++)
			{
				float tolerance = std::fabs(expected.st[k]) * 1e-4f + 1e-6f;
				if(!(std::fabs(st[k] - expected.st[k]) <= tolerance)) stOk = false;
				if(std::abs(static_cast<int32>(rgba[k]) - static_cast<int32>(expected.rgba[k])) > 1) rgbaOk = false;
			}
			for(int k = 0; k < 3; k++)
			{
				if(std::abs(xyz[k] - expected.xyz[k]) > 2) xyzOk = false;
			}
			if(!stOk) s.badSt++;
			if(!rgbaOk) s.badRgba++;
			if(!xyzOk) s.badXyz++;
			if((!stOk || !rgbaOk || !xyzOk) && (s.examples.size() < 6000))
			{
				char text[512];
				std::snprintf(text, sizeof(text),
				              "batch %llu v%u/%u top %X: st %g %g %g %g exp %g %g %g %g | rgba %u %u %u %u exp %u %u %u %u | xyz %d %d %d %08X exp %d %d %d %d\n",
				              static_cast<unsigned long long>(s.batches), i, count, s.top, st[0], st[1], st[2], st[3], expected.st[0], expected.st[1], expected.st[2],
				              expected.st[3], rgba[0], rgba[1], rgba[2], rgba[3], expected.rgba[0], expected.rgba[1], expected.rgba[2], expected.rgba[3],
				              xyz[0], xyz[1], xyz[2], xyz[3], expected.xyz[0], expected.xyz[1], expected.xyz[2], expected.xyz[3]);
				s.examples += text;
			}
		}
	}

	inline void Write(const char* path)
	{
		const auto& s = g_state;
		if(std::FILE* file = std::fopen(path, "w"))
		{
			std::fprintf(file, "calls at 1408 %llu, world code %llu\n", static_cast<unsigned long long>(s.calls1408), static_cast<unsigned long long>(s.matched));
			std::fprintf(file, "world batches %llu vertices %llu: st mismatches %llu, rgba %llu, xyz %llu\n",
			             static_cast<unsigned long long>(s.batches), static_cast<unsigned long long>(s.vertices),
			             static_cast<unsigned long long>(s.badSt), static_cast<unsigned long long>(s.badRgba), static_cast<unsigned long long>(s.badXyz));
			std::fprintf(file, "ADC predicted/actual: 0/0 %llu 0/1 %llu 1/0 %llu 1/1 %llu\n", static_cast<unsigned long long>(s.adcConfusion[0][0]),
			             static_cast<unsigned long long>(s.adcConfusion[0][1]), static_cast<unsigned long long>(s.adcConfusion[1][0]), static_cast<unsigned long long>(s.adcConfusion[1][1]));
			for(int c = 0; c < 8; c++) std::fprintf(file, "actual ADC with causes %d (restart 1, back 2, clip 4): %llu\n", c, static_cast<unsigned long long>(s.adcCause[c]));
			std::fputs(s.adcExamples.c_str(), file);
			std::fputs(s.examples.c_str(), file);
			std::fclose(file);
		}
	}
}
