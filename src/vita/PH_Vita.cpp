#include "PH_Vita.h"
#include <psp2/ctrl.h>
#include <psp2/touch.h>

using namespace PS2;

namespace
{
	uint8 ApplyDeadzone(uint8 value)
	{
		int centered = static_cast<int>(value) - 0x80;
		if(centered > -16 && centered < 16) return 0x7F;
		return value;
	}
}

CPH_Vita::CPH_Vita()
{
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
}

CPadHandler::FactoryFunction CPH_Vita::GetFactoryFunction(CPH_Vita** instance)
{
	return [instance]() {
		auto handler = new CPH_Vita();
		if(instance) *instance = handler;
		return handler;
	};
}

uint32 CPH_Vita::Poll()
{
	SceCtrlData pad = {};
	// Ext2 maps the Vita's shoulder buttons to L1/R1 and exposes the extra
	// buttons of external controllers on PS TV.
	sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

	bool rearL2 = false, rearR2 = false, frontL3 = false, frontR3 = false;
	SceTouchData touch = {};
	if(sceTouchPeek(SCE_TOUCH_PORT_BACK, &touch, 1) >= 0)
	{
		for(uint32 i = 0; i < touch.reportNum; i++)
		{
			if(touch.report[i].x < 960) rearL2 = true;
			else rearR2 = true;
		}
	}
	if(sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) >= 0)
	{
		for(uint32 i = 0; i < touch.reportNum; i++)
		{
			if(touch.report[i].y < 800) continue;
			if(touch.report[i].x < 400) frontL3 = true;
			if(touch.report[i].x > 1520) frontR3 = true;
		}
	}

	uint32 b = pad.buttons;
	std::lock_guard<std::mutex> lock(m_mutex);
	m_buttons[CControllerInfo::DPAD_UP] = b & SCE_CTRL_UP;
	m_buttons[CControllerInfo::DPAD_DOWN] = b & SCE_CTRL_DOWN;
	m_buttons[CControllerInfo::DPAD_LEFT] = b & SCE_CTRL_LEFT;
	m_buttons[CControllerInfo::DPAD_RIGHT] = b & SCE_CTRL_RIGHT;
	m_buttons[CControllerInfo::SELECT] = b & SCE_CTRL_SELECT;
	m_buttons[CControllerInfo::START] = b & SCE_CTRL_START;
	m_buttons[CControllerInfo::SQUARE] = b & SCE_CTRL_SQUARE;
	m_buttons[CControllerInfo::TRIANGLE] = b & SCE_CTRL_TRIANGLE;
	m_buttons[CControllerInfo::CIRCLE] = b & SCE_CTRL_CIRCLE;
	m_buttons[CControllerInfo::CROSS] = b & SCE_CTRL_CROSS;
	m_buttons[CControllerInfo::L1] = b & SCE_CTRL_L1;
	m_buttons[CControllerInfo::R1] = b & SCE_CTRL_R1;
	m_buttons[CControllerInfo::L2] = (b & SCE_CTRL_L2) || rearL2;
	m_buttons[CControllerInfo::R2] = (b & SCE_CTRL_R2) || rearR2;
	m_buttons[CControllerInfo::L3] = (b & SCE_CTRL_L3) || frontL3;
	m_buttons[CControllerInfo::R3] = (b & SCE_CTRL_R3) || frontR3;
	m_axes[0] = ApplyDeadzone(pad.lx);
	m_axes[1] = ApplyDeadzone(pad.ly);
	m_axes[2] = ApplyDeadzone(pad.rx);
	m_axes[3] = ApplyDeadzone(pad.ry);
	return b;
}

void CPH_Vita::Update(uint8* ram)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	for(auto* listener : m_interfaces)
	{
		for(unsigned int i = 0; i < CControllerInfo::MAX_BUTTONS; i++)
		{
			auto button = static_cast<CControllerInfo::BUTTON>(i);
			if(CControllerInfo::IsAxis(button))
			{
				listener->SetAxisState(0, button, m_axes[i], ram);
			}
			else
			{
				listener->SetButtonState(0, button, m_buttons[i], ram);
			}
		}
	}
}
