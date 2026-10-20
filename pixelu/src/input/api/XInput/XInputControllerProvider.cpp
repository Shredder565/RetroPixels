// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include <Windows.h>

#include "input/api/XInput/XInputControllerProvider.h"
#include "input/api/XInput/XInputController.h"

namespace
{
	template<typename T>
	void LoadFunction(HMODULE module, const char* name, T& function)
	{
		function = reinterpret_cast<T>(GetProcAddress(module, name));
	}
}

XInputControllerProvider::XInputControllerProvider()
{
	// newest first
	for (const char* dll : { "XInput1_4.DLL", "XInput1_3.DLL", "XInput9_1_0.dll" })
	{
		m_module = LoadLibraryA(dll);
		if (m_module)
			break;
	}
	if (!m_module)
		throw std::runtime_error("can't load any xinput dll");

	LoadFunction(m_module, "XInputGetCapabilities", m_XInputGetCapabilities);
	LoadFunction(m_module, "XInputGetState", m_XInputGetState);
	LoadFunction(m_module, "XInputSetState", m_XInputSetState);
	LoadFunction(m_module, "XInputGetBatteryInformation", m_XInputGetBatteryInformation); // optional

	if (!m_XInputGetCapabilities || !m_XInputGetState || !m_XInputSetState)
	{
		FreeLibrary(m_module);
		m_module = nullptr;
		throw std::runtime_error("can't find necessary xinput functions");
	}
}

XInputControllerProvider::~XInputControllerProvider()
{
	if (m_module)
		FreeLibrary(m_module);
}

std::vector<std::shared_ptr<ControllerBase>> XInputControllerProvider::get_controllers()
{
	std::vector<std::shared_ptr<ControllerBase>> result;
	for (DWORD user = 0; user < XUSER_MAX_COUNT; ++user)
	{
		XINPUT_CAPABILITIES caps{};
		if (m_XInputGetCapabilities(user, XINPUT_FLAG_GAMEPAD, &caps) == ERROR_SUCCESS)
			result.emplace_back(std::make_shared<XInputController>(user));
	}
	return result;
}
