// Golden test for Play!'s SPU2 mixer: optimizations of CSpuBase::Render and
// its helpers must produce bit-identical output. Exercises every envelope
// mode, fixed and sweep volumes (with phase), key on/off and pitch changes
// mid-stream, and reverb. The hash covers every output sample and the final
// voice state.
//
//   spu_mixer_tests            check against the recorded hash
//   spu_mixer_tests --print    print the hash (to record a new golden value)

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include "iop/Iop_SpuBase.h"

using namespace Iop;

namespace
{
	// Recorded from the original (unoptimized) Play! mixer.
	constexpr uint64 GOLDEN_HASH = 0x1F2CA13097488D0FULL;

	struct HASH
	{
		uint64 value = 0xCBF29CE484222325ULL;
		void Add(const void* data, size_t size)
		{
			auto bytes = static_cast<const uint8*>(data);
			for(size_t i = 0; i < size; i++)
			{
				value ^= bytes[i];
				value *= 0x100000001B3ULL;
			}
		}
		template <typename T>
		void AddValue(T v)
		{
			Add(&v, sizeof(v));
		}
	};

	uint16 RandomVolume(std::mt19937& rng)
	{
		switch(rng() % 5)
		{
		case 0: return static_cast<uint16>(rng() % 0x4000);                          // fixed
		case 1: return static_cast<uint16>(0x4000 | (rng() % 0x4000));               // fixed, inverted phase
		case 2: return static_cast<uint16>(0x8000 | (rng() % 0x80));                 // linear increase sweep
		case 3: return static_cast<uint16>(0x8000 | 0x2000 | (rng() % 0x80));        // linear decrease sweep
		default: return static_cast<uint16>(0x8000 | 0x4000 | 0x2000 | (rng() % 0x80)); // exponential decrease
		}
	}
}

int main(int argc, char** argv)
{
	bool print = (argc > 1) && !std::strcmp(argv[1], "--print");
	const uint32 ramSize = 2 * 1024 * 1024;
	std::vector<uint8> ram(ramSize);
	std::mt19937 rng(1234);

	// ADPCM samples: some looping, some one-shot (end without repeat).
	for(uint32 voice = 0; voice < 48; voice++)
	{
		uint32 base = 0x10000 + voice * 0x800;
		uint32 blocks = 8 + rng() % 100;
		bool loops = (voice % 3) != 0;
		for(uint32 block = 0; block < blocks; block++)
		{
			uint8* p = ram.data() + base + block * 16;
			p[0] = static_cast<uint8>(((rng() % 5) << 4) | (rng() % 13));
			uint8 flags = 0;
			if(block == 0) flags |= 0x04;
			if(block == blocks - 1) flags |= loops ? 0x03 : 0x01;
			p[1] = flags;
			for(int i = 2; i < 16; i++) p[i] = static_cast<uint8>(rng());
		}
	}

	CSpuSampleCache cache;
	CSpuIrqWatcher irqWatcher;
	CSpuBase core0(ram.data(), ramSize, &cache, &irqWatcher, 0);
	CSpuBase core1(ram.data(), ramSize, &cache, &irqWatcher, 1);
	CSpuBase* cores[2] = {&core0, &core1};

	auto setupVoice = [&](CSpuBase& spu, unsigned int core, unsigned int v) {
		auto& channel = spu.GetChannel(v);
		channel.address = 0x10000 + (core * 24 + v) * 0x800;
		channel.pitch = static_cast<uint16>(0x0400 + (rng() % 0x3000));
		uint16 left = RandomVolume(rng), right = RandomVolume(rng);
		channel.volumeLeft <<= left;
		channel.volumeRight <<= right;
		channel.volumeLeftAbs = (rng() % 0x4000) << 17;
		channel.volumeRightAbs = (rng() % 0x4000) << 17;
		channel.adsrLevel <<= static_cast<uint16>(rng());
		uint16 rate = static_cast<uint16>(rng());
		rate &= ~(1 << 13);
		channel.adsrRate <<= rate;
		spu.OnChannelPitchChanged(v);
	};

	for(unsigned int c = 0; c < 2; c++)
	{
		auto& spu = *cores[c];
		spu.Reset();
		spu.SetDestinationSamplingRate(44100);
		spu.SetBaseSamplingRate(48000);
		spu.SetReverbEnabled(true);
		spu.SetControl(CSpuBase::CONTROL_REVERB | 0x8000);
		spu.SetReverbWorkAddressStart(0x180000 + c * 0x40000);
		spu.SetReverbWorkAddressEnd(0x1BFFFF + c * 0x40000);
		for(unsigned int i = 0; i < CSpuBase::REVERB_REG_COUNT; i++)
			spu.SetReverbParam(i, (i < 10) ? static_cast<uint32>(rng() & 0xFFFF) : static_cast<uint32>(rng() % 0x800));
		spu.SetChannelReverbLo(static_cast<uint16>(rng()));
		spu.SetChannelReverbHi(static_cast<uint16>(rng() & 0xFF));
		for(unsigned int v = 0; v < 24; v++) setupVoice(spu, c, v);
		spu.SendKeyOn(0xFFFFFF & rng());
	}

	HASH hash;
	const unsigned int blockSize = 90; // as the VM renders: 45 stereo frames
	int16 out[blockSize];
	for(unsigned int block = 0; block < 3000; block++)
	{
		for(unsigned int c = 0; c < 2; c++)
		{
			auto& spu = *cores[c];
			if((block % 250) == 100) spu.SendKeyOff(rng() & 0xFFFFFF);
			if((block % 250) == 175)
			{
				uint32 voices = rng() & 0xFFFFFF;
				for(unsigned int v = 0; v < 24; v++)
					if(voices & (1 << v)) setupVoice(spu, c, v);
				spu.SendKeyOn(voices);
			}
			if((block % 300) == 150)
			{
				unsigned int v = rng() % 24;
				spu.GetChannel(v).pitch = static_cast<uint16>(rng() % 0x4000);
				spu.OnChannelPitchChanged(v);
			}
			spu.Render(out, blockSize);
			hash.Add(out, sizeof(out));
		}
	}
	for(auto* spu : cores)
	{
		for(unsigned int v = 0; v < 24; v++)
		{
			const auto& channel = spu->GetChannel(v);
			hash.AddValue(channel.adsrVolume);
			hash.AddValue(channel.status);
			hash.AddValue(channel.current);
			hash.AddValue(channel.volumeLeftAbs);
			hash.AddValue(channel.volumeRightAbs);
		}
	}

	if(print)
	{
		std::printf("0x%016llXULL\n", static_cast<unsigned long long>(hash.value));
		return 0;
	}
	bool ok = (hash.value == GOLDEN_HASH);
	std::printf("SPU mixer hash %016llX: %s\n", static_cast<unsigned long long>(hash.value), ok ? "PASSED" : "FAILED (output changed)");
	return ok ? 0 : 1;
}
