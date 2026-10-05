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
		m_slowSeconds = m_fastSeconds = 0;
	}

	// eeIdle: fraction of EE time spent idle (0..1). vmFps / targetFps: the
	// emulated frame rate and the game's full speed (50 or 60). Returns true
	// when the rate changed.
	bool Update(float eeIdle, float vmFps, float targetFps)
	{
		bool belowSpeed = vmFps < targetFps * 0.92f;
		if((eeIdle < 0.02f) && belowSpeed)
		{
			m_fastSeconds = 0;
			if(++m_slowSeconds >= 2 && m_index > 0)
			{
				m_index--;
				m_slowSeconds = 0;
				return true;
			}
		}
		else if((eeIdle > 0.25f) || !belowSpeed)
		{
			m_slowSeconds = 0;
			// Climb back slowly so it does not oscillate.
			if((eeIdle > 0.25f) && (++m_fastSeconds >= 5) && (m_index < RATE_COUNT - 1))
			{
				m_index++;
				m_fastSeconds = 0;
				return true;
			}
		}
		else
		{
			m_slowSeconds = m_fastSeconds = 0;
		}
		return false;
	}

private:
	uint32_t m_index = RATE_COUNT - 1;
	uint32_t m_slowSeconds = 0;
	uint32_t m_fastSeconds = 0;
};
