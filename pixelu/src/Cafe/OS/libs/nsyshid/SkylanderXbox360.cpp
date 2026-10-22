// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "SkylanderXbox360.h"

namespace nsyshid
{
#ifdef HAS_LIBUSB
	// size of one report of the Xbox 360 portal, header included
	constexpr size_t XBOX_REPORT_SIZE = 32;

	SkylanderXbox360PortalLibusb::SkylanderXbox360PortalLibusb(std::shared_ptr<backend::libusb::DeviceLibusb> usbPortal)
		: EmulatedHidDevice(0x1430, 0x0150), m_portal(std::move(usbPortal))
	{
		g72x_init_state(&m_encoder);
	}

	bool SkylanderXbox360PortalLibusb::Open()
	{
		return m_portal->Open();
	}

	void SkylanderXbox360PortalLibusb::Close()
	{
		m_portal->Close();
	}

	bool SkylanderXbox360PortalLibusb::IsOpened()
	{
		return m_portal->IsOpened();
	}

	Device::Status SkylanderXbox360PortalLibusb::Read(uint8* buffer, uint32 length, sint32& bytesRead)
	{
		std::array<uint8, XBOX_REPORT_SIZE> report{};
		const size_t want = std::min<size_t>(XBOX_REPORT_SIZE, length + sizeof(XBOX_DATA_HEADER));
		sint32 got = 0;
		Status s = m_portal->Read(report.data(), (uint32)want, got);
		// strip the header; the remainder of the guest buffer stays zero
		const size_t payload = want > sizeof(XBOX_DATA_HEADER) ? want - sizeof(XBOX_DATA_HEADER) : 0;
		memcpy(buffer, report.data() + sizeof(XBOX_DATA_HEADER), std::min<size_t>(payload, length));
		bytesRead = got;
		return s;
	}

	// commands go out on the interrupt endpoint rather than as SET_REPORT
	bool SkylanderXbox360PortalLibusb::SetReport(uint8 reportType, uint8 reportId, uint8* data, uint32 length)
	{
		if (length >= 2 && data[0] == 'M' && data[1] == 0x01) // speaker on
			g72x_init_state(&m_encoder);

		std::vector<uint8> report(sizeof(XBOX_DATA_HEADER) + length);
		memcpy(report.data(), XBOX_DATA_HEADER, sizeof(XBOX_DATA_HEADER));
		memcpy(report.data() + sizeof(XBOX_DATA_HEADER), data, length);
		sint32 sent = 0;
		return m_portal->Write(report.data(), (uint32)report.size(), sent) == Status::Ok;
	}

	// speaker audio: 16-bit little-endian mono PCM in, two 4-bit G.721 codes per output byte
	Device::Status SkylanderXbox360PortalLibusb::Write(uint8* data, uint32 length, sint32& bytesWritten)
	{
		const size_t pairs = length / 4;
		std::vector<uint8> report(sizeof(XBOX_AUDIO_DATA_HEADER) + pairs);
		memcpy(report.data(), XBOX_AUDIO_DATA_HEADER, sizeof(XBOX_AUDIO_DATA_HEADER));
		for (size_t i = 0; i < pairs; i++)
		{
			const uint8* s = data + i * 4;
			const sint16 first = (sint16)(s[0] | (s[1] << 8));
			const sint16 second = (sint16)(s[2] | (s[3] << 8));
			const uint8 lo = (uint8)(g721_encoder(first, &m_encoder) & 0x0F);
			const uint8 hi = (uint8)(g721_encoder(second, &m_encoder) & 0x0F);
			report[sizeof(XBOX_AUDIO_DATA_HEADER) + i] = (uint8)((hi << 4) | lo);
		}
		sint32 sent = 0;
		Status s = m_portal->Write(report.data(), (uint32)report.size(), sent);
		bytesWritten = sent - (sint32)sizeof(XBOX_AUDIO_DATA_HEADER);
		return s;
	}
#endif
} // namespace nsyshid
