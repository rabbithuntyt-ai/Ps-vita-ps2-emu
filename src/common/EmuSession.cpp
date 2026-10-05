#include "EmuSession.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "AppConfig.h"
#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "PathUtils.h"
#include "ee/PS2OS.h"
#include "GSH_Software.h"

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

	auto frames = &m_frames;
	CGSH_Software::OPTIONS gsOptions;
	gsOptions.gsThreaded = config.gsThreaded;
	gsOptions.rasterizerThreads = config.rasterizerThreads;
	gsOptions.interlacedRendering = config.interlacedRendering;
	gsOptions.frameSkip = config.frameSkip;
	m_vm->CreateGSHandler(CGSH_Software::GetFactoryFunction(
	    [frames](const uint32* pixels, uint32 width, uint32 height) {
		    frames->Publish(pixels, width, height);
	    },
	    gsOptions));

	if(config.padFactory) m_vm->CreatePadHandler(config.padFactory);
	if(config.soundFactory) m_vm->CreateSoundHandler(config.soundFactory);

	m_newFrameConnection = m_vm->OnNewFrame.Connect([this]() { m_vmFrames++; });
}

CEmuSession::~CEmuSession()
{
	if(m_vm)
	{
		m_vm->Pause();
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

	m_vm->Pause();
	m_vm->Reset();
	if(IsBootableExecutable(path))
	{
		m_vm->m_ee->m_os->BootFromFile(path);
	}
	else if(IsBootableDiscImage(path))
	{
		CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH, path);
		m_vm->CDROM0_SyncPath();
		m_vm->m_ee->m_os->BootFromCDROM();
	}
	else
	{
		throw std::runtime_error("Unsupported file type: " + path);
	}
	m_booted = true;
	m_vm->Resume();
}

void CEmuSession::Pause()
{
	m_vm->Pause();
}

void CEmuSession::Resume()
{
	if(m_booted) m_vm->Resume();
}

bool CEmuSession::IsRunning() const
{
	return m_vm->GetStatus() == CVirtualMachine::RUNNING;
}
