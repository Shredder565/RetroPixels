// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

#include "input/api/Wiimote/WiimoteDevice.h"
#include <bluetooth/bluetooth.h>

// A Wii remote over raw Bluetooth L2CAP sockets (Linux / BlueZ). Remotes are not
// discovered here: the pairing dialog hands us their addresses and we connect to the
// ones that are not in use.
class L2CapWiimote : public WiimoteDevice
{
public:
	L2CapWiimote(int controlFd, int dataFd, bdaddr_t addr);
	~L2CapWiimote() override;

	bool write_data(const std::vector<uint8>& data) override;
	std::optional<std::vector<uint8>> read_data() override;
	bool operator==(const WiimoteDevice& o) const override;

	static void AddCandidateAddress(bdaddr_t addr);
	static std::vector<WiimoteDevicePtr> get_devices();

private:
	int m_controlFd; // HID control channel, only kept open
	int m_dataFd;    // HID interrupt channel, carries the reports
	bdaddr_t m_addr;
};
