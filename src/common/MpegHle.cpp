#include "MpegHle.h"
#include "Mpeg2Simd.h"
#include <algorithm>
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

	// Syscalls of the stubs: start of a call (variant * 2) and "again" (+ 1).
	// CLibMc2 has 0x800-0x900.
	constexpr uint32_t SYSCALL_BASE = 0xA00;

	// sceMpegGetPicture(mp, buf, count): sets up sys and calls the common decode
	// routine (the jal, mask 0xFC000000), ending in ld ra; jr ra; addiu sp, 16.
	constexpr uint32_t ANY = 0x0C000000; //the jal
	const uint32_t g_codeRaw22[] = {
	    // libmpeg 2.2, RAW8 macroblocks. Silent Hill 2: 0x20E518.
	    0x27BDFFF0, 0x3C020FFF, 0xFFBF0000, 0x3442FFFF, 0x00A22824, 0x3C032000,
	    0x8C870040, 0x00A32825, 0xACE600E4, 0xACE500D8, 0xACE000DC, 0xACE000B0,
	    ANY, 0xACE000E0, 0xDFBF0000, 0x03E00008, 0x27BD0010};
	const uint32_t g_codeRaw30[] = {
	    // libmpeg 3.0, mode 0 (sys+0xC4), RAW8. Resident Evil 4: 0x2E7410.
	    0x27BDFFF0, 0x3C020FFF, 0xFFBF0000, 0x3442FFFF, 0x00A22824, 0x3C032000,
	    0x8C870040, 0x00A32825, 0xACE600F8, 0xACE500EC, 0xACE000F0, 0xACE000C4,
	    ANY, 0xACE000F4, 0xDFBF0000, 0x03E00008, 0x27BD0010};
	const uint32_t g_codeRgb30[] = {
	    // libmpeg 3.0, mode 1, RGB32. Resident Evil 4: 0x2E73C8.
	    0x27BDFFF0, 0x3C020FFF, 0xFFBF0000, 0x3442FFFF, 0x00A22824, 0x3C032000,
	    0x8C870040, 0x00A32825, 0x24020001, 0xACE200C4, 0xACE500EC, 0xACE600F8,
	    0xACE000F0, ANY, 0xACE000F4, 0xDFBF0000, 0x03E00008, 0x27BD0010};

	struct VARIANT
	{
		const uint32_t* code;
		uint32_t words;
		bool library30; //callback table and abort flag of libmpeg 3.0
		bool rgb;
		const char* name;
	};
	const VARIANT g_variants[] = {
	    {g_codeRaw22, 17, false, false, "libmpeg 2.2 RAW8"},
	    {g_codeRaw30, 17, true, false, "libmpeg 3.0 RAW8"},
	    {g_codeRgb30, 18, true, true, "libmpeg 3.0 RGB32"},
	};
	constexpr uint32_t EPILOGUE_WORDS = 3;

	// Keeps the original epilogue: a state saved inside libmpeg returns there.
	// The callback gets (mp, cbdata at sp+8, data).
	void BuildStub(uint32_t variantIndex, uint32_t* stub, uint32_t& stubWords)
	{
		const uint32_t syscallId = SYSCALL_BASE + variantIndex * 2;
		stubWords = g_variants[variantIndex].words - EPILOGUE_WORDS;
		const uint32_t words[12] = {
		    0x27BDFFF0,                 // addiu sp, sp, -16
		    0xFFBF0000,                 // sd    ra, 0(sp)
		    0x24030000 | syscallId,     // addiu v1, zero, start
		    0x0000000C,                 // syscall
		    0x14400000 | (stubWords - 5), // loop: bnez v0, epilogue
		    0x00000000,                 // nop
		    0x0320F809,                 // jalr  t9 (callback)
		    0x00000000,                 // nop
		    0x24030000 | (syscallId + 1), // addiu v1, zero, again
		    0x0000000C,                 // syscall
		    0x1000FFF9,                 // b     loop
		    0x00000000};                // nop
		for(uint32_t i = 0; i < stubWords; i++) stub[i] = (i < 12) ? words[i] : 0;
	}

	// libmpeg 3.0's sys: entries of (function, data, gp), type 1 = no data;
	// GetPictureAbort sets the flag (decoding fails while it is set).
	constexpr uint32_t SYS30_NODATA_FUNCTION = 0x18;
	constexpr uint32_t SYS30_NODATA_DATA = 0x1C;
	constexpr uint32_t SYS30_NODATA_GP = 0x20;
	constexpr uint32_t SYS30_ABORT = 0x878;
	constexpr uint32_t MAX_STUB_WORDS = 15;

	constexpr size_t MAX_BUFFERED_INPUT = 1024 * 1024;
	constexpr size_t MAX_QUEUED_PICTURES = 3;
	// Input on demand (Sofdec asks for a picture once 3 are buffered in its ring,
	// about 11 KB each in Resident Evil 4): the decoder stays within those.
	constexpr size_t MAX_QUEUED_PICTURES_ON_DEMAND = 2;
	// Sofdec stops DMA4 between pictures, so what the decoder works on while the
	// game runs has to arrive during the call before: about 2 pictures.
	constexpr size_t INPUT_WINDOW = 24 * 1024;
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
	m_pictureReady.notify_all();
	if(m_thread.joinable()) m_thread.join();
	if(m_inputDump) std::fclose(m_inputDump);
	m_inputDump = nullptr;
	if(m_log) std::fclose(m_log);
	m_log = nullptr;
	m_ram = nullptr;
}

void CMpegHle::SetInputDump(const char* path, size_t maxBytes)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if(m_inputDump) std::fclose(m_inputDump);
	m_inputDump = std::fopen(path, "wb");
	m_inputDumpLeft = maxBytes;
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
	auto matches = [&](uint32_t variant, uint32_t address) {
		const auto& v = g_variants[variant];
		for(uint32_t i = 0; i < v.words; i++)
		{
			uint32_t mask = (v.code[i] == ANY) ? 0xFC000000 : ~0u;
			if((Read32(m_ram, address + i * 4) & mask) != v.code[i]) return false;
		}
		return true;
	};
	uint32_t stub[MAX_STUB_WORDS];
	uint32_t stubWords = 0;

	m_vblanks++;
	bool anyPatched = false;
	for(uint32_t variant = 0; variant < VARIANT_COUNT; variant++)
	{
		uint32_t address = m_patchAddress[variant];
		if(address == 0) continue;
		BuildStub(variant, stub, stubWords);
		bool patched = true;
		for(uint32_t i = 0; patched && (i < stubWords); i++) patched = (Read32(m_ram, address + i * 4) == stub[i]);
		if(patched)
		{
			anyPatched = true;
			continue;
		}
		if(!matches(variant, address))
		{
			//Other code now (another executable): look again.
			Log("sceMpegGetPicture at 0x%08X is gone\n", address);
			m_patchAddress[variant] = 0;
		}
	}
	if(!anyPatched)
	{
		bool pending = false;
		for(uint32_t address : m_patchAddress) pending |= (address != 0);
		//Once a second until found (the game's code is above the kernel). One
		//pass finds every entry point of the library.
		if(!pending && ((m_vblanks % 60) == 1))
		{
			for(uint32_t address = 0x100000; address < 0x1000000; address += 4)
			{
				if(Read32(m_ram, address) != g_codeRaw22[0]) continue;
				for(uint32_t variant = 0; variant < VARIANT_COUNT; variant++)
				{
					if((m_patchAddress[variant] == 0) && matches(variant, address))
					{
						m_patchAddress[variant] = address;
						Log("sceMpegGetPicture (%s) found at 0x%08X\n", g_variants[variant].name, address);
					}
				}
			}
		}
	}
	for(uint32_t variant = 0; variant < VARIANT_COUNT; variant++)
	{
		uint32_t address = m_patchAddress[variant];
		if((address == 0) || !matches(variant, address)) continue;
		BuildStub(variant, stub, stubWords);
		//Not while the EE is in the bytes being replaced (it may stop anywhere).
		uint32_t pc = ee.m_State.nPC;
		if((pc >= address) && (pc < address + stubWords * 4)) continue;
		for(uint32_t i = 0; i < stubWords; i++) Write32(m_ram, address + i * 4, stub[i]);
		ee.m_executor->ClearActiveBlocksInRange(address, address + g_variants[variant].words * 4, false);
		Log("patched sceMpegGetPicture (%s) at 0x%08X\n", g_variants[variant].name, address);
	}
}

void CMpegHle::OnSyscall(void* context, CMIPS& ee)
{
	auto hle = static_cast<CMpegHle*>(context);
	uint32_t func = ee.m_State.nGPR[CMIPS::V1].nV0;
	hle->m_variant = std::min<uint32_t>((func - SYSCALL_BASE) / 2, VARIANT_COUNT - 1);
	hle->HandleGetPicture(ee, ((func - SYSCALL_BASE) & 1) == 0);
}

void CMpegHle::HandleGetPicture(CMIPS& ee, bool first)
{
	auto& gpr = ee.m_State.nGPR;
	const auto& variant = g_variants[m_variant];
	if(!first && variant.library30) gpr[CMIPS::GP].nD0 = static_cast<int32_t>(m_callerGp);
	if(first)
	{
		m_outputRgb = variant.rgb;
		m_mp = gpr[CMIPS::A0].nV0;
		m_buffer = gpr[CMIPS::A1].nV0 & 0x0FFFFFFF;
		m_mbCount = gpr[CMIPS::A2].nV0;
		m_waitsThisPicture = 0;
		if(!m_sinkEnabled)
		{
			//From now on DMA4 feeds the native decoder (what the IPU FIFO holds
			//goes first).
			{
				std::lock_guard<std::mutex> lock(m_mutex);
				m_inputOnDemand = variant.library30;
			}
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
	if(variant.library30 && (Read32(m_ram, sys + SYS30_ABORT) != 0))
	{
		gpr[CMIPS::V0].nD0 = static_cast<int64_t>(-1);
		return;
	}

	PICTURE picture;
	bool havePicture = false;
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		// Sofdec's no-data callback takes an empty ring buffer for the end of the
		// movie (it queues an end code, then aborts), and the input sink empties
		// the ring as soon as data is queued: while the decoder has data, wait for
		// its picture. The callback is for when it really ran out.
		if(variant.library30)
		{
			bool ready = m_pictureReady.wait_for(lock, std::chrono::seconds(3), [&]() { return !m_pictures.empty() || m_decoderStarved || m_quit; });
			if(!ready)
			{
				// Never freeze the game on a stuck decoder.
				Log("no picture from the decoder in 3 s (%u bytes queued, %u held)\n", static_cast<unsigned int>(m_incoming.size()), static_cast<unsigned int>(m_working));
			}
		}
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
		if(!picture.macroblocks.empty())
		{
			//Silent Hill 2 gets RAW8 macroblocks (it converts the colours on VU1),
			//Resident Evil 4 RGB32 ones (columns of them go to the GS as textures).
			size_t bytes = std::min<size_t>(picture.macroblocks.size(), static_cast<size_t>(m_mbCount) * (variant.rgb ? 0x400 : 0x180));
			if((m_buffer + bytes) <= 0x02000000) std::memcpy(m_ram + m_buffer, picture.macroblocks.data(), bytes);
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
			if(variant.library30)
			{
				//As libmpeg 3.0 leaves them under Sofdec, which keeps its own time.
				Write64(m_ram, m_mp + 0x10, ~0ULL);
				Write64(m_ram, m_mp + 0x18, ~0ULL);
				Write64(m_ram, m_mp + 0x20, 0x19B);
			}
			else
			{
				Write64(m_ram, m_mp + 0x10, static_cast<uint64_t>(m_frameIndex) * 3003);
				Write64(m_ram, m_mp + 0x18, ~0ULL);
				Write64(m_ram, m_mp + 0x20, 0x5B);
			}
			m_frameIndex++;
		}
		if(picture.macroblocks.empty() || picture.last)
		{
			Write32(m_ram, sys + 0x00, 1);
			Log("end of the movie after %u pictures\n", m_frameIndex);
		}
		gpr[CMIPS::V0].nD0 = 1;
		return;
	}

	uint32_t noData = Read32(m_ram, sys + (variant.library30 ? SYS30_NODATA_FUNCTION : 0x14));
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
	gpr[CMIPS::A2].nD0 = variant.library30 ? static_cast<int32_t>(Read32(m_ram, sys + SYS30_NODATA_DATA)) : 0;
	gpr[CMIPS::T9].nD0 = static_cast<int32_t>(noData);
	if(variant.library30)
	{
		m_callerGp = gpr[CMIPS::GP].nV0;
		gpr[CMIPS::GP].nD0 = static_cast<int32_t>(Read32(m_ram, sys + SYS30_NODATA_GP));
	}
}

uint32_t CMpegHle::OnInput(void* context, const uint8_t* data, uint32_t size)
{
	auto hle = static_cast<CMpegHle*>(context);
	{
		std::lock_guard<std::mutex> lock(hle->m_mutex);
		if(hle->m_incoming.size() + hle->m_working >= MAX_BUFFERED_INPUT) return 0;
		if(hle->m_inputOnDemand)
		{
			// A picture can be larger than the window: a decoder out of data can
			// always take more.
			size_t held = hle->m_incoming.size() + hle->m_working;
			size_t room = (held < INPUT_WINDOW) ? (INPUT_WINDOW - held) : 0;
			if(hle->m_decoderStarved) room = std::max<size_t>(room, 8 * 1024);
			size = static_cast<uint32_t>(std::min<size_t>(size, room & ~static_cast<size_t>(0xF)));
			if(size == 0) return 0;
		}
		if((data != hle->m_lastInputEnd) && (hle->m_inputJumps++ < 300))
		{
			hle->Log("input jump: 0x%08X -> 0x%08X (%u bytes)\n", static_cast<unsigned int>(hle->m_lastInputEnd ? hle->m_lastInputEnd - hle->m_ram : 0),
			         static_cast<unsigned int>(data - hle->m_ram), size);
		}
		hle->m_lastInputEnd = data + size;
		hle->m_incoming.insert(hle->m_incoming.end(), data, data + size);
		if(hle->m_inputDump && (hle->m_inputDumpLeft != 0))
		{
			size_t bytes = std::min<size_t>(size, hle->m_inputDumpLeft);
			std::fwrite(data, 1, bytes, hle->m_inputDump);
			hle->m_inputDumpLeft -= bytes;
			if(hle->m_inputDumpLeft == 0) std::fclose(hle->m_inputDump), hle->m_inputDump = nullptr;
		}
		hle->m_decoderStarved = false;
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
		hle->m_decoderStarved = false;
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
			       ((m_pictures.size() < (m_inputOnDemand ? MAX_QUEUED_PICTURES_ON_DEMAND : MAX_QUEUED_PICTURES)) && (!needData || !m_incoming.empty()));
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
			bool rgb = m_outputRgb;
			picture.macroblocks.resize(mbWidth * mbHeight * (rgb ? 0x400 : 0x180));
			picture.width = frame.width;
			picture.height = frame.height;
			// Macroblocks column by column (libmpeg's output order: Silent Hill
			// 2 shows 16 pixel wide strips of the wrong blocks otherwise; its
			// reference frames in sys+0xFC.. are row by row).
			uint8_t* out = picture.macroblocks.data();
			for(uint32_t mbx = 0; mbx < mbWidth; mbx++)
			{
				for(uint32_t mby = 0; mby < mbHeight; mby++, out += (rgb ? 0x400 : 0x180))
				{
					if(rgb)
					{
						uint32_t chromaStride = frame.width / 2;
						Mpeg2Simd::ConvertMacroblock(&frame.y[mby * 16 * frame.width + mbx * 16], frame.width,
						                             &frame.cb[mby * 8 * chromaStride + mbx * 8], &frame.cr[mby * 8 * chromaStride + mbx * 8], chromaStride, out);
						continue;
					}
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
			if(m_errorsLogged < 50)
			{
				m_errorsLogged++;
				Log("native decoder: result %d after %u pictures, skipping to the next start code\n", result, m_stats.decoded);
			}
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
			if(m_incoming.empty())
			{
				m_decoderStarved = true;
				m_pictureReady.notify_all();
			}
			continue;
		}
		if(push)
		{
			m_pictures.push_back(std::move(picture));
			m_pictureReady.notify_all();
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
