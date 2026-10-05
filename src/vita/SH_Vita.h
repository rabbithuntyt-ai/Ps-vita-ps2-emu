#pragma once

#include <atomic>
#include <thread>
#include <vector>
#include "AudioStretcher.h"
#include "sound/SoundHandler.h"

// Streams the SPU2 output (44.1kHz stereo) to a BGM audio port from a
// dedicated thread. The emulator never blocks on audio. When it runs below
// full speed, the stream is time-stretched (pitch preserved) instead of
// being chopped into sound and silence.
class CSH_Vita : public CSoundHandler
{
public:
	CSH_Vita();
	~CSH_Vita() override;

	static CSoundHandler* HandlerFactory();

	void Reset() override;
	void Write(int16*, unsigned int, unsigned int) override;
	bool HasFreeBuffers() override;
	void RecycleBuffers() override;

	// Last stretcher state, for the performance overlay.
	static float GetTempo();
	static uint32_t GetUnderruns();

private:
	enum
	{
		GRAIN = 1024, //stereo frames per output call
	};

	void ThreadProc();

	int m_port = -1;
	std::thread m_thread;
	std::atomic<bool> m_running{true};
	CAudioStretcher m_stretcher;
};
