#include "AudioStretcher.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	// Input kept around before old audio is dropped (latency cap).
	constexpr size_t MAX_BUFFERED_FRAMES = 44100;
	// Buffer level the tempo control steers towards.
	constexpr float TARGET_BUFFERED_FRAMES = CAudioStretcher::SEQUENCE + CAudioStretcher::SEEK + 2048;
	constexpr size_t FADE_FRAMES = 64;
}

CAudioStretcher::CAudioStretcher()
    : m_tail(OVERLAP * 2)
{
}

void CAudioStretcher::Reset()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_input.clear();
	m_inputRead = 0;
	m_inputPosition = 0;
	m_output.clear();
	m_outputRead = 0;
	m_hasTail = false;
	m_pushedSinceLastPull = 0;
	m_inputRate = 1.0f;
	m_tempo = 1.0f;
}

void CAudioStretcher::Push(const int16_t* samples, size_t frames)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_input.insert(m_input.end(), samples, samples + frames * 2);
	m_pushedSinceLastPull += frames;

	// Producer far ahead of playback (e.g. no frame limiter): drop old audio.
	size_t buffered = m_input.size() / 2 - static_cast<size_t>(m_inputPosition);
	if(buffered > MAX_BUFFERED_FRAMES)
	{
		m_inputPosition += static_cast<double>(buffered - MAX_BUFFERED_FRAMES / 2);
		m_hasTail = false;
	}
}

void CAudioStretcher::UpdateTempo(size_t frames)
{
	float instant = static_cast<float>(m_pushedSinceLastPull) / static_cast<float>(frames);
	m_pushedSinceLastPull = 0;
	m_inputRate += (std::min(instant, 2.0f) - m_inputRate) * 0.08f;

	float buffered = static_cast<float>(m_input.size() / 2) - static_cast<float>(m_inputPosition);
	float correction = 1.0f + 0.5f * (buffered - TARGET_BUFFERED_FRAMES) / TARGET_BUFFERED_FRAMES;
	correction = std::clamp(correction, 0.5f, 1.5f);
	m_tempo = std::clamp(m_inputRate * correction, 0.2f, 1.5f);
}

size_t CAudioStretcher::FindBestOffset(const int16_t* candidates) const
{
	// Mono, decimated by 2, normalized cross-correlation against the tail.
	float tail[OVERLAP / 2];
	for(size_t i = 0; i < OVERLAP / 2; i++)
	{
		tail[i] = static_cast<float>(m_tail[i * 4] + m_tail[i * 4 + 1]);
	}
	float mono[(SEEK + OVERLAP) / 2];
	for(size_t i = 0; i < (SEEK + OVERLAP) / 2; i++)
	{
		mono[i] = static_cast<float>(candidates[i * 4] + candidates[i * 4 + 1]);
	}

	size_t best = 0;
	float bestScore = -1e30f;
	for(size_t k = 0; k < SEEK / 2; k++)
	{
		float corr = 0, energy = 1.0f;
		for(size_t i = 0; i < OVERLAP / 2; i++)
		{
			float c = mono[k + i];
			corr += tail[i] * c;
			energy += c * c;
		}
		float score = corr / std::sqrt(energy);
		if(score > bestScore)
		{
			bestScore = score;
			best = k;
		}
	}
	return best * 2;
}

bool CAudioStretcher::ProcessSegment()
{
	size_t totalFrames = m_input.size() / 2;
	size_t pos = static_cast<size_t>(m_inputPosition);
	if(pos + SEEK + SEQUENCE > totalFrames) return false;

	const int16_t* in = m_input.data() + pos * 2;
	size_t offset = m_hasTail ? FindBestOffset(in) : 0;
	const int16_t* segment = in + offset * 2;

	size_t outStart = m_output.size();
	m_output.resize(outStart + (SEQUENCE - OVERLAP) * 2);
	int16_t* out = m_output.data() + outStart;
	if(m_hasTail)
	{
		// Linear crossfade from the previous segment's tail.
		for(size_t i = 0; i < OVERLAP; i++)
		{
			int32_t fadeIn = static_cast<int32_t>(i);
			int32_t fadeOut = static_cast<int32_t>(OVERLAP - i);
			for(size_t c = 0; c < 2; c++)
			{
				int32_t value = (m_tail[i * 2 + c] * fadeOut + segment[i * 2 + c] * fadeIn) / static_cast<int32_t>(OVERLAP);
				out[i * 2 + c] = static_cast<int16_t>(value);
			}
		}
		std::memcpy(out + OVERLAP * 2, segment + OVERLAP * 2, (SEQUENCE - 2 * OVERLAP) * 2 * sizeof(int16_t));
	}
	else
	{
		std::memcpy(out, segment, (SEQUENCE - OVERLAP) * 2 * sizeof(int16_t));
	}
	std::memcpy(m_tail.data(), segment + (SEQUENCE - OVERLAP) * 2, OVERLAP * 2 * sizeof(int16_t));
	m_hasTail = true;

	m_inputPosition += static_cast<double>(SEQUENCE - OVERLAP) * m_tempo;
	CompactInput();
	return true;
}

void CAudioStretcher::CompactInput()
{
	size_t consumed = static_cast<size_t>(m_inputPosition);
	if(consumed < 16384) return;
	m_input.erase(m_input.begin(), m_input.begin() + consumed * 2);
	m_inputPosition -= static_cast<double>(consumed);
}

void CAudioStretcher::Pull(int16_t* samples, size_t frames)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	UpdateTempo(frames);

	while((m_output.size() / 2 - m_outputRead) < frames)
	{
		if(!ProcessSegment()) break;
	}

	size_t available = std::min(m_output.size() / 2 - m_outputRead, frames);
	std::memcpy(samples, m_output.data() + m_outputRead * 2, available * 2 * sizeof(int16_t));
	m_outputRead += available;
	if(available != 0)
	{
		m_lastSample[0] = samples[available * 2 - 2];
		m_lastSample[1] = samples[available * 2 - 1];
	}
	if(available < frames)
	{
		// Nothing left: fade out instead of clicking.
		m_underruns++;
		for(size_t i = available; i < frames; i++)
		{
			size_t step = i - available;
			int32_t gain = (step < FADE_FRAMES) ? static_cast<int32_t>(FADE_FRAMES - step) : 0;
			samples[i * 2 + 0] = static_cast<int16_t>(m_lastSample[0] * gain / static_cast<int32_t>(FADE_FRAMES));
			samples[i * 2 + 1] = static_cast<int16_t>(m_lastSample[1] * gain / static_cast<int32_t>(FADE_FRAMES));
		}
		m_lastSample[0] = m_lastSample[1] = 0;
		// Restart cleanly when audio comes back.
		m_hasTail = false;
	}

	if(m_outputRead > 16384)
	{
		m_output.erase(m_output.begin(), m_output.begin() + m_outputRead * 2);
		m_outputRead = 0;
	}
}

CAudioStretcher::STATS CAudioStretcher::GetStats()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	STATS stats;
	stats.tempo = m_tempo;
	stats.inputRate = m_inputRate;
	stats.bufferedFrames = static_cast<uint32_t>(m_input.size() / 2 - static_cast<size_t>(m_inputPosition));
	stats.underruns = m_underruns;
	return stats;
}
