// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once
#if BOOST_OS_WINDOWS

// DirectInput 8 game controllers (older and non-Xbox pads on Windows).

#define DIRECTINPUT_VERSION 0x0800

#include <dinput.h>
#include <wrl/client.h>

#include "input/api/ControllerProvider.h"

#ifndef HAS_DIRECTINPUT
#define HAS_DIRECTINPUT 1
#endif

class DirectInputControllerProvider : public ControllerProviderBase
{
public:
	// throws when DirectInput can't be created
	DirectInputControllerProvider();
	~DirectInputControllerProvider() override;

	inline static InputAPI::Type kAPIType = InputAPI::DirectInput;
	InputAPI::Type api() const override { return kAPIType; }

	// every attached game controller class device
	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	IDirectInput8W* get_dinput() const { return m_dinput8.Get(); }
	// the standard joystick format (DIJOYSTATE)
	LPCDIDATAFORMAT get_data_format() const;

private:
	Microsoft::WRL::ComPtr<IDirectInput8W> m_dinput8;
};

#endif
