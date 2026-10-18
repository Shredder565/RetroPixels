// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "BackendEmulated.h"

#include "Dimensions.h"
#include "Infinity.h"
#include "Skylander.h"
#include "SkylanderXbox360.h"
#include "config/CemuConfig.h"

namespace nsyshid
{
	bool EmulatedHidDevice::GetDescriptor(uint8 descType, uint8 descIndex, uint16 lang, uint8* output, uint32 outputMaxLength)
	{
		static constexpr uint8 kDescriptor[] = {
			// configuration: 1 interface, bus powered, 500 mA
			9, 0x02, 0x00, 0x29, 1, 1, 0, 0x80, 0xFA,
			// interface 0: HID, two endpoints
			9, 0x04, 0, 0, 2, 0x03, 0, 0, 0,
			// HID 1.11, one report descriptor of 0x1D bytes
			9, 0x21, 0x01, 0x11, 0x00, 1, 0x22, 0x00, 0x1D,
			// endpoint 0x81: interrupt in, 64 bytes, every frame
			7, 0x05, 0x81, 0x03, 0x00, 0x40, 1,
			// endpoint 0x02: interrupt out, 64 bytes, every frame
			7, 0x05, 0x02, 0x03, 0x00, 0x40, 1,
		};
		static_assert(sizeof(kDescriptor) == 0x29);
		memcpy(output, kDescriptor, std::min<uint32>(outputMaxLength, sizeof(kDescriptor)));
		return true;
	}
} // namespace nsyshid

namespace nsyshid::backend::emulated
{
	BackendEmulated::BackendEmulated() = default;
	BackendEmulated::~BackendEmulated() = default;

	bool BackendEmulated::IsInitialisedOk()
	{
		return true;
	}

	void BackendEmulated::AttachVisibleDevices()
	{
		const auto& usb = GetConfig().emulated_usb_devices;

		// an emulated Skylanders portal stands in unless a real one is connected;
		// otherwise an Xbox 360 portal, if present, is presented as a Wii U one
		if (usb.emulate_skylander_portal && !FindDeviceById(0x1430, 0x0150))
			AttachDevice(std::make_shared<SkylanderPortalDevice>());
#ifdef HAS_LIBUSB
		else if (auto xboxPortal = std::dynamic_pointer_cast<backend::libusb::DeviceLibusb>(FindDeviceById(0x1430, 0x1F17)))
			AttachDevice(std::make_shared<SkylanderXbox360PortalLibusb>(xboxPortal));
#endif

		if (usb.emulate_infinity_base && !FindDeviceById(0x0E6F, 0x0129))
			AttachDevice(std::make_shared<InfinityBaseDevice>());

		if (usb.emulate_dimensions_toypad && !FindDeviceById(0x0E6F, 0x0241))
			AttachDevice(std::make_shared<DimensionsToypadDevice>());
	}
} // namespace nsyshid::backend::emulated
