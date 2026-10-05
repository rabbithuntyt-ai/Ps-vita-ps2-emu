#include "SH_Vita.h"
#include <algorithm>
#include <cstring>
#include <psp2/audioout.h>

CSH_Vita::CSH_Vita()
    : m_ring(RING_FRAMES * 2)
{
	m_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, 44100, SCE_AUDIO_OUT_MODE_STEREO);
	if(m_port >= 0)
	{
		m_thread = std::thread([this]() { ThreadProc(); });
	}
}

CSH_Vita::~CSH_Vita()
{
	m_running = false;
	if(m_thread.joinable()) m_thread.join();
	if(m_port >= 0) sceAudioOutReleasePort(m_port);
}

CSoundHandler* CSH_Vita::HandlerFactory()
{
	return new CSH_Vita();
}

void CSH_Vita::Reset()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_readPos = 0;
	m_available = 0;
}

void CSH_Vita::Write(int16* samples, unsigned int sampleCount, unsigned int)
{
	unsigned int frames = sampleCount / 2;
	std::lock_guard<std::mutex> lock(m_mutex);
	for(unsigned int i = 0; i < frames; i++)
	{
		if(m_available == RING_FRAMES)
		{
			//Drop the oldest frame
			m_readPos = (m_readPos + 1) % RING_FRAMES;
			m_available--;
		}
		size_t writePos = (m_readPos + m_available) % RING_FRAMES;
		m_ring[writePos * 2 + 0] = samples[i * 2 + 0];
		m_ring[writePos * 2 + 1] = samples[i * 2 + 1];
		m_available++;
	}
}

bool CSH_Vita::HasFreeBuffers()
{
	return true;
}

void CSH_Vita::RecycleBuffers()
{
}

void CSH_Vita::ThreadProc()
{
	std::vector<int16> grain(GRAIN * 2);
	while(m_running)
	{
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			size_t frames = std::min<size_t>(m_available, GRAIN);
			for(size_t i = 0; i < frames; i++)
			{
				size_t pos = (m_readPos + i) % RING_FRAMES;
				grain[i * 2 + 0] = m_ring[pos * 2 + 0];
				grain[i * 2 + 1] = m_ring[pos * 2 + 1];
			}
			std::fill(grain.begin() + frames * 2, grain.end(), 0);
			m_readPos = (m_readPos + frames) % RING_FRAMES;
			m_available -= frames;
		}
		//Blocks until the hardware consumed the previous grain (~23ms)
		sceAudioOutOutput(m_port, grain.data());
	}
}
