// Time stretcher: at any emulation speed the device must get continuous audio
// at the original pitch, without clicks.

#include <cmath>
#include <cstdio>
#include <vector>
#include "AudioStretcher.h"

namespace
{
	struct RESULT
	{
		double frequency = 0;
		int maxJump = 0;
		unsigned underruns = 0;
		float tempo = 0;
		unsigned buffered = 0;
	};

	// Simulates 'seconds' of real time. The emulator produces audio at
	// 'speed' (1 = full speed) in bursts of one emulated frame (735 frames at
	// 60Hz); the device pulls 1024 frame grains.
	RESULT Run(double speed, double seconds, double toneHz)
	{
		CAudioStretcher stretcher;
		const double rate = 44100;
		double phase = 0;
		double emuTime = 0;   //emulated time produced so far
		double nextPull = 0.1; //device starts after some buffering
		std::vector<int16_t> burst(735 * 2), grain(1024 * 2);
		std::vector<int16_t> output;
		unsigned underrunsAtWarmup = 0;
		bool warm = false;
		for(double t = 0; t < seconds; t += 0.001)
		{
			// Producer: one emulated frame whenever emulated time lags behind.
			while(emuTime < t * speed)
			{
				for(int i = 0; i < 735; i++)
				{
					int16_t v = static_cast<int16_t>(10000 * std::sin(phase));
					phase += 2 * M_PI * toneHz / rate;
					burst[i * 2] = burst[i * 2 + 1] = v;
				}
				stretcher.Push(burst.data(), 735);
				emuTime += 1.0 / 60.0;
			}
			while(t >= nextPull)
			{
				stretcher.Pull(grain.data(), 1024);
				nextPull += 1024 / rate;
				if(t > 1.5)
				{
					if(!warm)
					{
						warm = true;
						underrunsAtWarmup = stretcher.GetStats().underruns;
					}
					output.insert(output.end(), grain.begin(), grain.end());
				}
			}
		}
		RESULT result;
		auto stats = stretcher.GetStats();
		result.underruns = stats.underruns - underrunsAtWarmup;
		result.tempo = stats.tempo;
		result.buffered = stats.bufferedFrames;
		unsigned crossings = 0;
		for(size_t i = 2; i < output.size(); i += 2)
		{
			if((output[i - 2] < 0) != (output[i] < 0)) crossings++;
			result.maxJump = std::max(result.maxJump, std::abs(output[i] - output[i - 2]));
		}
		result.frequency = crossings / 2.0 / (output.size() / 2 / rate);
		return result;
	}
}

int main()
{
	bool ok = true;
	for(double speed : {1.0, 0.75, 0.5, 0.25})
	{
		auto r = Run(speed, 12.0, 440.0);
		// A 440Hz sine at amplitude 10000 changes by at most ~630 per sample.
		bool pass = (r.underruns == 0) && (std::fabs(r.frequency - 440.0) < 440.0 * 0.03) && (r.maxJump < 1500) && (r.buffered < 8192);
		std::printf("  %s speed %.2f: tempo %.2f, %.1f Hz, max jump %d, underruns %u, buffered %u frames\n", pass ? "ok  " : "FAIL", speed, r.tempo,
		            r.frequency, r.maxJump, r.underruns, r.buffered);
		ok &= pass;
	}
	std::printf("%s\n", ok ? "PASSED" : "FAILED");
	return ok ? 0 : 1;
}
