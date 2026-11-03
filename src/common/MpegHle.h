#pragma once

// Movie HLE "replace mode" (docs/HLE_MPEG.md): libmpeg's sceMpegGetPicture is
// patched with a stub that asks the frontend for each picture, and the MPEG-2
// stream the game feeds to the IPU (DMA4) is decoded natively on another core.
//
// - The IPU input sink takes every DMA4 byte (up to a buffer limit), so the
//   game's ring buffer bookkeeping (it reads DMA4 progress) works unchanged.
// - The stub loops on a syscall: the handler either writes the next picture
//   (RAW8 macroblocks, as Silent Hill 2 asks libmpeg for) and returns 1, or
//   asks the stub to call the
//   game's "no data" callback (it reads the disc and queues more DMA, and lets
//   emulated time pass) and to ask again.
// - An IPU reset (between movies) drops the stream and the pictures.

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "Mpeg2Decoder.h"

class CMIPS;

class CMpegHle
{
public:
	~CMpegHle();

	// decoderThreadInit runs first on the decoder thread (core affinity).
	void Enable(uint8_t* eeRam, const char* logPath, std::function<void()> decoderThreadInit);
	void Disable();

	struct STATS
	{
		uint32_t pictures = 0;    //returned to the game
		uint32_t waits = 0;       //no-data callbacks (no picture ready)
		double decodeMs = 0;      //total native decoding time
		uint32_t decoded = 0;
	};
	STATS GetStats();

private:
	struct PICTURE
	{
		std::vector<uint8_t> raw8; //RAW8 macroblocks (256 Y, 64 Cb, 64 Cr); empty: end of the stream
		bool last = false;         //the sequence ends with this picture
	};

	static void OnSyscall(void*, CMIPS&);
	static void OnVBlank(void*, CMIPS&);
	static uint32_t OnInput(void*, const uint8_t*, uint32_t);
	static void OnIpuReset(void*);

	void HandleGetPicture(CMIPS&, bool first);
	void PatchCode(CMIPS&);
	void DecoderThread();
	void Log(const char* format, ...);

	uint8_t* m_ram = nullptr;
	std::FILE* m_log = nullptr;
	std::function<void()> m_decoderThreadInit;

	// EE thread: code patch and the current sceMpegGetPicture call.
	uint32_t m_patchAddress = 0;
	uint32_t m_vblanks = 0;
	uint32_t m_mp = 0, m_buffer = 0, m_mbCount = 0;
	uint32_t m_frameIndex = 0;
	uint32_t m_waitsThisPicture = 0;
	bool m_sinkEnabled = false;
	const uint8_t* m_lastInputEnd = nullptr; //diagnostics: DMA4 source jumps
	uint32_t m_inputJumps = 0;

	// Shared with the decoder thread
	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::vector<uint8_t> m_incoming; //from DMA4, not taken by the decoder yet
	size_t m_working = 0;            //bytes the decoder holds
	std::deque<PICTURE> m_pictures;
	uint32_t m_generation = 0;       //bumped by resets
	bool m_quit = false;
	STATS m_stats;
	std::thread m_thread;
};
