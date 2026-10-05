#pragma once

// Pitch-preserving time stretching (WSOLA) between the emulated SPU2 and the
// audio device.
//
// The emulator produces 44.1kHz audio only as fast as it runs: at 50% speed
// it delivers half the samples the device consumes. Instead of chopping the
// output into sound/silence (crackle), the stream is stretched in time while
// keeping its pitch. The tempo follows the measured input rate, corrected by
// the buffer level to keep latency bounded.
//
// Thread safety: Push() and Pull() may run on different threads.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

class CAudioStretcher
{
public:
	struct STATS
	{
		float tempo = 1.0f;          // input frames consumed per output frame
		float inputRate = 1.0f;      // input frames per output frame (smoothed)
		uint32_t bufferedFrames = 0; // input frames waiting
		uint32_t underruns = 0;      // Pull() calls that had to pad with silence
	};

	CAudioStretcher();

	void Reset();

	// Producer: interleaved stereo frames.
	void Push(const int16_t* samples, size_t frames);

	// Consumer: always fills 'frames' stereo frames (with silence, faded, if
	// there is truly nothing to play).
	void Pull(int16_t* samples, size_t frames);

	STATS GetStats();

	enum
	{
		SEQUENCE = 2048, // frames per WSOLA segment (46ms)
		OVERLAP = 256,   // crossfade length (5.8ms)
		SEEK = 384,      // search window for the best splice point (8.7ms)
	};

private:
	void UpdateTempo(size_t frames);
	bool ProcessSegment();
	size_t FindBestOffset(const int16_t* candidates) const;
	void CompactInput();

	std::mutex m_mutex;

	std::vector<int16_t> m_input; // interleaved stereo
	size_t m_inputRead = 0;       // in frames
	double m_inputPosition = 0;   // fractional read position (frames) for the next segment
	std::vector<int16_t> m_output;
	size_t m_outputRead = 0;
	std::vector<int16_t> m_tail; // last OVERLAP frames of the previous segment
	bool m_hasTail = false;
	int16_t m_lastSample[2] = {};

	size_t m_pushedSinceLastPull = 0;
	float m_inputRate = 1.0f;
	float m_tempo = 1.0f;
	uint32_t m_underruns = 0;
};
