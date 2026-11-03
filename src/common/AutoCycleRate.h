#pragma once

// "Auto" EE cycle rate: lowers the emulated EE clock while the EE is the
// bottleneck (no idle time left and the game runs below full speed), and
// raises it back once there is headroom. Fed once per second.

#include <cstdint>

class CAutoCycleRate
{
public:
	static constexpr uint32_t RATES[] = {60, 75, 90, 100};
	static constexpr uint32_t RATE_COUNT = sizeof(RATES) / sizeof(RATES[0]);

	uint32_t GetRate() const
	{
		return RATES[m_index];
	}

	void Reset()
	{
		m_index = RATE_COUNT - 1;
		m_slowSeconds = m_fastSeconds = m_cooldown = 0;
	}

	// eeIdle: fraction of EE time spent idle (0..1). vmFps / targetFps: the
	// emulated frame rate and the game's full speed (50 or 60). Returns true
	// when the rate changed.
	//
	// stallShare: fraction of the emulation thread's time not spent emulating
	// (recompiling, waiting for the GS or the SPU thread). A slower EE does not
	// help there, and it costs correctness: games that build frames in
	// several buffers (Silent Hill 2) miss their deadlines and show unfinished
	// frames. Loading screens are mostly recompiling: they must not step down.
	//
	// Every change shifts the game's timing, so it has to be rare: steps down
	// need 3 EE-bound seconds in a row, steps up 10 seconds with plenty of
	// idle time (or stalled elsewhere), and nothing climbs back within 30
	// seconds of a step down (that would oscillate between two rates).
	bool Update(float eeIdle, float vmFps, float targetFps, float stallShare = 0.0f)
	{
		if(m_cooldown != 0) m_cooldown--;
		bool belowSpeed = vmFps < targetFps * 0.92f;
		bool stalledElsewhere = stallShare > 0.25f;
		if((eeIdle < 0.02f) && belowSpeed && !stalledElsewhere)
		{
			m_fastSeconds = 0;
			if(++m_slowSeconds >= 3 && m_index > 0)
			{
				m_index--;
				m_slowSeconds = 0;
				m_cooldown = 30;
				return true;
			}
			return false;
		}
		m_slowSeconds = 0;
		if((eeIdle > 0.40f) || (stallShare > 0.50f))
		{
			if((++m_fastSeconds >= 10) && (m_cooldown == 0) && (m_index < RATE_COUNT - 1))
			{
				m_index++;
				m_fastSeconds = 0;
				return true;
			}
		}
		else
		{
			m_fastSeconds = 0;
		}
		return false;
	}

private:
	uint32_t m_index = RATE_COUNT - 1;
	uint32_t m_slowSeconds = 0;
	uint32_t m_fastSeconds = 0;
	uint32_t m_cooldown = 0;
};
