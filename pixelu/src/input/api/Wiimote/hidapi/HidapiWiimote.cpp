// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/Wiimote/hidapi/HidapiWiimote.h"

#include <SDL3/SDL.h>
#include <cwchar>

namespace
{
	constexpr uint16 kNintendoVendor = 0x057e;
	constexpr uint16 kRemoteProduct = 0x0306;            // RVL-CNT-01
	constexpr uint16 kRemotePlusProduct = 0x0330;        // RVL-CNT-01-TR (built in Motion Plus)
	constexpr size_t kLongestInputReport = 22;           // report id + 21 bytes
	constexpr const wchar_t* kWiiUProName = L"Nintendo RVL-CNT-01-UC"; // shares the product id, not a remote

	bool IsRemote(const SDL_hid_device_info& info)
	{
		if (info.product_id != kRemoteProduct && info.product_id != kRemotePlusProduct)
			return false;
		return !(info.product_string && std::wcscmp(info.product_string, kWiiUProName) == 0);
	}
}

HidapiWiimote::HidapiWiimote(SDL_hid_device* dev, std::string_view path)
	: m_handle(dev), m_path(path)
{
}

HidapiWiimote::~HidapiWiimote()
{
	SDL_hid_close(m_handle);
}

bool HidapiWiimote::write_data(const std::vector<uint8>& data)
{
	return SDL_hid_write(m_handle, data.data(), data.size()) >= 0;
}

std::optional<std::vector<uint8>> HidapiWiimote::read_data()
{
	uint8 report[kLongestInputReport]{};
	const int length = SDL_hid_read(m_handle, report, sizeof(report));
	if (length < 0)
		return std::nullopt; // gone
	return std::vector<uint8>(report, report + length); // empty: nothing waiting
}

bool HidapiWiimote::operator==(const WiimoteDevice& o) const
{
	const auto* other = dynamic_cast<const HidapiWiimote*>(&o);
	return other && other->m_path == m_path;
}

std::vector<WiimoteDevicePtr> HidapiWiimote::get_devices()
{
	std::vector<WiimoteDevicePtr> found;
	SDL_hid_init();

	SDL_hid_device_info* list = SDL_hid_enumerate(kNintendoVendor, 0);
	for (SDL_hid_device_info* info = list; info; info = info->next)
	{
		if (!IsRemote(*info))
			continue;

		SDL_hid_device* handle = SDL_hid_open_path(info->path);
		if (!handle)
		{
			cemuLog_logDebug(LogType::Force, "Unable to open Wiimote device at {}: {}", info->path, SDL_GetError());
			continue;
		}
		SDL_hid_set_nonblocking(handle, true); // the reader thread polls
		found.emplace_back(std::make_shared<HidapiWiimote>(handle, info->path));
	}
	SDL_hid_free_enumeration(list);
	return found;
}
