#pragma once

// Movie HLE "shadow mode" (docs/HLE_MPEG.md): the native MPEG-2 decoder runs
// next to the emulated libmpeg on the same stream (the IPU input, teed from
// DMA4) and every picture libmpeg returns is compared with the native one.
// Logs libmpeg's per-picture fields (frame count, pts, dts, flags) next to the
// native picture type and the native decoding time. Changes nothing in the
// emulation.

#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>
#include "Mpeg2Decoder.h"

class CMpegShadow
{
public:
	~CMpegShadow();

	// mpAddress: the game's sceMpeg context in EE RAM.
	void Enable(const uint8_t* eeRam, uint32_t mpAddress, const char* logPath);
	void Disable();

	// Frontend thread: decodes the stream that arrived, compares pictures.
	void Poll();

private:
	struct PICTURE
	{
		std::vector<uint32_t> rgb; //sceIpuRGB32 macroblocks
		int type = 0;
		uint32_t temporalReference = 0;
		double decodeMs = 0;
		uint8_t samples[16][3] = {}; //Y, Cb, Cr at SamplePosition(i), to fit libmpeg's color conversion
		std::vector<uint8_t> raw8; //the native picture as RAW8 macroblocks (256 Y, 64 Cb, 64 Cr each)
	};

	static void OnInput(void*, const uint8_t*, uint32_t);
	void Compare(const PICTURE&, uint32_t frameCount);

	std::mutex m_mutex;
	std::vector<uint8_t> m_incoming; //from the EE thread
	const uint8_t* m_lastInputEnd = nullptr; //end of the last chunk (in EE RAM)

	const uint8_t* m_ram = nullptr;
	uint32_t m_mpAddress = 0;
	std::FILE* m_log = nullptr;
	std::vector<uint8_t> m_stream;
	CMpeg2Decoder m_decoder;
	std::deque<PICTURE> m_pictures;
	uint32_t m_lastFrameCount = 0;
	uint32_t m_width = 0, m_height = 0;
	double m_decodeMsTotal = 0;
	uint32_t m_decoded = 0;
};
