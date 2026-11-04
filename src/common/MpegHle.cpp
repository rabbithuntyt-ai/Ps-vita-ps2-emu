#include "MpegHle.h"
#include "Mpeg2Simd.h"
#include <chrono>
#include <cstdarg>
#include <cstring>
#include "MIPS.h"
#include "ee/IPU.h"
#include "ee/PS2OS.h"

namespace
{
	uint32_t Read32(const uint8_t* ram, uint32_t address)
	{
		uint32_t value;
		std::memcpy(&value, ram + (address & 0x01FFFFFF), 4);
		return value;
	}

	void Write32(uint8_t* ram, uint32_t address, uint32_t value)
	{
		std::memcpy(ram + (address & 0x01FFFFFF), &value, 4);
	}

	void Write64(uint8_t* ram, uint32_t address, uint64_t value)
	{
		std::memcpy(ram + (address & 0x01FFFFFF), &value, 8);
	}

	enum
	{
		SYSCALL_GETPICTURE = 0xA00, //CLibMc2 has 0x800-0x900
		SYSCALL_GETPICTURE_CONTINUE = 0xA01,
	};

	// libmpeg 2.2 sceMpegGetPicture(mp, buf, count): sets up sys and calls the
	// common decode routine (jal, word 12). Silent Hill 2: 0x20E518.
	constexpr uint32_t GETPICTURE_WORDS = 17;
	const uint32_t g_getPictureCode[GETPICTURE_WORDS] = {
	    0x27BDFFF0, 0x3C020FFF, 0xFFBF0000, 0x3442FFFF, 0x00A22824, 0x3C032000,
	    0x8C870040, 0x00A32825, 0xACE600E4, 0xACE500D8, 0xACE000DC, 0xACE000B0,
	    0x0C000000, 0xACE000E0, 0xDFBF0000, 0x03E00008, 0x27BD0010};
	const uint32_t g_getPictureMask[GETPICTURE_WORDS] = {
	    ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, ~0u, 0xFC000000, ~0u, ~0u, ~0u, ~0u};

	// The stub keeps the original epilogue (words 14-16: ld ra; jr ra; addiu
	// sp,16): a state saved inside libmpeg returns there. The callback gets
	// (mp, cbdata at sp+8, data).
	constexpr uint32_t STUB_WORDS = 14;
	const uint32_t g_stub[STUB_WORDS] = {
	    0x27BDFFF0,                       // addiu sp, sp, -16
	    0xFFBF0000,                       // sd    ra, 0(sp)
	    0x24030000 | SYSCALL_GETPICTURE,  // addiu v1, zero, 0xA00
	    0x0000000C,                       // syscall
	    0x14400009,                       // loop: bnez v0, epilogue (word 14)
	    0x00000000,                       // nop
	    0x0320F809,                       // jalr  t9 (callback)
	    0x00000000,                       // nop
	    0x24030000 | SYSCALL_GETPICTURE_CONTINUE,
	    0x0000000C,                       // syscall
	    0x1000FFF9,                       // b     loop
	    0x00000000,                       // nop
	    0x00000000,
	    0x00000000};

	constexpr size_t MAX_BUFFERED_INPUT = 1024 * 1024;
	constexpr size_t MAX_QUEUED_PICTURES = 3;
	// No picture after this many no-data callbacks: give up on the movie
	// (sceMpegGetPicture fails, the game ends it).
	constexpr uint32_t MAX_WAITS = 5000;
}

CMpegHle::~CMpegHle()
{
	Disable();
}

void CMpegHle::Enable(uint8_t* eeRam, const char* logPath, std::function<void()> decoderThreadInit)
{
	Disable();
	m_ram = eeRam;
	m_log = std::fopen(logPath, "w");
	m_decoderThreadInit = std::move(decoderThreadInit);
	m_quit = false;
	m_thread = std::thread([this]() { DecoderThread(); });
	CIPU::SetResetObserver(&CMpegHle::OnIpuReset, this);
	CPS2OS::SetHleHooks(&CMpegHle::OnSyscall, &CMpegHle::OnVBlank, this);
	unsigned int simdFailures = Mpeg2Simd::SelfTest();
	Log("movie HLE enabled (decoder kernels: %s, %u self-test mismatches)\n", Mpeg2Simd::g_neon ? "NEON" : "scalar", simdFailures);
}

void CMpegHle::Disable()
{
	if(!m_ram) return;
	CPS2OS::SetHleHooks(nullptr, nullptr, nullptr);
	CIPU::SetResetObserver(nullptr, nullptr);
	CIPU::SetInputSink(nullptr, nullptr);
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_quit = true;
	}
	m_wake.notify_all();
	if(m_thread.joinable()) m_thread.join();
	if(m_log) std::fclose(m_log);
	m_log = nullptr;
	m_ram = nullptr;
}

CMpegHle::STATS CMpegHle::GetStats()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_stats;
}

void CMpegHle::Log(const char* format, ...)
{
	if(!m_log) return;
	va_list args;
	va_start(args, format);
	std::vfprintf(m_log, format, args);
	va_end(args);
	std::fflush(m_log);
}

void CMpegHle::OnVBlank(void* context, CMIPS& ee)
{
	static_cast<CMpegHle*>(context)->PatchCode(ee);
}

void CMpegHle::PatchCode(CMIPS& ee)
{
	auto matches = [&](uint32_t address) {
		for(uint32_t i = 0; i < GETPICTURE_WORDS; i++)
		{
			if((Read32(m_ram, address + i * 4) & g_getPictureMask[i]) != g_getPictureCode[i]) return false;
		}
		return true;
	};
	auto patched = [&](uint32_t address) {
		for(uint32_t i = 0; i < STUB_WORDS; i++)
		{
			if(Read32(m_ram, address + i * 4) != g_stub[i]) return false;
		}
		return true;
	};

	m_vblanks++;
	if(m_patchAddress != 0)
	{
		if(patched(m_patchAddress)) return;
		if(!matches(m_patchAddress))
		{
			//Other code now (another executable): look again.
			Log("sceMpegGetPicture at 0x%08X is gone\n", m_patchAddress);
			m_patchAddress = 0;
		}
	}
	if(m_patchAddress == 0)
	{
		//Once a second until found (the game's code is above the kernel).
		if((m_vblanks % 60) != 1) return;
		for(uint32_t address = 0x100000; address < 0x1000000; address += 4)
		{
			if((Read32(m_ram, address) == g_getPictureCode[0]) && matches(address))
			{
				m_patchAddress = address;
				Log("sceMpegGetPicture found at 0x%08X\n", address);
				break;
			}
		}
		if(m_patchAddress == 0) return;
	}
	//Not while the EE is in the bytes being replaced (it may stop anywhere).
	uint32_t pc = ee.m_State.nPC;
	if((pc >= m_patchAddress) && (pc < m_patchAddress + STUB_WORDS * 4)) return;
	for(uint32_t i = 0; i < STUB_WORDS; i++) Write32(m_ram, m_patchAddress + i * 4, g_stub[i]);
	ee.m_executor->ClearActiveBlocksInRange(m_patchAddress, m_patchAddress + GETPICTURE_WORDS * 4, false);
	Log("patched sceMpegGetPicture at 0x%08X\n", m_patchAddress);
}

void CMpegHle::OnSyscall(void* context, CMIPS& ee)
{
	auto hle = static_cast<CMpegHle*>(context);
	uint32_t func = ee.m_State.nGPR[CMIPS::V1].nV0;
	hle->HandleGetPicture(ee, func == SYSCALL_GETPICTURE);
}

void CMpegHle::HandleGetPicture(CMIPS& ee, bool first)
{
	auto& gpr = ee.m_State.nGPR;
	if(first)
	{
		m_mp = gpr[CMIPS::A0].nV0;
		m_buffer = gpr[CMIPS::A1].nV0 & 0x0FFFFFFF;
		m_mbCount = gpr[CMIPS::A2].nV0;
		m_waitsThisPicture = 0;
		if(!m_sinkEnabled)
		{
			//From now on DMA4 feeds the native decoder (what the IPU FIFO holds
			//goes first).
			m_sinkEnabled = true;
			CIPU::SetInputSink(&CMpegHle::OnInput, this);
			//Taking over a movie libmpeg started (a state load): keep counting
			//frames where it was (the game paces display by mp->frameCount). A
			//new movie has to start at 0: the game builds its display DMA chains
			//when it sees frame 0 (Silent Hill 2 0x2A5738).
			uint32_t frameCount = Read32(m_ram, m_mp + 0x08);
			m_frameIndex = (frameCount != 0) ? frameCount + 1 : 0;
			Log("input sink on (mp 0x%08X, buffer 0x%08X, %u macroblocks)\n", m_mp, m_buffer, m_mbCount);
		}
	}
	uint32_t sys = Read32(m_ram, m_mp + 0x40);
	Write32(m_ram, sys + 0x00, 0); //sceMpegIsEnd

	PICTURE picture;
	bool havePicture = false;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if(!m_pictures.empty())
		{
			picture = std::move(m_pictures.front());
			m_pictures.pop_front();
			havePicture = true;
			m_stats.pictures++;
		}
		else
		{
			m_stats.waits++;
		}
	}
	if(havePicture)
	{
		m_wake.notify_all();
		if(!picture.raw8.empty())
		{
			//Silent Hill 2 gets RAW8 macroblocks (it converts the colours on VU1).
			size_t bytes = std::min<size_t>(picture.raw8.size(), static_cast<size_t>(m_mbCount) * 0x180);
			if((m_buffer + bytes) <= 0x02000000) std::memcpy(m_ram + m_buffer, picture.raw8.data(), bytes);
			if(m_frameIndex < 400) Log("give picture %u after %u waits\n", m_frameIndex, m_waitsThisPicture);
			//libmpeg's per-picture fields (docs/HLE_MPEG.md): picture size (set
			//by libmpeg from the sequence header), frame count from 0,
			//timestamps at 29.97 Hz, frame picture flags.
			if(m_frameIndex < 2)
			{
				Log("mp size was %ux%u, picture %ux%u\n", Read32(m_ram, m_mp + 0x00), Read32(m_ram, m_mp + 0x04), picture.width, picture.height);
			}
			Write32(m_ram, m_mp + 0x00, picture.width);
			Write32(m_ram, m_mp + 0x04, picture.height);
			Write32(m_ram, m_mp + 0x08, m_frameIndex);
			Write64(m_ram, m_mp + 0x10, static_cast<uint64_t>(m_frameIndex) * 3003);
			Write64(m_ram, m_mp + 0x18, ~0ULL);
			Write64(m_ram, m_mp + 0x20, 0x5B);
			m_frameIndex++;
		}
		if(picture.raw8.empty() || picture.last)
		{
			Write32(m_ram, sys + 0x00, 1);
			Log("end of the movie after %u pictures\n", m_frameIndex);
		}
		gpr[CMIPS::V0].nD0 = 1;
		return;
	}

	uint32_t noData = Read32(m_ram, sys + 0x14);
	if((noData == 0) || (++m_waitsThisPicture > MAX_WAITS))
	{
		Log("no picture (%u waits): sceMpegGetPicture fails\n", m_waitsThisPicture);
		gpr[CMIPS::V0].nD0 = static_cast<int64_t>(-1);
		return;
	}
	//Ask the stub to call the game's no-data callback, then to ask again.
	uint32_t cbData = gpr[CMIPS::SP].nV0 + 8;
	Write32(m_ram, cbData, 1); //sceMpegCbNodata
	gpr[CMIPS::V0].nD0 = 0;
	gpr[CMIPS::A0].nD0 = static_cast<int32_t>(m_mp);
	gpr[CMIPS::A1].nD0 = static_cast<int32_t>(cbData);
	gpr[CMIPS::A2].nD0 = 0;
	gpr[CMIPS::T9].nD0 = static_cast<int32_t>(noData);
}

uint32_t CMpegHle::OnInput(void* context, const uint8_t* data, uint32_t size)
{
	auto hle = static_cast<CMpegHle*>(context);
	{
		std::lock_guard<std::mutex> lock(hle->m_mutex);
		if(hle->m_incoming.size() + hle->m_working >= MAX_BUFFERED_INPUT) return 0;
		if((data != hle->m_lastInputEnd) && (hle->m_inputJumps++ < 300))
		{
			hle->Log("input jump: 0x%08X -> 0x%08X (%u bytes)\n", static_cast<unsigned int>(hle->m_lastInputEnd ? hle->m_lastInputEnd - hle->m_ram : 0),
			         static_cast<unsigned int>(data - hle->m_ram), size);
		}
		hle->m_lastInputEnd = data + size;
		hle->m_incoming.insert(hle->m_incoming.end(), data, data + size);
	}
	hle->m_wake.notify_all();
	return size;
}

void CMpegHle::OnIpuReset(void* context)
{
	auto hle = static_cast<CMpegHle*>(context);
	if(!hle->m_sinkEnabled) return;
	CIPU::SetInputSink(nullptr, nullptr);
	hle->m_sinkEnabled = false;
	hle->m_frameIndex = 0;
	{
		std::lock_guard<std::mutex> lock(hle->m_mutex);
		hle->m_incoming.clear();
		hle->m_pictures.clear();
		hle->m_generation++;
	}
	hle->m_wake.notify_all();
	hle->Log("IPU reset: stream dropped\n");
}

void CMpegHle::DecoderThread()
{
	if(m_decoderThreadInit) m_decoderThreadInit();
	CMpeg2Decoder decoder;
	std::vector<uint8_t> work;
	size_t readPosition = 0;
	uint32_t generation = 0;
	bool needData = false;
	std::unique_lock<std::mutex> lock(m_mutex);
	while(true)
	{
		m_wake.wait(lock, [&]() {
			return m_quit || (generation != m_generation) ||
			       ((m_pictures.size() < MAX_QUEUED_PICTURES) && (!needData || !m_incoming.empty()));
		});
		if(m_quit) break;
		if(generation != m_generation)
		{
			generation = m_generation;
			work.clear();
			readPosition = 0;
			m_working = 0;
			decoder.Reset();
			needData = false;
			continue;
		}
		if(!m_incoming.empty())
		{
			if(readPosition > (work.size() / 2))
			{
				work.erase(work.begin(), work.begin() + readPosition);
				readPosition = 0;
			}
			work.insert(work.end(), m_incoming.begin(), m_incoming.end());
			m_incoming.clear();
			needData = false;
		}
		m_working = work.size() - readPosition;
		lock.unlock();

		size_t consumed = 0;
		auto start = std::chrono::steady_clock::now();
		auto result = decoder.Decode(work.data() + readPosition, work.size() - readPosition, false, consumed);
		PICTURE picture;
		bool push = false;
		if(result == CMpeg2Decoder::RESULT_PICTURE)
		{
			const auto& frame = decoder.GetPicture();
			uint32_t mbWidth = frame.width / 16, mbHeight = frame.height / 16;
			picture.raw8.resize(mbWidth * mbHeight * 0x180);
			picture.width = frame.width;
			picture.height = frame.height;
			// Macroblocks column by column (libmpeg's output order: Silent Hill
			// 2 shows 16 pixel wide strips of the wrong blocks otherwise; its
			// reference frames in sys+0xFC.. are row by row).
			uint8_t* out = picture.raw8.data();
			for(uint32_t mbx = 0; mbx < mbWidth; mbx++)
			{
				for(uint32_t mby = 0; mby < mbHeight; mby++, out += 0x180)
				{
					for(unsigned int j = 0; j < 16; j++) std::memcpy(out + j * 16, &frame.y[(mby * 16 + j) * frame.width + mbx * 16], 16);
					for(unsigned int j = 0; j < 8; j++)
					{
						std::memcpy(out + 0x100 + j * 8, &frame.cb[(mby * 8 + j) * (frame.width / 2) + mbx * 8], 8);
						std::memcpy(out + 0x140 + j * 8, &frame.cr[(mby * 8 + j) * (frame.width / 2) + mbx * 8], 8);
					}
				}
			}
			picture.last = decoder.PictureEndsSequence();
			if(m_stats.decoded < 400) Log("decoded type %d tref %u, %.1f ms, %u bytes\n", frame.pictureType, frame.temporalReference, 0.0, static_cast<unsigned int>(consumed));
			push = true;
		}
		else if(result == CMpeg2Decoder::RESULT_END)
		{
			push = true; //end without a picture to show
		}
		else if(result != CMpeg2Decoder::RESULT_NEED_DATA)
		{
			Log("native decoder: result %d, skipping to the next start code\n", result);
			consumed = std::min<size_t>(4, work.size() - readPosition);
		}
		double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		readPosition += consumed;

		lock.lock();
		if(generation != m_generation) continue; //reset while decoding
		m_working = work.size() - readPosition;
		if(result == CMpeg2Decoder::RESULT_NEED_DATA)
		{
			needData = true;
			continue;
		}
		if(push)
		{
			m_pictures.push_back(std::move(picture));
			if(result == CMpeg2Decoder::RESULT_PICTURE)
			{
				m_stats.decoded++;
				m_stats.decodeMs += ms;
				if((m_stats.decoded % 60) == 1)
				{
					Log("picture %u: %.1f ms (avg %.1f), %u returned, %u waits, input %u bytes\n", m_stats.decoded, ms,
					    m_stats.decodeMs / m_stats.decoded, m_stats.pictures, m_stats.waits,
					    static_cast<unsigned int>(m_working + m_incoming.size()));
				}
			}
		}
	}
}
