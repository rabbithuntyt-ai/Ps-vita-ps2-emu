#pragma once

// Text screens drawn by the CPU straight into a display framebuffer, for
// errors that happen before (or instead of) GPU initialization, e.g. when the
// runtime shader compiler that vitaGL needs is missing.

#include <string>

// Shows the message until X or O is pressed.
void CpuScreen_ShowMessage(const std::string& title, const std::string& body);
