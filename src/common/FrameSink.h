#pragma once

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

// Single-slot mailbox carrying the most recent GS output from the GS thread to
// whichever thread presents it. Frames move by swapping buffers, never by
// copying: the producer hands over its filled buffer and gets back a free one.
// An unconsumed frame is simply replaced by a newer one.
class CFrameMailbox
{
public:
	// Takes the contents of 'pixels' (leaving it with a recycled buffer).
	void Publish(std::vector<uint32_t>& pixels, uint32_t width, uint32_t height)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::swap(m_pixels, pixels);
		m_width = width;
		m_height = height;
		m_serial++;
	}

	// If a frame newer than lastSerial is available, swaps it into 'pixels'
	// and returns true.
	bool Fetch(uint64_t& lastSerial, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if(m_serial == lastSerial) return false;
		std::swap(m_pixels, pixels);
		width = m_width;
		height = m_height;
		lastSerial = m_serial;
		return true;
	}

	uint64_t GetSerial()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_serial;
	}

private:
	std::mutex m_mutex;
	std::vector<uint32_t> m_pixels;
	uint32_t m_width = 0;
	uint32_t m_height = 0;
	uint64_t m_serial = 0;
};
