// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "input/api/Wiimote/WiimoteDevice.h"

// A Wii remote reached through SDL's HID functions (Windows, macOS, Linux hidraw).
class HidapiWiimote : public WiimoteDevice
{
public:
	HidapiWiimote(struct SDL_hid_device* dev, std::string_view path);
	~HidapiWiimote() override;

	bool write_data(const std::vector<uint8>& data) override;
	std::optional<std::vector<uint8>> read_data() override;
	bool operator==(const WiimoteDevice& o) const override;

	// opens every remote that is plugged in (paired) right now
	static std::vector<WiimoteDevicePtr> get_devices();

private:
	struct SDL_hid_device* m_handle;
	const std::string m_path; // identifies the remote
};
