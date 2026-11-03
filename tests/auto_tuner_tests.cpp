// CAutoTuner: decisions on simulated games.

#include <cstdio>
#include <functional>
#include "AutoTuner.h"

namespace
{
	unsigned int g_failures = 0;

	void Check(bool condition, const char* what)
	{
		if(!condition)
		{
			std::printf("FAIL: %s\n", what);
			g_failures++;
		}
	}

	struct SAMPLE
	{
		float vmFps;
		float stallShare;
	};

	// Runs the tuner on a game model (fps from the applied choice and the
	// time in seconds) until it is done; returns the final choice.
	CAutoTuner::CHOICE Run(const std::function<SAMPLE(const CAutoTuner::CHOICE&, unsigned int)>& game, unsigned int* seconds = nullptr,
	                       const CAutoTuner::CHOICE& start = {})
	{
		CAutoTuner tuner(start);
		unsigned int t = 0;
		for(; (t < 3000) && !tuner.IsDone(); t++)
		{
			auto sample = game(tuner.GetChoice(), t);
			tuner.Update(sample.vmFps, 60.0f, sample.stallShare);
		}
		if(seconds) *seconds = t;
		return tuner.GetChoice();
	}

	float Noise(unsigned int t)
	{
		return static_cast<float>((t * 7919u) % 11u) * 0.1f - 0.5f; //+-0.5 fps
	}
}

int main()
{
	// VU1 on its own core helps (+20%): kept on; turning it off is tried and rejected.
	{
		CAutoTuner::CHOICE start;
		start.threadedVu1 = false;
		auto choice = Run([](const CAutoTuner::CHOICE& c, unsigned int t) { return SAMPLE{(c.threadedVu1 ? 36.0f : 30.0f) + Noise(t), 0.05f}; }, nullptr, start);
		Check(choice.threadedVu1, "a 20% gain from threaded VU1 is kept");
	}
	// Threaded VU1 hurts (synchronization): turned off.
	{
		auto choice = Run([](const CAutoTuner::CHOICE& c, unsigned int t) { return SAMPLE{(c.threadedVu1 ? 28.0f : 33.0f) + Noise(t), 0.05f}; });
		Check(!choice.threadedVu1, "threaded VU1 that slows the game down is turned off");
	}
	// No difference: settings stay as they were.
	{
		auto choice = Run([](const CAutoTuner::CHOICE&, unsigned int t) { return SAMPLE{30.0f + Noise(t), 0.05f}; });
		Check(choice.threadedVu1 && (choice.frameSkip == 0), "no gain, no change");
	}
	// The scene gets faster during the trial and stays faster (a menu turning
	// into a lighter scene): the second baseline catches it.
	{
		bool trialSeen = false;
		auto choice = Run([&](const CAutoTuner::CHOICE& c, unsigned int t) {
			if(!c.threadedVu1) trialSeen = true;
			float scene = trialSeen ? 40.0f : 30.0f; //lighter from the trial on, whatever the settings
			return SAMPLE{scene + Noise(t), 0.05f};
		});
		Check(choice.threadedVu1, "a scene change during a trial is not taken for a gain");
	}
	// Full speed already: no trials at all.
	{
		unsigned int seconds = 0;
		auto choice = Run([](const CAutoTuner::CHOICE&, unsigned int t) { return SAMPLE{59.8f + Noise(t) * 0.2f, 0.02f}; }, &seconds);
		Check(choice.threadedVu1 && (choice.frameSkip == 0), "full speed: settings unchanged");
		Check(seconds < CAutoTuner::WARMUP_SECONDS + CAutoTuner::SAMPLES + 5, "full speed: done after one measurement");
	}
	// Slow game where frame skip gives +25%: frame skip 1 kept.
	{
		auto choice = Run([](const CAutoTuner::CHOICE& c, unsigned int t) { return SAMPLE{(c.frameSkip ? 25.0f : 20.0f) + Noise(t), 0.05f}; });
		Check(choice.frameSkip == 1, "frame skip with a 25% gain on a slow game is kept");
	}
	// Frame skip with a small gain (+5%) is not worth its stutter.
	{
		auto choice = Run([](const CAutoTuner::CHOICE& c, unsigned int t) { return SAMPLE{(c.frameSkip ? 21.0f : 20.0f) + Noise(t) * 0.2f, 0.05f}; });
		Check(choice.frameSkip == 0, "frame skip with a 5% gain is rejected");
	}
	// Frame skip is not tried on games near full speed.
	{
		bool tried = false;
		Run([&](const CAutoTuner::CHOICE& c, unsigned int t) {
			if(c.frameSkip) tried = true;
			return SAMPLE{52.0f + Noise(t), 0.05f};
		});
		Check(!tried, "frame skip is not tried above 80% speed");
	}
	// Loading seconds (recompiling) don't count; a game that never settles
	// finishes on the timeout with the starting settings.
	{
		unsigned int seconds = 0;
		auto choice = Run([](const CAutoTuner::CHOICE&, unsigned int) { return SAMPLE{5.0f, 0.9f}; }, &seconds);
		Check(choice.threadedVu1 && (choice.frameSkip == 0), "never steady: starting settings kept");
		Check(seconds <= CAutoTuner::WARMUP_SECONDS + CAutoTuner::PHASE_TIMEOUT + 2, "never steady: gives up on the timeout");
	}
	// Loading in the middle of a trial is skipped, not averaged in.
	{
		auto choice = Run([](const CAutoTuner::CHOICE& c, unsigned int t) {
			if((t % 10) < 3) return SAMPLE{2.0f, 0.8f}; //a load every 10 seconds
			return SAMPLE{(c.threadedVu1 ? 30.0f : 24.0f) + Noise(t), 0.05f};
		});
		Check(choice.threadedVu1, "loading seconds don't count in measurements");
	}
	// The player changing settings stops tuning.
	{
		CAutoTuner tuner;
		for(unsigned int t = 0; t < 40; t++) tuner.Update(30.0f, 60.0f, 0.05f);
		tuner.Cancel();
		Check(tuner.IsDone() && !tuner.Update(30.0f, 60.0f, 0.05f), "cancel stops tuning");
	}

	std::printf("auto tuner: %u failures\n", g_failures);
	return (g_failures == 0) ? 0 : 1;
}
