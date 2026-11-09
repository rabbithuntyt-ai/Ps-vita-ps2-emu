#include "EmuSession.h"

#include <algorithm>
#include <vector>
#include <cctype>
#include <cstring>
#include <stdexcept>
#include <thread>

#include "AppConfig.h"
#include "DiskUtils.h"
#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "PathUtils.h"
#include "ee/PS2OS.h"
#include "ee/Vpu.h"
#include "ee/Ee_SubSystem.h"
#include "MA_MIPSIV.h"
#include "BasicBlock.h"
#include "ee/EeExecutor.h"
#include "offsetof_def.h"
#if defined(__arm__)
#include "Jitter_CodeGen_AArch32.h"
#endif
#include "iop/IopBios.h"
#include "iop/Iop_SubSystem.h"
#include "GSH_Software.h"
#include "ThreadProfiler.h"
#include "Mpeg2Decoder.h"
#include "EmuProfile.h"

namespace
{
	fs::path g_dataPath;
	fs::path g_resourcesPath;
	bool g_indirectDispatchWanted = true;
	bool g_pinnedFieldsWanted = true;
	bool g_fastRamWanted = true;

	std::string LowerExtension(const std::string& path)
	{
		auto ext = fs::path(path).extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return ext;
	}
}

// Play! requires every executable to say where its writable data lives.
fs::path CAppConfig::GetBasePath() const
{
	return g_dataPath;
}

void CEmuSession::SetDataPaths(const std::string& dataPath, const std::string& resourcesPath)
{
	g_dataPath = dataPath;
	g_resourcesPath = resourcesPath;
	Framework::PathUtils::EnsurePathExists(g_dataPath);
#if defined(__vita__)
	Framework::PathUtils::SetFilesDirPath(dataPath.c_str());
	Framework::PathUtils::SetAppResourcesPath(resourcesPath.c_str());
#endif
}

bool CEmuSession::IsBootableExecutable(const std::string& path)
{
	return LowerExtension(path) == ".elf";
}

bool CEmuSession::IsBootableDiscImage(const std::string& path)
{
	static const char* extensions[] = {".iso", ".cso", ".chd", ".isz", ".cue", ".mds", ".bin"};
	auto ext = LowerExtension(path);
	return std::any_of(std::begin(extensions), std::end(extensions), [&](const char* e) { return ext == e; });
}

std::string CEmuSession::GetDiscSerial(const std::string& path)
{
	if(!IsBootableDiscImage(path)) return std::string();
	try
	{
		std::string serial;
		if(DiskUtils::TryGetDiskId(path, &serial)) return serial;
	}
	catch(const std::exception&)
	{
	}
	return std::string();
}

CEmuSession::CEmuSession(const CONFIG& config)
{
	if(g_dataPath.empty())
	{
		SetDataPaths(config.dataPath, config.resourcesPath);
	}

	auto& appConfig = CAppConfig::GetInstance();
	appConfig.SetPreferenceBoolean(PREF_PS2_LIMIT_FRAMERATE, config.limitFrameRate);

	m_vm = std::make_unique<CPS2VM>();
	m_vm->Initialize();
	m_vm->ReloadFrameRateLimit();

	m_gsPump = config.gsPump;
	m_vu1ThreadInit = config.vu1ThreadInit;
	m_spuThreadInit = config.spuThreadInit;
	m_emuThreadInit = config.emuThreadInit;
	m_frameHook = config.frameHook;
	m_gsShutdown = config.gsShutdown;
	auto frames = &m_frames;
	if(config.gsFactory)
	{
		m_vm->CreateGSHandler(config.gsFactory);
	}
	else
	{
		CGSH_Software::OPTIONS gsOptions;
		gsOptions.gsThreaded = config.gsThreaded;
		gsOptions.rasterizerThreads = config.rasterizerThreads;
		gsOptions.interlacedRendering = config.interlacedRendering;
		gsOptions.frameSkip = config.frameSkip;
		m_vm->CreateGSHandler(CGSH_Software::GetFactoryFunction(
		    [frames](std::vector<uint32>& pixels, uint32 width, uint32 height) {
			    frames->Publish(pixels, width, height);
		    },
		    gsOptions));
	}

	m_speedHacks.interlacedRendering = config.interlacedRendering;
	m_speedHacks.frameSkip = config.frameSkip;

	if(config.padFactory) m_vm->CreatePadHandler(config.padFactory);
	if(config.soundFactory) m_vm->CreateSoundHandler(config.soundFactory);

	for(auto& count : m_profileCounts) count = 0;
	for(auto& count : m_iopProfileCounts) count = 0;
	m_profiler = std::thread([this, init = config.profilerThreadInit]() {
		ThreadProfiler::RegisterCurrentThread("Profiler");
		if(init) init();
		ProfilerProc();
	});

	m_newFrameConnection = m_vm->OnNewFrame.Connect([this]() {
		// Runs on the emulation thread (EE, IOP, VU, SPU).
		if(m_vmFrames++ == 0)
		{
			ThreadProfiler::RegisterCurrentThread("PS2 (EE/IOP/VU)");
			if(m_emuThreadInit) m_emuThreadInit();
		}
		// Called before the VM resets the frame's counters: average EE idle
		// time over whole frames (half a second at 60 fps).
		auto info = m_vm->GetCpuUtilisationInfo();
		m_eeIdleTicks += std::max(info.eeIdleTicks, 0);
		m_eeBusyTicks += std::max(info.eeTotalTicks, 0);
		m_eeExecutedTicks += std::max(info.eeTotalTicks - info.eeIdleTicks, 0);
		m_eeIdleTotalTicks += std::max(info.eeIdleTicks, 0);
		if(++m_eeIdleFrames >= 30)
		{
			int64_t total = m_eeIdleTicks + m_eeBusyTicks;
			m_eeIdleRatio = (total > 0) ? static_cast<float>(m_eeIdleTicks) / static_cast<float>(total) : 0.0f;
			m_eeIdleTicks = m_eeBusyTicks = 0;
			m_eeIdleFrames = 0;
		}
		if(m_frameHook) m_frameHook(m_vmFrames.load(), m_eeExecutedTicks.load());
	});
}

CEmuSession::~CEmuSession()
{
	m_stopProfiler = true;
	if(m_profiler.joinable()) m_profiler.join();
	if(m_vm)
	{
		RunPumped([this]() { m_vm->Pause(); });
		m_vm->m_ee->m_vpu1->SetThreaded(false);
		m_vm->SetSpuThreaded(false);
		if(m_gsShutdown) m_gsShutdown();
		m_vm->DestroyPadHandler();
		m_vm->DestroySoundHandler();
		m_vm->DestroyGSHandler();
		m_vm->Destroy();
	}
}

void CEmuSession::Boot(const std::string& path)
{
	if(!fs::exists(path))
	{
		throw std::runtime_error("File not found: " + path);
	}

	if(!IsBootableExecutable(path) && !IsBootableDiscImage(path))
	{
		throw std::runtime_error("Unsupported file type: " + path);
	}

	std::string error;
	RunPumped([&]() {
		try
		{
			m_vm->Pause();
			m_vm->Reset();
			if(IsBootableExecutable(path))
			{
				m_vm->m_ee->m_os->BootFromFile(path);
			}
			else
			{
				CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, path);
				m_vm->CDROM0_SyncPath();
				m_vm->m_ee->m_os->BootFromCDROM();
			}
		}
		catch(const std::exception& e)
		{
			error = e.what();
		}
	});
	if(!error.empty()) throw std::runtime_error(error);
	m_booted = true;
	SetSpeedHacks(m_speedHacks); //Reset() restored the EE clock
	m_vm->Resume();
}

void CEmuSession::SetSpeedHacks(const SPEED_HACKS& hacks)
{
	m_speedHacks = hacks;
	bool wasRunning = IsRunning();
	RunPumped([&]() {
		if(wasRunning) m_vm->Pause();
		uint32_t percent = std::clamp<uint32_t>(hacks.eeCycleRatePercent, 25, 300);
		m_vm->SetEeFrequencyScale(percent, 100);
		// The VM is paused: VU1 is idle (or gets synchronized) while switching.
		auto spuInit = m_spuThreadInit;
		m_vm->SetSpuThreaded(hacks.threadedSpu, [spuInit]() {
			ThreadProfiler::RegisterCurrentThread("SPU2 audio");
			if(spuInit) spuInit();
		});
		auto init = m_vu1ThreadInit;
		m_vm->m_ee->m_vpu1->SetThreaded(hacks.threadedVu1, [init]() {
			ThreadProfiler::RegisterCurrentThread("VU1");
			if(init) init();
		});
	});
	if(wasRunning) m_vm->Resume();

	if(auto gs = dynamic_cast<CGSH_Primitives*>(m_vm->GetGSHandler()))
	{
		gs->SendGSCall([gs, hacks]() {
			gs->SetInterlacedRendering(hacks.interlacedRendering);
			gs->SetFrameSkip(hacks.frameSkip);
		});
	}
}

uint32_t CEmuSession::GetGsRasterMicros()
{
	auto gs = dynamic_cast<CGSH_Software*>(m_vm->GetGSHandler());
	return gs ? gs->GetLastFrameRasterMicros() : 0;
}

float CEmuSession::GetEeIdleRatio()
{
	return m_eeIdleRatio.load();
}

CEmuSession::DEBUG_STATE CEmuSession::GetDebugState()
{
	DEBUG_STATE state;
	auto& ee = m_vm->m_ee->m_EE.m_State;
	state.eePc = ee.nPC;
	state.eeRa = ee.nGPR[CMIPS::RA].nV0;
	auto& iop = m_vm->m_iop->m_cpu.m_State;
	state.iopPc = iop.nPC;
	state.iopRa = iop.nGPR[CMIPS::RA].nV0;
	if(auto bios = dynamic_cast<CIopBios*>(m_vm->m_iop->m_bios.get()))
	{
		state.iopThread = bios->GetCurrentThreadIdRaw();
	}
	state.intcStat = m_vm->m_ee->m_intc.GetRegister(CINTC::INTC_STAT);
	state.intcMask = m_vm->m_ee->m_intc.GetRegister(CINTC::INTC_MASK);
	state.dmacStat = m_vm->m_ee->m_dmac.GetRegister(CDMAC::D_STAT);
	for(int i = 0; i < 32; i++) state.eeGpr[i] = ee.nGPR[i].nV0;
	state.ipuCtrl = m_vm->m_ee->m_ipu.GetRegister(CIPU::IPU_CTRL);
	state.ipuBp = m_vm->m_ee->m_ipu.GetRegister(CIPU::IPU_BP);
	state.d3Chcr = m_vm->m_ee->m_dmac.GetRegister(CDMAC::D3_CHCR);
	state.d3Qwc = m_vm->m_ee->m_dmac.GetRegister(CDMAC::D3_QWC);
	state.d4Chcr = m_vm->m_ee->m_dmac.GetRegister(CDMAC::D4_CHCR);
	state.d4Madr = m_vm->m_ee->m_dmac.GetRegister(CDMAC::D4_MADR);
	state.d4Qwc = m_vm->m_ee->m_dmac.GetRegister(CDMAC::D4_QWC);
	auto& vpu1 = *m_vm->m_ee->m_vpu1;
	state.vu1Start = vpu1.GetProgramStart();
	state.vu1Pc = vpu1.GetCurrentPc();
	state.vu1RunMs = vpu1.GetProgramRunMs();
	return state;
}

uint64_t CEmuSession::GetEeExecutedTicks() const
{
	auto info = m_vm->GetCpuUtilisationInfo();
	return m_eeExecutedTicks + std::max(info.eeTotalTicks - info.eeIdleTicks, 0);
}

void CEmuSession::SetSafeJit(bool safe)
{
	// EE: 64-bit compares/logic on known sign-extended values use 32-bit operations
	CMA_MIPSIV::SetSignExtTracking(!safe);
#if defined(__arm__)
	Jitter::CCodeGen_AArch32::SetConservativeAllocation(safe);
	// Shared frames across linked blocks (no prolog/epilog between blocks).
	Jitter::CCodeGen_AArch32::SetChainedFrames(!safe);
	// EE/IOP loads and stores as single ops: registers stay allocated across them.
	Jitter::CCodeGen_AArch32::SetGuestMemoryOps(!safe);
	// Cycle quota and page table pointer kept in registers across linked blocks.
	// Loads and stores in RAM without the page lookup (guest memory ops only); the RAM's
	// base pointer then takes the page table pointer's pinned register.
	CBasicBlock::EnableFastRam(!safe && g_fastRamWanted);
	if(!safe && g_pinnedFieldsWanted)
		Jitter::CCodeGen_AArch32::SetPinnedFields(offsetof(CMIPS, m_State.cycleQuota),
		                                          CBasicBlock::GetFastRam() ? offsetof(CMIPS, m_fastRamBase) : offsetof(CMIPS, m_pageLookup));
	else
		Jitter::CCodeGen_AArch32::SetPinnedFields(-1, -1);
	// Exceptions from memory handlers flagged in the pinned quota: block exits after
	// loads and stores skip the exception test.
	Jitter::CCodeGen_AArch32::SetExceptionPoison(offsetof(CMIPS, m_State.nHasException));
	CBasicBlock::EnableMemoryExceptionPoison(Jitter::CCodeGen_AArch32::GetExceptionPoison() && Jitter::CCodeGen_AArch32::GetChainedFrames() &&
	                                         Jitter::CCodeGen_AArch32::GetGuestMemoryOps());
#else
	(void)safe;
#endif
	// Indirect jumps (function returns...) resolved in compiled code (needs chained frames).
	CBasicBlock::EnableIndirectDispatch(!safe && g_indirectDispatchWanted);
}

void CEmuSession::SetPinnedFields(bool enabled)
{
	g_pinnedFieldsWanted = enabled;
}

void CEmuSession::SetFastRam(bool enabled)
{
	g_fastRamWanted = enabled;
}

void CEmuSession::SetIndirectDispatch(bool enabled)
{
	g_indirectDispatchWanted = enabled;
}

void CEmuSession::SetCheapExits(bool enabled)
{
	CBasicBlock::EnableCheapExits(enabled);
}

void CEmuSession::SetRegions(bool enabled)
{
	CEeExecutor::SetRegions(enabled);
}

void CEmuSession::ProfilerProc()
{
	while(!m_stopProfiler)
	{
		std::this_thread::sleep_for(std::chrono::microseconds(1000));
		unsigned int section = EmuProfile::g_section.load(std::memory_order_relaxed);
		if(section < EmuProfile::SECTION_COUNT) m_profileCounts[section]++;
		unsigned int iopSection = EmuProfile::g_iopSection.load(std::memory_order_relaxed);
		if(iopSection < EmuProfile::SECTION_COUNT) m_iopProfileCounts[iopSection]++;
		unsigned int stage = Mpeg2Profile::g_stage.load(std::memory_order_relaxed);
		if(stage < Mpeg2Profile::STAGE_COUNT) Mpeg2Profile::g_samples[stage].fetch_add(1, std::memory_order_relaxed);
		// Racy reads of the PC: blocks store it when they exit, so it points
		// at (or just past) the code that is running.
		if((section == EmuProfile::SECTION_EE) && m_vm)
		{
			AddHotSample(m_eeHotPc, m_eeHotCount, m_eeHotSamples, m_vm->m_ee->m_EE.m_State.nPC);
		}
		if((section == EmuProfile::SECTION_IOP) && m_vm)
		{
			AddHotSample(m_iopHotPc, m_iopHotCount, m_iopHotSamples, m_vm->m_iop->m_cpu.m_State.nPC);
		}
	}
}

void CEmuSession::AddHotSample(std::atomic<uint32_t>* pcs, std::atomic<uint32_t>* counts, std::atomic<uint32_t>& samples, uint32_t pc)
{
	uint32_t bucket = (pc & ~0x1Fu) | 1;
	uint32_t slot = ((bucket >> 5) * 2654435761u) % EE_HOTSPOT_SLOTS;
	// Open addressing; only the profiler thread writes the table.
	for(unsigned int probe = 0; probe < 16; probe++, slot = (slot + 1) % EE_HOTSPOT_SLOTS)
	{
		uint32_t current = pcs[slot].load(std::memory_order_relaxed);
		if(current == 0) pcs[slot].store(current = bucket, std::memory_order_relaxed);
		if(current != bucket) continue;
		counts[slot].fetch_add(1, std::memory_order_relaxed);
		break;
	}
	samples.fetch_add(1, std::memory_order_relaxed);
}

void CEmuSession::ResetEeHotspots()
{
	// Counts only: clearing addresses would race with the profiler thread.
	for(auto& count : m_eeHotCount) count.store(0, std::memory_order_relaxed);
	m_eeHotSamples.store(0, std::memory_order_relaxed);
	for(auto& count : m_iopHotCount) count.store(0, std::memory_order_relaxed);
	m_iopHotSamples.store(0, std::memory_order_relaxed);
}

void CEmuSession::WriteEeHotspots(std::FILE* file, unsigned int count)
{
	WriteHotspots(file, count, "EE", m_eeHotPc, m_eeHotCount, m_eeHotSamples);
}

void CEmuSession::WriteIopHotspots(std::FILE* file, unsigned int count)
{
	WriteHotspots(file, count, "IOP", m_iopHotPc, m_iopHotCount, m_iopHotSamples);
}

void CEmuSession::WriteHotspots(std::FILE* file, unsigned int count, const char* name, std::atomic<uint32_t>* pcs, std::atomic<uint32_t>* counts, std::atomic<uint32_t>& samplesTotal)
{
	std::vector<std::pair<uint32_t, uint32_t>> hot;
	for(unsigned int i = 0; i < EE_HOTSPOT_SLOTS; i++)
	{
		uint32_t samples = counts[i].load(std::memory_order_relaxed);
		if(samples != 0) hot.emplace_back(pcs[i].load(std::memory_order_relaxed) & ~0x1Fu, samples);
	}
	std::sort(hot.begin(), hot.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	uint32_t total = std::max<uint32_t>(samplesTotal.load(std::memory_order_relaxed), 1);
	std::fprintf(file, "%s samples: %u\n", name, total);
	for(unsigned int i = 0; (i < count) && (i < hot.size()); i++)
	{
		std::fprintf(file, "%08X %5.1f%%\n", hot[i].first, hot[i].second * 100.0f / total);
	}
}

CEmuSession::PROFILE CEmuSession::TakeProfile(bool iopThread)
{
	PROFILE profile;
	uint32_t counts[EmuProfile::SECTION_COUNT];
	for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++)
	{
		counts[i] = (iopThread ? m_iopProfileCounts[i] : m_profileCounts[i]).exchange(0);
		profile.samples += counts[i];
	}
	if(profile.samples != 0)
	{
		for(unsigned int i = 0; i < EmuProfile::SECTION_COUNT; i++) profile.share[i] = static_cast<float>(counts[i]) / static_cast<float>(profile.samples);
	}
	return profile;
}

uint32_t CEmuSession::ReadEeWord(uint32_t address)
{
	address &= 0x1FFFFFFC; //kseg0/kseg1 mirrors
	if(address >= PS2::EE_RAM_SIZE) return 0;
	uint32_t value = 0;
	std::memcpy(&value, m_vm->m_ee->m_ram + address, 4);
	return value;
}

void CEmuSession::Pause()
{
	RunPumped([this]() { m_vm->Pause(); });
}

void CEmuSession::RunPumped(const std::function<void()>& work)
{
	if(!m_gsPump)
	{
		work();
		return;
	}
	std::atomic<bool> done{false};
	std::thread worker([&]() {
		work();
		done = true;
	});
	while(!done)
	{
		m_gsPump();
	}
	worker.join();
}

void CEmuSession::Resume()
{
	if(m_booted) m_vm->Resume();
}

bool CEmuSession::IsRunning() const
{
	return m_vm->GetStatus() == CVirtualMachine::RUNNING;
}
