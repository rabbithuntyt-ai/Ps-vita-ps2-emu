#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>
#include "sound/SoundHandler.h"

// Streams the SPU2 output (44.1kHz stereo) to a BGM audio port from a
// dedicated thread. The emulator never blocks on audio: when the ring buffer
// is full, the oldest samples are dropped; when it runs dry, silence plays.
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

private:
	enum
	{
		GRAIN = 1024,                 //stereo frames per output call
		RING_FRAMES = GRAIN * 8,
	};

	void ThreadProc();

	int m_port = -1;
	std::thread m_thread;
	std::atomic<bool> m_running{true};
	std::mutex m_mutex;
	std::vector<int16> m_ring;     //interleaved stereo
	size_t m_readPos = 0;
	size_t m_available = 0;        //in stereo frames
};
