// Measures the cost of Play!'s SPU2 mixer: both cores with 24 looping ADPCM
// voices each and reverb, like a busy game scene. Reports host time per
// second of audio (multiply by the host/Vita speed ratio for the Vita cost).
//
//   spu_benchmark [seconds-of-audio] [--silent]   (--silent: no voice keyed on)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
#include "iop/Iop_SpuBase.h"

using namespace Iop;

int main(int argc, char** argv)
{
	double seconds = (argc > 1) ? std::atof(argv[1]) : 2.0;
	bool silent = (argc > 2) && !std::strcmp(argv[2], "--silent");
	const uint32 ramSize = 2 * 1024 * 1024;
	std::vector<uint8> ram(ramSize);

	// A looping ADPCM sample per voice: 64 blocks of 16 bytes (112 samples each).
	std::mt19937 rng(1);
	for(uint32 voice = 0; voice < 48; voice++)
	{
		uint32 base = 0x10000 + voice * 0x400;
		for(uint32 block = 0; block < 64; block++)
		{
			uint8* p = ram.data() + base + block * 16;
			p[0] = static_cast<uint8>((rng() % 5) << 4 | (rng() % 12)); // filter, shift
			p[1] = (block == 0) ? 0x04 : (block == 63) ? 0x03 : 0x00;   // loop start / loop end+repeat
			for(int i = 2; i < 16; i++) p[i] = static_cast<uint8>(rng());
		}
	}

	CSpuSampleCache cache;
	CSpuIrqWatcher irqWatcher;
	CSpuBase core0(ram.data(), ramSize, &cache, &irqWatcher, 0);
	CSpuBase core1(ram.data(), ramSize, &cache, &irqWatcher, 1);
	CSpuBase* cores[2] = {&core0, &core1};
	for(int c = 0; c < 2; c++)
	{
		auto& spu = *cores[c];
		spu.Reset();
		spu.SetDestinationSamplingRate(44100);
		spu.SetBaseSamplingRate(48000);
		spu.SetReverbEnabled(true);
		spu.SetControl(CSpuBase::CONTROL_REVERB | 0x8000);
		spu.SetReverbWorkAddressStart(0x180000);
		spu.SetReverbWorkAddressEnd(0x1FFFFF);
		// A plausible "hall" preset (word offsets / coefficients).
		const uint32 params[] = {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0,
		                         0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E,
		                         0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080,
		                         0x8000, 0x8000};
		for(unsigned int i = 0; i < CSpuBase::REVERB_REG_COUNT && i < sizeof(params) / sizeof(params[0]); i++)
			spu.SetReverbParam(i, params[i]);
		spu.SetChannelReverbLo(0xFFFF);
		spu.SetChannelReverbHi(0x00FF);
		for(unsigned int v = 0; v < 24; v++)
		{
			auto& channel = spu.GetChannel(v);
			channel.address = 0x10000 + (c * 24 + v) * 0x400;
			channel.pitch = static_cast<uint16>(0x0800 + (rng() % 0x1000));
			channel.volumeLeft <<= static_cast<uint16>(0x3000);
			channel.volumeRight <<= static_cast<uint16>(0x2000);
			channel.volumeLeftAbs = 0x3000 << 17;
			channel.volumeRightAbs = 0x2000 << 17;
			channel.adsrLevel <<= static_cast<uint16>(0x00FF); // fast attack, sustain level max
			channel.adsrRate <<= static_cast<uint16>(0x1FC0);
			spu.OnChannelPitchChanged(v);
		}
		if(!silent) spu.SendKeyOn(0xFFFFFF);
	}

	const unsigned int blockSize = 128; // stereo samples * 2 (as the VM renders: 64 frames)
	int16 out[blockSize];
	uint64 frames = static_cast<uint64>(seconds * 44100);
	int64 checksum = 0;
	auto start = std::chrono::steady_clock::now();
	for(uint64 done = 0; done < frames; done += blockSize / 2)
	{
		for(auto* spu : cores)
		{
			spu->Render(out, blockSize);
			for(unsigned int i = 0; i < blockSize; i++) checksum += out[i];
		}
	}
	double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::printf("SPU2 mix (2 cores x 24 %s voices + reverb): %.1f ms per second of audio (checksum %lld)\n", silent ? "silent" : "playing", elapsed * 1000.0 / seconds,
	            static_cast<long long>(checksum));
	return 0;
}
