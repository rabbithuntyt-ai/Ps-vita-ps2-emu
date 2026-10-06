#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include "FrameSink.h"
#include "PadHandler.h"
#include "gs/GSHandler.h"
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
		uint32_t rasterizerThreads = 1;   // threads rasterizing (GS thread included)
		bool interlacedRendering = false; // speed hack: draw alternate lines per frame
		uint32_t frameSkip = 0;           // speed hack: skip N of N+1 frames
		// Renderer override (GPU renderer). The software renderer is used
		// when empty; it publishes frames to GetFrames().
		CGSHandler::FactoryFunction gsFactory;
		// Non-threaded renderers run on the frontend's thread: gsPump executes
		// pending GS work and is called repeatedly whenever the session waits
		// on the emulation thread (pause, boot...), so that the emulation
		// thread can never deadlock waiting on the GS. gsShutdown runs on the
		// same thread before the renderer is destroyed.
		std::function<void()> gsPump;
		std::function<void()> gsShutdown;
		// Runs first on the VU1 worker thread (e.g. to pin it to a core).
		std::function<void()> vu1ThreadInit;
		// Same for the SPU2 mixing worker.
		std::function<void()> spuThreadInit;
		// Runs once on the emulation (EE/IOP) thread when it starts.
		std::function<void()> emuThreadInit;
		CPadHandler::FactoryFunction padFactory;
		CSoundHandler::FactoryFunction soundFactory;
	};

	struct SPEED_HACKS
	{
		uint32_t eeCycleRatePercent = 100; // <100 underclocks the EE: less work per frame
		bool interlacedRendering = false;  // draw alternate lines per frame
		uint32_t frameSkip = 0;            // skip N of N+1 frames
		bool threadedVu1 = false;          // run VU1 microprograms on their own thread/core
		bool threadedSpu = false;          // mix SPU2 audio on its own thread/core
	};

	explicit CEmuSession(const CONFIG&);
	~CEmuSession();

	CEmuSession(const CEmuSession&) = delete;
	CEmuSession& operator=(const CEmuSession&) = delete;

	static bool IsBootableExecutable(const std::string& path);
	static bool IsBootableDiscImage(const std::string& path);
	// Disc serial from SYSTEM.CNF (e.g. "SLUS-20228"); empty when unknown.
	static std::string GetDiscSerial(const std::string& path);

	// Boots an .elf directly or a disc image (.iso/.cso/.chd/.isz/.cue/.mds/.bin).
	// Throws std::runtime_error on failure.
	void Boot(const std::string& path);

	void Pause();
	void Resume();
	bool IsRunning() const;

	// Can be called at any time; applied safely on the emulation threads.
	void SetSpeedHacks(const SPEED_HACKS&);
	const SPEED_HACKS& GetSpeedHacks() const
	{
		return m_speedHacks;
	}

	CFrameMailbox& GetFrames()
	{
		return m_frames;
	}

	// Number of frames the emulated machine has completed (vblanks with GS activity).
	uint64_t GetVmFrameCount() const
	{
		return m_vmFrames.load();
	}

	// Profiling: GS rasterization time of the last frame, and the share of EE
	// cycles the game spent idling (waiting for vsync etc.) over the last 30 frames.
	uint32_t GetGsRasterMicros();
	float GetEeIdleRatio();

	// Snapshot of the emulated CPUs for diagnosing hangs. Read without
	// stopping the VM, so values may be slightly inconsistent.
	struct DEBUG_STATE
	{
		uint32_t eePc = 0, eeRa = 0;
		uint32_t iopPc = 0, iopRa = 0;
		int32_t iopThread = -1;
		uint32_t intcStat = 0, intcMask = 0, dmacStat = 0;
		// VU1: microprogram start, current PC, ms it has been running (0: idle).
		uint32_t vu1Start = 0, vu1Pc = 0, vu1RunMs = 0;
		uint32_t eeGpr[32] = {}; // low 32 bits of the EE registers
	};
	DEBUG_STATE GetDebugState();
	// A word of EE main memory (0 outside RAM), for dumping code.
	uint32_t ReadEeWord(uint32_t address);

	CPS2VM* GetVm()
	{
		return m_vm.get();
	}

	// ARM JIT: keep vector/float values in memory instead of NEON/VFP
	// registers (slower; to rule out register allocation bugs). Takes effect
	// for code compiled afterwards: set before booting. No-op elsewhere.
	static void SetSafeJit(bool);

	// Called once at startup before anything touches CAppConfig.
	static void SetDataPaths(const std::string& dataPath, const std::string& resourcesPath);

private:
	// Runs 'work' (which may block on the emulation thread) while pumping a
	// non-threaded GS on the calling thread.
	void RunPumped(const std::function<void()>& work);

	std::unique_ptr<CPS2VM> m_vm;
	std::function<void()> m_gsPump;
	std::function<void()> m_gsShutdown;
	std::function<void()> m_vu1ThreadInit;
	std::function<void()> m_spuThreadInit;
	std::function<void()> m_emuThreadInit;
	CFrameMailbox m_frames;
	std::atomic<uint64_t> m_vmFrames{0};
	// EE idle accounting (emulation thread), published every 30 frames.
	int64_t m_eeIdleTicks = 0, m_eeBusyTicks = 0;
	uint32_t m_eeIdleFrames = 0;
	std::atomic<float> m_eeIdleRatio{0.0f};
	Framework::CSignal<void()>::Connection m_newFrameConnection;
	bool m_booted = false;
	SPEED_HACKS m_speedHacks;
};
