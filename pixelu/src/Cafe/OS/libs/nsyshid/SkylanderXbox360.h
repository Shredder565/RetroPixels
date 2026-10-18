// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Presents a Skylanders portal made for the Xbox 360 (connected through
// libusb) to the game as a Wii U portal. The Xbox portal frames every report
// with a two-byte header and takes speaker audio as G.721 ADPCM.
#pragma once

#include "nsyshid.h"
#include "BackendEmulated.h"
#ifdef HAS_LIBUSB
#include "BackendLibusb.h"
#include "g721/g721.h"
#endif

namespace nsyshid
{
	constexpr uint8 XBOX_DATA_HEADER[] = {0x0B, 0x14};
	constexpr uint8 XBOX_AUDIO_DATA_HEADER[] = {0x0B, 0x17};

#ifdef HAS_LIBUSB
	class SkylanderXbox360PortalLibusb final : public EmulatedHidDevice
	{
	public:
		explicit SkylanderXbox360PortalLibusb(std::shared_ptr<backend::libusb::DeviceLibusb> usbPortal);

		bool Open() override;
		void Close() override;
		bool IsOpened() override;

		Status Read(uint8* buffer, uint32 length, sint32& bytesRead) override;
		Status Write(uint8* data, uint32 length, sint32& bytesWritten) override;
		bool SetReport(uint8 reportType, uint8 reportId, uint8* data, uint32 length) override;

	private:
		std::shared_ptr<backend::libusb::DeviceLibusb> m_portal;
		g72x_state m_encoder{};
	};
#endif
} // namespace nsyshid
