#pragma once

#include <array>
#include <mutex>
#include "PadHandler.h"
#include "ControllerInfo.h"

// DualShock 2 emulation from the Vita's controls.
//
//  Vita            DualShock 2
//  ----            -----------
//  L / R           L1 / R1
//  rear touch      L2 (left half) / R2 (right half)
//  front touch     L3 (bottom-left corner) / R3 (bottom-right corner)
//  sticks          left / right analog sticks
//
// On PS TV with a DualShock 3/4 the real L2/R2/L3/R3 are used as well.
class CPH_Vita : public CPadHandler
{
public:
	CPH_Vita();
	~CPH_Vita() override = default;

	// Polls the hardware. Called from the UI thread once per presented frame.
	// Returns the raw Vita button mask so the frontend can detect hotkeys.
	uint32 Poll();

	// Called by the VM at every vblank.
	void Update(uint8*) override;

	static FactoryFunction GetFactoryFunction(CPH_Vita** instance);

private:
	std::mutex m_mutex;
	std::array<bool, PS2::CControllerInfo::MAX_BUTTONS> m_buttons = {};
	std::array<uint8, 4> m_axes = {0x7F, 0x7F, 0x7F, 0x7F};
};
