#pragma once

#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

// Single-slot mailbox carrying the most recent GS output from the GS thread to
// whichever thread presents it. Producers never block on consumers: an
// unconsumed frame is simply replaced by a newer one.
class CFrameMailbox
{
public:
	void Publish(const uint32_t* pixels, uint32_t width, uint32_t height)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_pixels.resize(width * height);
		std::memcpy(m_pixels.data(), pixels, width * height * sizeof(uint32_t));
		m_width = width;
		m_height = height;
		m_serial++;
	}

	// Copies the latest frame out if it is newer than lastSerial.
	// Returns true (and updates lastSerial) when a new frame was copied.
	bool Fetch(uint64_t& lastSerial, std::vector<uint32_t>& pixels, uint32_t& width, uint32_t& height)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if(m_serial == lastSerial) return false;
		pixels = m_pixels;
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
