#include "EmuSession.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <thread>

#include "AppConfig.h"
#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "PathUtils.h"
#include "ee/PS2OS.h"
#include "ee/Vpu.h"
#include "ee/Ee_SubSystem.h"
#include "GSH_Software.h"
#include "ThreadProfiler.h"

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

	m_newFrameConnection = m_vm->OnNewFrame.Connect([this]() {
		// Runs on the emulation thread (EE, IOP, VU, SPU).
		if(m_vmFrames++ == 0) ThreadProfiler::RegisterCurrentThread("PS2 (EE/IOP/VU)");
	});
}

CEmuSession::~CEmuSession()
{
	if(m_vm)
	{
		RunPumped([this]() { m_vm->Pause(); });
		m_vm->m_ee->m_vpu1->SetThreaded(false);
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
	auto info = m_vm->GetCpuUtilisationInfo();
	if(info.eeTotalTicks <= 0) return 0;
	return static_cast<float>(info.eeIdleTicks) / static_cast<float>(info.eeTotalTicks + info.eeIdleTicks);
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
