// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#if BOOST_OS_WINDOWS

// XInput (Xbox controllers on Windows), loaded at run time from whichever XInput DLL
// the system has.

#include "input/api/ControllerProvider.h"

#include <Xinput.h>

#ifndef HAS_XINPUT
#define HAS_XINPUT 1
#endif

class XInputControllerProvider : public ControllerProviderBase
{
	friend class XInputController;
public:
	// throws when no usable XInput DLL is found
	XInputControllerProvider();
	~XInputControllerProvider() override;

	inline static InputAPI::Type kAPIType = InputAPI::XInput;
	InputAPI::Type api() const override { return kAPIType; }

	// one controller per connected user index
	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

private:
	HMODULE m_module = nullptr;
	// the DLL's functions; battery information is missing from XInput9_1_0
	decltype(&XInputGetBatteryInformation) m_XInputGetBatteryInformation = nullptr;
	decltype(&XInputGetCapabilities) m_XInputGetCapabilities = nullptr;
	decltype(&XInputSetState) m_XInputSetState = nullptr;
	decltype(&XInputGetState) m_XInputGetState = nullptr;
};

#endif
