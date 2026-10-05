#pragma once

#include <atomic>
#include <memory>
#include <string>
#include "FrameSink.h"
#include "PadHandler.h"
#include "sound/SoundHandler.h"
#include "signal/Signal.h"

class CPS2VM;

// Frontend-independent lifetime management of one emulated PS2: data paths,
// VM creation, renderer hookup and booting a disc image or ELF.
class CEmuSession
{
public:
	struct CONFIG
	{
		std::string dataPath;             // writable: config, memory cards, saves, logs
		std::string resourcesPath;        // read-only: GameConfig.xml etc.
		bool limitFrameRate = true;
		bool gsThreaded = true;
		CPadHandler::FactoryFunction padFactory;
		CSoundHandler::FactoryFunction soundFactory;
	};

	explicit CEmuSession(const CONFIG&);
	~CEmuSession();

	CEmuSession(const CEmuSession&) = delete;
	CEmuSession& operator=(const CEmuSession&) = delete;

	static bool IsBootableExecutable(const std::string& path);
	static bool IsBootableDiscImage(const std::string& path);

	// Boots an .elf directly or a disc image (.iso/.cso/.chd/.isz/.cue/.mds/.bin).
	// Throws std::runtime_error on failure.
	void Boot(const std::string& path);

	void Pause();
	void Resume();
	bool IsRunning() const;

	CFrameMailbox& GetFrames()
	{
		return m_frames;
	}

	// Number of frames the emulated machine has completed (vblanks with GS activity).
	uint64_t GetVmFrameCount() const
	{
		return m_vmFrames.load();
	}

	CPS2VM* GetVm()
	{
		return m_vm.get();
	}

	// Called once at startup before anything touches CAppConfig.
	static void SetDataPaths(const std::string& dataPath, const std::string& resourcesPath);

private:
	std::unique_ptr<CPS2VM> m_vm;
	CFrameMailbox m_frames;
	std::atomic<uint64_t> m_vmFrames{0};
	Framework::CSignal<void()>::Connection m_newFrameConnection;
	bool m_booted = false;
};
