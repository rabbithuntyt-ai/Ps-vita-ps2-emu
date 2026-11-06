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
		// Word 12: an empty function, called instead of the no-data callback to
		// let the EE run while a starved decoder waits for DMA and the disc.
		stub[12] = 0x03E00008; // jr ra
		stub[13] = 0x00000000; // nop
	}

	// Sofdec's scan for an MPEG start code of a given kind (a0 buffer, a1 size,
	// a2 mask of kinds), byte by byte: 12% of the EE's time in Resident Evil 4's
	// movies. Returns the start code's address or 0. RE4: 0x2DAD80.
	constexpr uint32_t SYSCALL_SCAN = SYSCALL_BASE + 6;
	const uint32_t g_codeScan[] = {
	    0x27BDFFC0, 0x0000182D, 0xFFB10008, 0x0080882D, 0xFFB20010, 0x02259021, 0x0232102B, 0xFFB50028, 0xFFB00000, 0x00C0A82D, 0xFFB30018,
	    0xFFB40020, 0xFFB60030, 0x10400029, 0xFFBF0038, 0x24140001, 0x24130002, 0x24160003, 0x82220000, 0x00000000, 0x1074000D, 0x26310001,
	    0x10600007, 0x00000000, 0x1073000D, 0x00000000, 0x50760013, 0x2630FFFC, 0x10000018, 0x0232102B, 0x50400015, 0x24030001, 0x10000014,
	    0x0232102B, 0x0260182D, 0x10000010, 0x0002180B, 0x00000000, 0x14540003, 0x00000000, 0x1000000B, 0x24030003, 0x1040000A, 0x0232102B,
	    0x10000008, 0x0000182D, ANY, 0x0200202D, 0x02A21824, 0x14600006, 0x0200102D, 0x0000182D, 0x0232102B, 0x5440FFDE, 0x82220000,
	    0x0000102D, 0xDFB00000, 0xDFB10008, 0xDFB20010, 0xDFB30018, 0xDFB40020, 0xDFB50028, 0xDFB60030, 0xDFBF0038, 0x03E00008, 0x27BD0040};
	constexpr uint32_t SCAN_WORDS = sizeof(g_codeScan) / sizeof(g_codeScan[0]);
	const uint32_t g_stubScan[] = {0x24030000 | SYSCALL_SCAN, 0x0000000C, 0x03E00008, 0x00000000};

	// Resident Evil 4 (USA) enters and leaves its critical sections with about 5
	// kernel calls per pair (priority up, a helper thread resumed, 2700 pairs a
	// second in movies, a quarter of the EE's time). Matched exactly, all or nothing.
	constexpr uint32_t RE4_LOCK_ENTER = 0x17BEA8;
	constexpr uint32_t RE4_LOCK_LEAVE = 0x17BF30;
	constexpr uint32_t RE4_LOCK_COUNT = 0x2F68F0;
	const uint32_t g_codeLockEnter[] = {
	    0x27BDFFE0, 0x3C02002F, 0xFFB20010, 0x245268F0, 0x8E420000, 0xFFB00000, 0xFFB10008, 0x14400011, 0xFFBF0018, 0x0C0A72C4, 0x3C10002F,
	    0x0040882D, 0x3C02002F, 0x8C4568C0, 0x0220202D, 0x0C0A72AC, 0x26106948, 0x3C030030, 0xAC62E1E0, 0x3C050030, 0x8E040000, 0x0C05F144,
	    0xACB1E1E4, 0x8E030000, 0x8E420000, 0x24420001, 0xDFB00000, 0xAE420000, 0xDFB10008, 0xDFB20010, 0xDFBF0018, 0x03E00008, 0x27BD0020};
	const uint32_t g_codeLockLeave[] = {
	    0x27BDFFE0, 0x3C03002F, 0xFFB10008, 0x247168F0, 0x8E220000, 0xFFB00000, 0x2442FFFF, 0xFFBF0010, 0x1440000B, 0xAE220000, 0x3C10002F,
	    0x26106948, 0x0C05F15E, 0x8E040000, 0x0C0A72C4, 0x8E030000, 0x3C030030, 0x8C65E1E0, 0x0C0A72AC, 0x0040202D, 0x8E220000, 0x04420001,
	    0xAE200000, 0xDFB00000, 0xDFB10008, 0xDFBF0010, 0x03E00008, 0x27BD0020};
	const uint32_t g_stubLockEnter[] = {
	    0x3C03002F, // lui   v1, 0x2F
	    0x8C6268F0, // lw    v0, 0x68F0(v1)
	    0x24420001, // addiu v0, v0, 1
	    0xAC6268F0, // sw    v0, 0x68F0(v1)
	    0x03E00008, // jr    ra
	    0x00000000};
	const uint32_t g_stubLockLeave[] = {
	    0x3C03002F, // lui   v1, 0x2F
	    0x8C6268F0, // lw    v0, 0x68F0(v1)
	    0x2442FFFF, // addiu v0, v0, -1
	    0xAC6268F0, // sw    v0, 0x68F0(v1)
	    0x10400003, // beqz  v0, done: nothing was held back
	    0x00000000, // nop
	    0x24030A0F, // addiu v1, zero, SYSCALL_DISPATCH_UNLOCK
	    0x0000000C, // syscall
	    0x03E00008, // done: jr ra
	    0x00000000};

	// memcpy of Sony's C library (position independent, so it can match any game on
	// this SDK; RE4: 0x2AF3AC). Sizes under 32 and unaligned copies go through a
	// loop of one byte at a time: 13% of the EE's time in RE4's intro, where
	// Sofdec copies video out of disc sectors. That loop becomes native code; the
	// aligned 128 bit path stays as it is.
	constexpr uint32_t SYSCALL_MEMCPY_BYTES = SYSCALL_BASE + 7;
	const uint32_t g_codeMemcpy[] = {0x0080402D, 0x2CC20020, 0x1440001C, 0x0100182D, 0x00A81025, 0x3042000F, 0x54400019, 0x24C6FFFF, 0x0100382D, 0x78A30000, 0x24C6FFE0, 0x24A50010, 0x2CC40020, 0x7CE30000, 0x24E70010, 0x78A20000, 0x24A50010, 0x7CE20000, 0x1080FFF6, 0x24E70010, 0x2CC20008, 0x14400009, 0x00E0182D, 0xDCA30000, 0x24C6FFF8, 0x24A50008, 0x2CC20008, 0xFCE30000, 0x1040FFFA, 0x24E70008, 0x00E0182D, 0x24C6FFFF, 0x2402FFFF, 0x10C20007, 0x0040202D, 0x90A20000, 0x24C6FFFF, 0x24A50001, 0xA0620000, 0x14C4FFFB, 0x24630001, 0x03E00008, 0x0100102D};
	constexpr uint32_t MEMCPY_WORDS = sizeof(g_codeMemcpy) / sizeof(g_codeMemcpy[0]);
	constexpr uint32_t MEMCPY_UNALIGNED_DELAY = (0x2AF3C8 - 0x2AF3AC) / 4; //addiu a2, a2, -1 in a branch delay slot
	constexpr uint32_t MEMCPY_UNALIGNED_BRANCH = (0x2AF3C4 - 0x2AF3AC) / 4;
	constexpr uint32_t MEMCPY_LOOP = (0x2AF428 - 0x2AF3AC) / 4;
	const uint32_t g_stubMemcpyLoop[] = {
	    0x0060202D,                         // move  a0, v1 (where the bytes go)
	    0x24030000 | SYSCALL_MEMCPY_BYTES,  // addiu v1, zero, id
	    0x0000000C,                         // syscall (a0, a1, a2)
	    0x03E00008,                         // jr    ra
	    0x0100102D};                        // move  v0, t0

	// Kinds of the code after 00 00 01 (Sofdec's classifier, RE4 0x2DABE8).
	uint32_t StartCodeKind(uint8_t code)
	{
		switch(code)
		{
		case 0x00: return 4;
		case 0x01: return 3;
		case 0xB2: return 32;
		case 0xB3: return 64;
		case 0xB5: return 16;
		case 0xB7: return 128;
		case 0xB8: return 8;
		default: return ((code >= 0x02) && (code <= 0xAF)) ? 1 : 0;
		}
	}

	// libmpeg 3.0's sys: entries of (function, data, gp), type 1 = no data;
	// GetPictureAbort sets the flag (decoding fails while it is set).
	constexpr uint32_t SYS30_NODATA_FUNCTION = 0x18;
	constexpr uint32_t SYS30_NODATA_DATA = 0x1C;
	constexpr uint32_t SYS30_NODATA_GP = 0x20;
	constexpr uint32_t SYS30_ABORT = 0x878;
	constexpr uint32_t MAX_STUB_WORDS = 15;
	// Sofdec's no-data callback ends the movie, so it is the last resort: a decoder
	// out of data gets this long first (a stream that has just started is still
	// coming from the disc).
	constexpr uint32_t STARVED_GRACE_VBLANKS = 120;
	// The callback also refreshes the DMA feed, so it has to run during that time.
	// In this Sofdec build (RE4: 0x2CE3F0) it can only end the movie while a flag
	// set by its caller is on: data + 8120 -> player, +4264 + 20.
	const uint32_t g_sofdecNoData[] = {0x27BDFFE0, 0xFFB00000, 0xFFB10008, 0xFFB20010, 0x24120001, 0xFFBF0018, 0x8CC21FB8, 0x245110A8, 0x244410D0, 0x8E230018, 0x14720005, 0x24501118, 0x0C0B9D16, 0x00000000, 0x10000014, 0x24020001, 0x0C0B3664, 0x0200202D, 0x8E230014, 0x1060000F, 0x24020001, 0x8E020010, 0x1440000C, 0x24020001, 0xAE320018, 0x3C05003D, 0x0200202D, 0x24A58338, 0x0C0B38D4, 0x24060010, 0x8E030018, 0x0200202D, 0x0C0B3664, 0xAE030014, 0x24020001, 0xDFB00000, 0xDFB10008, 0xDFB20010, 0xDFBF0018, 0x03E00008, 0x27BD0020};
	constexpr uint32_t SOFDEC_NODATA_WORDS = sizeof(g_sofdecNoData) / sizeof(g_sofdecNoData[0]);
	// Sony's ezmpeg ViBuf (docs/HLE_MPEG.md), inside this Sofdec's player: ring start,
	// tags, units, first queued unit, units queued. RE4: 2 KB units, 512 KB.
	constexpr uint32_t SOFDEC_VIBUF = 4376;
	constexpr uint32_t VIBUF_UNIT = 2048;

	constexpr size_t MAX_BUFFERED_INPUT = 1024 * 1024;
	constexpr size_t MAX_QUEUED_PICTURES = 3;
	// Input on demand (Sofdec asks for a picture once 3 are buffered in its ring,
	// about 11 KB each in Resident Evil 4): the decoder stays within those.
	constexpr size_t MAX_QUEUED_PICTURES_ON_DEMAND = 3;
	// Sofdec stops DMA4 between pictures, so what the decoder works on while the
	// game runs has to arrive during the call before. RE4's intro has 13.5 KB per
	// picture on average. Well under its 512 KB ring, but check that no input is
	// stale when changing this (docs/RE4.md).
	constexpr size_t INPUT_WINDOW = 96 * 1024;
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
	CPS2OS::g_dispatchLockAddress = 0;
	m_threadLockPatched = false;
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
	PatchStartCodeScan(ee);
	PatchThreadLock(ee);
	PatchMemcpy(ee);
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

void CMpegHle::PatchMemcpy(CMIPS& ee)
{
	auto matches = [&](uint32_t address) {
		for(uint32_t i = 0; i < MEMCPY_WORDS; i++)
		{
			if(Read32(m_ram, address + i * 4) != g_codeMemcpy[i]) return false;
		}
		return true;
	};
	if(m_memcpyAddress != 0)
	{
		if(Read32(m_ram, m_memcpyAddress + MEMCPY_LOOP * 4) == g_stubMemcpyLoop[0]) return;
		if(!matches(m_memcpyAddress)) m_memcpyAddress = 0;
	}
	if((m_memcpyAddress == 0) && ((m_vblanks % 60) == 1))
	{
		for(uint32_t address = 0x100000; address < 0x1000000; address += 4)
		{
			if((Read32(m_ram, address) == g_codeMemcpy[0]) && matches(address))
			{
				m_memcpyAddress = address;
				Log("memcpy found at 0x%08X\n", address);
				break;
			}
		}
	}
	if(m_memcpyAddress == 0) return;
	uint32_t pc = ee.m_State.nPC;
	if((pc >= m_memcpyAddress) && (pc < m_memcpyAddress + MEMCPY_WORDS * 4)) return;
	// The unaligned branch: to the loop's first word, without touching the size.
	Write32(m_ram, m_memcpyAddress + MEMCPY_UNALIGNED_BRANCH * 4, (Read32(m_ram, m_memcpyAddress + MEMCPY_UNALIGNED_BRANCH * 4) & 0xFFFF0000) | ((MEMCPY_LOOP - MEMCPY_UNALIGNED_BRANCH - 1) & 0xFFFF));
	Write32(m_ram, m_memcpyAddress + MEMCPY_UNALIGNED_DELAY * 4, 0);
	for(uint32_t i = 0; i < 5; i++) Write32(m_ram, m_memcpyAddress + (MEMCPY_LOOP + i) * 4, g_stubMemcpyLoop[i]);
	ee.m_executor->ClearActiveBlocksInRange(m_memcpyAddress, m_memcpyAddress + MEMCPY_WORDS * 4, false);
	Log("patched memcpy at 0x%08X\n", m_memcpyAddress);
}

void CMpegHle::HandleMemcpyBytes(CMIPS& ee)
{
	auto& gpr = ee.m_State.nGPR;
	uint32_t destination = gpr[CMIPS::A0].nV0;
	uint32_t source = gpr[CMIPS::A1].nV0;
	uint32_t size = gpr[CMIPS::A2].nV0;
	uint32_t physicalDestination = destination & 0x1FFFFFFF, physicalSource = source & 0x1FFFFFFF;
	bool inRam = (physicalDestination < 0x02000000) && (physicalSource < 0x02000000) && (size <= (0x02000000 - physicalDestination)) && (size <= (0x02000000 - physicalSource));
	bool separate = ((physicalDestination + size) <= physicalSource) || ((physicalSource + size) <= physicalDestination);
	if(inRam && separate)
	{
		std::memcpy(m_ram + physicalDestination, m_ram + physicalSource, size);
	}
	else
	{
		// Scratchpad, hardware, or overlapping: byte by byte, upwards, as the loop.
		for(uint32_t i = 0; i < size; i++) ee.m_pMemoryMap->SetByte(destination + i, ee.m_pMemoryMap->GetByte(source + i));
	}
	gpr[CMIPS::A1].nD0 = static_cast<int32_t>(source + size);
}

void CMpegHle::PatchThreadLock(CMIPS& ee)
{
	if(!m_threadLockHle || m_threadLockPatched || ((m_vblanks % 60) != 1)) return;
	auto same = [&](uint32_t address, const uint32_t* words, size_t count) {
		for(size_t i = 0; i < count; i++)
		{
			if(Read32(m_ram, address + static_cast<uint32_t>(i) * 4) != words[i]) return false;
		}
		return true;
	};
	const size_t enterWords = sizeof(g_codeLockEnter) / 4, leaveWords = sizeof(g_codeLockLeave) / 4;
	if(!same(RE4_LOCK_ENTER, g_codeLockEnter, enterWords) || !same(RE4_LOCK_LEAVE, g_codeLockLeave, leaveWords)) return;
	// Only outside the lock, with the EE outside the code being replaced.
	uint32_t pc = ee.m_State.nPC;
	if((Read32(m_ram, RE4_LOCK_COUNT) != 0) || ((pc >= RE4_LOCK_ENTER) && (pc < RE4_LOCK_LEAVE + leaveWords * 4))) return;
	for(size_t i = 0; i < sizeof(g_stubLockEnter) / 4; i++) Write32(m_ram, RE4_LOCK_ENTER + static_cast<uint32_t>(i) * 4, g_stubLockEnter[i]);
	for(size_t i = 0; i < sizeof(g_stubLockLeave) / 4; i++) Write32(m_ram, RE4_LOCK_LEAVE + static_cast<uint32_t>(i) * 4, g_stubLockLeave[i]);
	ee.m_executor->ClearActiveBlocksInRange(RE4_LOCK_ENTER, RE4_LOCK_LEAVE + leaveWords * 4, false);
	CPS2OS::g_dispatchLockAddress = RE4_LOCK_COUNT;
	m_threadLockPatched = true;
	Log("patched the critical sections at 0x%08X and 0x%08X\n", RE4_LOCK_ENTER, RE4_LOCK_LEAVE);
}

void CMpegHle::PatchStartCodeScan(CMIPS& ee)
{
	auto matches = [&](uint32_t address) {
		for(uint32_t i = 0; i < SCAN_WORDS; i++)
		{
			uint32_t mask = (g_codeScan[i] == ANY) ? 0xFC000000 : ~0u;
			if((Read32(m_ram, address + i * 4) & mask) != g_codeScan[i]) return false;
		}
		return true;
	};
	if(m_scanAddress != 0)
	{
		bool patched = true;
		for(uint32_t i = 0; patched && (i < 4); i++) patched = (Read32(m_ram, m_scanAddress + i * 4) == g_stubScan[i]);
		if(patched) return;
		if(!matches(m_scanAddress)) m_scanAddress = 0;
	}
	if((m_scanAddress == 0) && ((m_vblanks % 60) == 1))
	{
		for(uint32_t address = 0x100000; address < 0x1000000; address += 4)
		{
			if((Read32(m_ram, address) == g_codeScan[0]) && matches(address))
			{
				m_scanAddress = address;
				Log("start code scan found at 0x%08X\n", address);
				break;
			}
		}
	}
	if(m_scanAddress == 0) return;
	uint32_t pc = ee.m_State.nPC;
	if((pc >= m_scanAddress) && (pc < m_scanAddress + 16)) return;
	for(uint32_t i = 0; i < 4; i++) Write32(m_ram, m_scanAddress + i * 4, g_stubScan[i]);
	ee.m_executor->ClearActiveBlocksInRange(m_scanAddress, m_scanAddress + SCAN_WORDS * 4, false);
	Log("patched the start code scan at 0x%08X\n", m_scanAddress);
}

void CMpegHle::HandleStartCodeScan(CMIPS& ee)
{
	auto& gpr = ee.m_State.nGPR;
	uint32_t address = gpr[CMIPS::A0].nV0;
	uint32_t size = gpr[CMIPS::A1].nV0;
	uint32_t mask = gpr[CMIPS::A2].nV0;
	uint32_t physical = address & 0x1FFFFFFF;
	uint32_t result = 0;
	// The game's buffers are in main memory (cached or not); anything else, or a
	// buffer that wraps, is not something the original could find a code in here.
	if((physical < 0x02000000) && (size <= (0x02000000 - physical)))
	{
		// Same state machine: after a hit of the wrong kind the search goes on
		// behind that code, and a partial match restarts on the next byte.
		const uint8_t* data = m_ram + physical;
		uint32_t state = 0;
		for(uint32_t i = 0; i < size; i++)
		{
			uint8_t byte = data[i];
			switch(state)
			{
			case 0:
				if(byte == 0) state = 1;
				break;
			case 1:
				state = (byte == 0) ? 2 : 0;
				break;
			case 2:
				if(byte == 1) state = 3;
				else if(byte != 0) state = 0;
				break;
			default:
				if(StartCodeKind(byte) & mask)
				{
					result = address + i - 3;
					i = size;
				}
				state = 0;
				break;
			}
		}
	}
	gpr[CMIPS::V0].nD0 = static_cast<int32_t>(result);
}

void CMpegHle::OnSyscall(void* context, CMIPS& ee)
{
	auto hle = static_cast<CMpegHle*>(context);
	uint32_t func = ee.m_State.nGPR[CMIPS::V1].nV0;
	if(func == SYSCALL_MEMCPY_BYTES)
	{
		hle->HandleMemcpyBytes(ee);
		return;
	}
	if(func == SYSCALL_SCAN)
	{
		hle->HandleStartCodeScan(ee);
		return;
	}
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
				// The ring's bookkeeping, to stay within what has been written.
				m_viBuf = 0;
				if(variant.library30)
				{
					uint32_t sysAddress = Read32(m_ram, m_mp + 0x40);
					uint32_t noDataFunction = Read32(m_ram, sysAddress + SYS30_NODATA_FUNCTION);
					bool known = (noDataFunction != 0);
					for(uint32_t i = 0; known && (i < SOFDEC_NODATA_WORDS); i++) known = (Read32(m_ram, noDataFunction + i * 4) == g_sofdecNoData[i]);
					if(known) m_viBuf = Read32(m_ram, Read32(m_ram, sysAddress + SYS30_NODATA_DATA) + 8120) + SOFDEC_VIBUF;
					Log("ViBuf at 0x%08X\n", m_viBuf);
				}
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
			auto waitStart = std::chrono::steady_clock::now();
			bool ready = m_pictureReady.wait_for(lock, std::chrono::seconds(3), [&]() { return !m_pictures.empty() || m_decoderStarved || m_quit; });
			m_stats.waitMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waitStart).count();
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
		m_starvedSince = 0;
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
	if(variant.library30 && (noData != 0))
	{
		// Hold back the end of the movie for a while, while the callback runs.
		bool known = true;
		for(uint32_t i = 0; known && (i < SOFDEC_NODATA_WORDS); i++) known = (Read32(m_ram, noData + i * 4) == g_sofdecNoData[i]);
		if(known)
		{
			if(m_starvedSince == 0) m_starvedSince = m_vblanks + 1;
			uint32_t player = Read32(m_ram, Read32(m_ram, sys + SYS30_NODATA_DATA) + 8120);
			uint32_t mayEnd = ((m_vblanks + 1 - m_starvedSince) >= STARVED_GRACE_VBLANKS) ? 1 : 0;
			if(mayEnd && (m_loggedEnd != m_starvedSince))
			{
				m_loggedEnd = m_starvedSince;
				Log("decoder out of data for %u vblanks: the callback may end the movie\n", STARVED_GRACE_VBLANKS);
			}
			Write32(m_ram, player + 4264 + 20, mayEnd);
		}
		// Waits only end a movie through the callback here.
		m_waitsThisPicture = 0;
	}
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
			if(hle->m_viBuf != 0)
			{
				// Never past the units queued: the DMA chain runs on around the whole
				// ring, over old data (at the end of a movie, nothing new comes).
				uint32_t ring = Read32(hle->m_ram, hle->m_viBuf + 0x00) & 0x01FFFFFF;
				uint32_t units = Read32(hle->m_ram, hle->m_viBuf + 0x08);
				uint32_t first = Read32(hle->m_ram, hle->m_viBuf + 0x0C);
				uint32_t queued = Read32(hle->m_ram, hle->m_viBuf + 0x10);
				const uint8_t* ringStart = hle->m_ram + ring;
				if((units != 0) && (data >= ringStart) && (data < ringStart + units * VIBUF_UNIT))
				{
					uint32_t offset = static_cast<uint32_t>(data - ringStart);
					uint32_t unitsFromFirst = ((offset / VIBUF_UNIT) + units - (first % units)) % units;
					uint32_t allowed = (unitsFromFirst < queued) ? ((queued - unitsFromFirst) * VIBUF_UNIT - (offset % VIBUF_UNIT)) : 0;
					size = std::min<uint32_t>(size, allowed);
				}
			}
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
	hle->m_starvedSince = 0;
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
				m_stats.starved++;
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
