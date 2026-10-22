// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// A transport to one Wii remote (HID via SDL, or a raw Bluetooth L2CAP channel).
class WiimoteDevice
{
public:
	virtual ~WiimoteDevice() = default;

	// sends one output report (report id first); false when the remote is gone
	virtual bool write_data(const std::vector<uint8>& data) = 0;
	// one input report; an empty vector when none is waiting, nullopt when the remote is gone
	virtual std::optional<std::vector<uint8_t>> read_data() = 0;

	// the same physical remote
	virtual bool operator==(const WiimoteDevice& o) const = 0;
};

using WiimoteDevicePtr = std::shared_ptr<WiimoteDevice>;
