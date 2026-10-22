// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/DirectInput/DirectInputControllerProvider.h"

#include "input/api/DirectInput/DirectInputController.h"

DirectInputControllerProvider::DirectInputControllerProvider()
{
	const HRESULT hr = DirectInput8Create(GetModuleHandle(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W,
		reinterpret_cast<void**>(m_dinput8.GetAddressOf()), nullptr);
	if (FAILED(hr))
		throw std::runtime_error(fmt::format("can't create direct input object (error: {:#x})", GetLastError()));
}

DirectInputControllerProvider::~DirectInputControllerProvider() = default;

std::vector<std::shared_ptr<ControllerBase>> DirectInputControllerProvider::get_controllers()
{
	std::vector<std::shared_ptr<ControllerBase>> found;
	const auto add_device = [](LPCDIDEVICEINSTANCEW device, LPVOID context) -> BOOL
	{
		auto& list = *static_cast<std::vector<std::shared_ptr<ControllerBase>>*>(context);
		list.emplace_back(std::make_shared<DirectInputController>(device->guidInstance, boost::nowide::narrow(device->tszProductName)));
		return DIENUM_CONTINUE;
	};
	m_dinput8->EnumDevices(DI8DEVCLASS_GAMECTRL, add_device, &found, DIEDFL_ALLDEVICES);
	return found;
}

LPCDIDATAFORMAT DirectInputControllerProvider::get_data_format() const
{
	return GetdfDIJoystick();
}
