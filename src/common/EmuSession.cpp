#include "EmuSession.h"

#include <algorithm>
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
#if defined(__arm__)
#include "Jitter_CodeGen_AArch32.h"
#endif
#include "iop/IopBios.h"
#include "iop/Iop_SubSystem.h"
#include "GSH_Software.h"
#include "ThreadProfiler.h"
#include "EmuProfile.h"

namespace
{
	fs::path g_dataPath;
	fs::path g_resourcesPath;

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
		if(++m_eeIdleFrames >= 30)
		{
			int64_t total = m_eeIdleTicks + m_eeBusyTicks;
			m_eeIdleRatio = (total > 0) ? static_cast<float>(m_eeIdleTicks) / static_cast<float>(total) : 0.0f;
			m_eeIdleTicks = m_eeBusyTicks = 0;
			m_eeIdleFrames = 0;
		}
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

void CEmuSession::SetSafeJit(bool safe)
{
#if defined(__arm__)
	Jitter::CCodeGen_AArch32::SetConservativeAllocation(safe);
	// Shared frames across linked blocks (no prolog/epilog between blocks).
	Jitter::CCodeGen_AArch32::SetChainedFrames(!safe);
#else
	(void)safe;
#endif
}

void CEmuSession::ProfilerProc()
{
	while(!m_stopProfiler)
	{
		std::this_thread::sleep_for(std::chrono::microseconds(500));
		unsigned int section = EmuProfile::g_section.load(std::memory_order_relaxed);
		if(section < 16) m_profileCounts[section]++;
	}
}

CEmuSession::PROFILE CEmuSession::TakeProfile()
{
	PROFILE profile;
	uint32_t counts[16];
	for(int i = 0; i < 16; i++)
	{
		counts[i] = m_profileCounts[i].exchange(0);
		profile.samples += counts[i];
	}
	if(profile.samples != 0)
	{
		for(int i = 0; i < 16; i++) profile.share[i] = static_cast<float>(counts[i]) / static_cast<float>(profile.samples);
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
