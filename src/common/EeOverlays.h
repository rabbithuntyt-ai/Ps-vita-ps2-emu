#pragma once

// Native overlays of hot EE functions (docs/PATH_TO_30FPS.md, E3).
//
// An overlay is a C++ function that does what a game function does, on emulated
// memory and registers, bit for bit as the recompiler would: the same float
// operations in the same order (with the recompiler's clamps), the same writes,
// and the same final values in every register the original leaves behind
// (temporaries included). Only the VU0 flag pipelines (MAC, sticky, clip) are
// not kept up to date by overlays that use VU0 macro instructions.
//
// Overlays are found by the function's code: every word must match (some words
// are masked where they hold an address, which the overlay then reads from the
// code). Library functions are keyed by code alone and serve every game.
//
// Entry: the EE recompiler asks at each new block (EeOverlay::g_lookup, patch
// 0110); the block at an overlay's address calls it first and returns to ra.
// Overlays that ask for it can also enter through a syscall stub patched over
// the function's first words (EeOverlay::SYSCALL).
//
// Verification (PC runner, --overlay-verify): at sampled entries the recompiled
// original runs first with ra pointing at a stub in the BIOS area; when it
// returns there, its result is kept aside, the state before the call is put
// back, the overlay runs, and memory (RAM, scratchpad) and registers are
// compared. The original's result is what the game continues with. The first
// differences are written to a log with the call's inputs.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "ee/EeOverlay.h"

class CMIPS;
class CPS2VM;

class CEeOverlays
{
public:
	struct SETTINGS
	{
		bool enabled = true;
		// Names of the overlays switched off (empty: all on), or on ("" = default set).
		std::vector<std::string> disabled;
		bool verify = false;       // compare against the original instead of replacing it
		uint32_t verifyFirst = 200; // every call of an overlay up to this many...
		uint32_t verifyEvery = 50;  // ...then one in N
		uint32_t verifyMax = 4000;  // per overlay
		bool selfTest = false;      // breaks each checked result on purpose (the check must see it)
	};

	struct STATS
	{
		std::string name;
		uint32_t address = 0;
		uint64_t calls = 0;
		uint64_t taken = 0;
		uint64_t verified = 0;
		uint64_t mismatches = 0;
		uint64_t flagOnly = 0;    // only VU0 flag state differed (not checked by design)
		uint64_t interrupted = 0; // a slice ended inside the original: not compared
	};

	CEeOverlays();
	~CEeOverlays();

	// Before the game's code is compiled (or call ClearCompiledCode after).
	void Enable(CPS2VM&, const SETTINGS&, const char* logPath);
	void Disable();
	bool IsEnabled() const
	{
		return m_vm != nullptr;
	}

	std::vector<STATS> GetStats() const;
	void WriteReport(std::FILE*) const;

	// Names of all overlays, for settings and help texts.
	static std::vector<std::string> GetNames();

	struct INSTANCE;

private:
	static bool Lookup(void*, CMIPS&, uint32 address, EeOverlay::HOOK&);
	static uint32 Dispatch(CMIPS*, uint32 id);
	static void OnSyscall(void*, CMIPS&);
	static void OnVBlank(void*, CMIPS&);

	uint32_t Run(CMIPS&, INSTANCE&);
	uint32_t VerifyEntry(CMIPS&, INSTANCE&);
	void VerifyReturn(CMIPS&);
	void PatchStubs(CMIPS&);
	void Log(const char* format, ...);

	CPS2VM* m_vm = nullptr;
	SETTINGS m_settings;
	std::FILE* m_log = nullptr;
	std::vector<INSTANCE*> m_instances;

	// Verification of one call at a time (nested calls run the original unchecked)
	bool m_verifying = false;
	uint32_t m_verifyId = 0;
	uint32_t m_verifyCount = 0;     // COP0 count at the entry (a slice end changes it)
	std::vector<uint8_t> m_ram0;    // RAM before the call
	std::vector<uint8_t> m_spr0;
	std::vector<uint8_t> m_ram1;    // pages the original changed, by page
	std::vector<uint8_t> m_state0;  // MIPSSTATE before the call
	uint32_t m_mismatchLogs = 0;
};
