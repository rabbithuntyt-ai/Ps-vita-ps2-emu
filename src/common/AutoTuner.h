#pragma once

// Automatic per-game tuning on the first boot of a game: tries the speed
// settings that help some games and hurt others (VU1 on its own core, frame
// skip) and keeps the ones that make the game run faster. Fed once per
// second, like CAutoCycleRate; the frontend applies GetChoice() when Update()
// returns true and stores the result per game when IsDone().
//
// Each trial is measured A/B/A: the current settings, the trial, then the
// current settings again. A trial is kept only if it beats both baseline
// measurements, so a scene change in the middle (a menu turning into
// gameplay) cannot look like a gain. Only "steady" seconds count: loading
// (recompiling, stalls) and paused seconds are skipped.

#include <algorithm>
#include <cstdint>
#include <vector>

class CAutoTuner
{
public:
	struct CHOICE
	{
		bool threadedVu1 = true;
		uint32_t frameSkip = 0;

		bool operator==(const CHOICE& other) const
		{
			return (threadedVu1 == other.threadedVu1) && (frameSkip == other.frameSkip);
		}
	};

	static constexpr uint32_t WARMUP_SECONDS = 30;  //boot, logos, first loads
	static constexpr uint32_t SAMPLES = 15;         //steady seconds per measurement
	static constexpr uint32_t SETTLE_SECONDS = 3;   //after each change
	static constexpr uint32_t PHASE_TIMEOUT = 120;  //seconds to collect SAMPLES
	static constexpr float STEADY_STALL = 0.20f;    //stall share above this: loading
	static constexpr float FULL_SPEED = 0.97f;      //of the target fps: nothing to tune
	static constexpr float GAIN_VU1 = 1.05f;        //needed over both baselines
	static constexpr float GAIN_FRAME_SKIP = 1.10f; //frame skip costs smoothness
	static constexpr float SLOW_FOR_FRAME_SKIP = 0.80f; //only tried below this speed

	CAutoTuner() = default;

	explicit CAutoTuner(const CHOICE& start)
	    : m_choice(start)
	    , m_base(start)
	{
	}

	const CHOICE& GetChoice() const
	{
		return m_choice;
	}

	bool IsDone() const
	{
		return m_phase == PHASE_DONE;
	}

	// The player changed settings: their choice wins, tuning stops.
	void Cancel()
	{
		m_phase = PHASE_DONE;
	}

	// vmFps: emulated frames per second; targetFps: full speed (50 or 60);
	// stallShare: share of the emulation thread recompiling or waiting.
	// Returns true when GetChoice() changed (apply it).
	bool Update(float vmFps, float targetFps, float stallShare)
	{
		switch(m_phase)
		{
		case PHASE_WARMUP:
			if(++m_seconds >= WARMUP_SECONDS) StartPhase(PHASE_BASE_A);
			return false;
		case PHASE_DONE:
			return false;
		default:
			break;
		}
		m_seconds++;
		if(m_settle != 0)
		{
			m_settle--;
			return false;
		}
		if((stallShare <= STEADY_STALL) && (vmFps > 1.0f)) m_samples.push_back(vmFps);
		if(m_samples.size() < SAMPLES)
		{
			if(m_seconds > PHASE_TIMEOUT) return Finish(); //never steady: keep what we have
			return false;
		}
		float median = Median();
		switch(m_phase)
		{
		case PHASE_BASE_A:
			m_baseA = median;
			if(median >= targetFps * FULL_SPEED) return Finish(); //already full speed
			return NextTrial(targetFps);
		case PHASE_TRIAL:
			m_trialFps = median;
			m_choice = m_base; //back to the baseline for the second measurement
			StartPhase(PHASE_BASE_B);
			return true;
		case PHASE_BASE_B:
		{
			float gain = m_trial.frameSkip != m_base.frameSkip ? GAIN_FRAME_SKIP : GAIN_VU1;
			bool better = m_trialFps >= std::max(m_baseA, median) * gain;
			if(better)
			{
				m_base = m_trial;
				m_baseA = m_trialFps;
			}
			else
			{
				m_baseA = median;
			}
			return NextTrial(targetFps) || better;
		}
		default:
			return false;
		}
	}

	// Measured emulated fps of the final choice (0 when not measured).
	float GetFinalFps() const
	{
		return m_baseA;
	}

private:
	enum PHASE
	{
		PHASE_WARMUP,
		PHASE_BASE_A,
		PHASE_TRIAL,
		PHASE_BASE_B,
		PHASE_DONE,
	};

	void StartPhase(PHASE phase)
	{
		m_phase = phase;
		m_samples.clear();
		m_seconds = 0;
		m_settle = (phase == PHASE_BASE_A) ? 0 : SETTLE_SECONDS;
	}

	// Starts the next candidate (from the current baseline), or finishes.
	bool NextTrial(float targetFps)
	{
		while(m_nextCandidate < 2)
		{
			CHOICE trial = m_base;
			switch(m_nextCandidate++)
			{
			case 0:
				trial.threadedVu1 = !m_base.threadedVu1;
				break;
			case 1:
				if((m_base.frameSkip != 0) || (m_baseA >= targetFps * SLOW_FOR_FRAME_SKIP)) continue;
				trial.frameSkip = 1;
				break;
			}
			m_trial = trial;
			m_choice = trial;
			StartPhase(PHASE_TRIAL);
			return true;
		}
		return Finish();
	}

	bool Finish()
	{
		bool changed = !(m_choice == m_base);
		m_choice = m_base;
		m_phase = PHASE_DONE;
		return changed;
	}

	float Median()
	{
		std::vector<float> sorted = m_samples;
		std::sort(sorted.begin(), sorted.end());
		size_t n = sorted.size();
		return (n % 2) ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) * 0.5f;
	}

	PHASE m_phase = PHASE_WARMUP;
	CHOICE m_choice;  //applied now
	CHOICE m_base;    //best known
	CHOICE m_trial;   //being measured
	uint32_t m_nextCandidate = 0;
	uint32_t m_seconds = 0;
	uint32_t m_settle = 0;
	std::vector<float> m_samples;
	float m_baseA = 0, m_trialFps = 0;
};
