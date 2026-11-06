#pragma once

// Movie HLE "replace mode" (docs/HLE_MPEG.md): libmpeg's sceMpegGetPicture is
// patched with a stub that asks the frontend for each picture, and the MPEG-2
// stream the game feeds to the IPU (DMA4) is decoded natively on another core.
//
// - The IPU input sink takes every DMA4 byte (up to a buffer limit), so the
//   game's ring buffer bookkeeping (it reads DMA4 progress) works unchanged.
// - The stub loops on a syscall: the handler either writes the next picture
//   and returns 1, or asks the stub to call the game's "no data" callback (it
//   reads the disc and queues more DMA, and lets emulated time pass) and to ask
//   again.
// - Pictures are what the patched entry point returns: RAW8 macroblocks (Silent
//   Hill 2, libmpeg 2.2) or RGB32 ones (Resident Evil 4, libmpeg 3.0 under
//   Sofdec), which libmpeg would send back through the IPU to convert.
// - An IPU reset (between movies) drops the stream and the pictures.

#include <atomic>
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
	// Resident Evil 4 (USA): its critical sections become a counter in guest code.
	void EnableThreadLockHle()
	{
		m_threadLockHle = true;
	}
	// Diagnostics: the first maxBytes the game feeds to the decoder, for replaying
	// on a PC (tests/mpeg2_decoder_tests --es).
	void SetInputDump(const char* path, size_t maxBytes);

	struct STATS
	{
		uint32_t pictures = 0;    //returned to the game
		uint32_t waits = 0;       //no-data callbacks (no picture ready)
		double decodeMs = 0;      //total native decoding time
		uint32_t decoded = 0;
		double waitMs = 0;        //EE thread blocked waiting for pictures
		uint32_t starved = 0;     //times the decoder ran out of input
	};
	STATS GetStats();

private:
	struct PICTURE
	{
		std::vector<uint8_t> macroblocks; //RAW8 (256 Y, 64 Cb, 64 Cr) or RGB32 (256 pixels); empty: end of the stream
		uint32_t width = 0, height = 0; //sequence size in pixels
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
	std::FILE* m_inputDump = nullptr;
	size_t m_inputDumpLeft = 0;
	uint32_t m_errorsLogged = 0;
	std::function<void()> m_decoderThreadInit;

	// EE thread: code patch and the current sceMpegGetPicture call.
	void PatchThreadLock(CMIPS&);
	bool m_threadLockHle = false;
	bool m_threadLockPatched = false;
	void PatchStartCodeScan(CMIPS&);
	void HandleStartCodeScan(CMIPS&);
	uint32_t m_scanAddress = 0; //Sofdec's start code scan, replaced by native code
	static constexpr uint32_t VARIANT_COUNT = 3;
	uint32_t m_patchAddress[VARIANT_COUNT] = {};
	uint32_t m_vblanks = 0;
	uint32_t m_variant = 0;  //of the current call
	uint32_t m_callerGp = 0; //restored after a libmpeg 3.0 callback, which runs with its own gp
	uint32_t m_mp = 0, m_buffer = 0, m_mbCount = 0;
	uint32_t m_frameIndex = 0;
	uint32_t m_waitsThisPicture = 0;
	bool m_sinkEnabled = false;
	const uint8_t* m_lastInputEnd = nullptr; //diagnostics: DMA4 source jumps
	uint32_t m_inputJumps = 0;

	// Shared with the decoder thread
	std::mutex m_mutex;
	std::condition_variable m_wake;
	std::condition_variable m_pictureReady; //a picture is queued, or the decoder ran out of input
	bool m_decoderStarved = false;
	// Demand-driven input (libmpeg 3.0 under Sofdec): Sofdec's chain loops over
	// its whole ring, and only an input as slow as the IPU's keeps behind what
	// Sofdec has written there. The decoder holds a small window of the stream.
	bool m_inputOnDemand = false;
	std::vector<uint8_t> m_incoming; //from DMA4, not taken by the decoder yet
	size_t m_working = 0;            //bytes the decoder holds
	std::deque<PICTURE> m_pictures;
	uint32_t m_generation = 0;       //bumped by resets
	bool m_quit = false;
	std::atomic<bool> m_outputRgb{false}; //set before the input sink is: read by the decoder thread
	STATS m_stats;
	std::thread m_thread;
};
