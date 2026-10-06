#include "SH_Vita.h"
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include "ThreadProfiler.h"

namespace
{
	std::atomic<float> g_tempo{1.0f};
	std::atomic<uint32_t> g_underruns{0};
}

CSH_Vita::CSH_Vita()
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
	m_stretcher.Reset();
}

void CSH_Vita::Write(int16* samples, unsigned int sampleCount, unsigned int)
{
	m_stretcher.Push(samples, sampleCount / 2);
}

bool CSH_Vita::HasFreeBuffers()
{
	return true;
}

void CSH_Vita::RecycleBuffers()
{
}

float CSH_Vita::GetTempo()
{
	return g_tempo.load();
}

uint32_t CSH_Vita::GetUnderruns()
{
	return g_underruns.load();
}

void CSH_Vita::ThreadProc()
{
	ThreadProfiler::RegisterCurrentThread("Audio");
	// Core 2 (with VU1): core 0 is reserved for the EE/IOP thread.
	sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), 0x40000 /* SCE_KERNEL_CPU_MASK_USER_2 */);
	std::vector<int16> grain(GRAIN * 2);
	while(m_running)
	{
		m_stretcher.Pull(grain.data(), GRAIN);
		auto stats = m_stretcher.GetStats();
		g_tempo = stats.tempo;
		g_underruns = stats.underruns;
		//Blocks until the hardware consumed the previous grain (~23ms)
		sceAudioOutOutput(m_port, grain.data());
	}
}
