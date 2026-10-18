// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Backend for the toy portals emulated in software (Skylanders portal,
// Disney Infinity base, LEGO Dimensions toy pad).
#pragma once
#include "nsyshid.h"
#include "Backend.h"

namespace nsyshid
{
	// common behaviour of an emulated toy portal: a single HID interface with
	// one interrupt endpoint in each direction, always available
	class EmulatedHidDevice : public Device
	{
	public:
		EmulatedHidDevice(uint16 vendorId, uint16 productId)
			: Device(vendorId, productId, 1, 2, 0)
		{
		}

		bool Open() override
		{
			m_open = true;
			return true;
		}
		void Close() override { m_open = false; }
		bool IsOpened() override { return m_open; }

		// configuration descriptor of a toy portal (the Wii U reads it big-endian)
		bool GetDescriptor(uint8 descType, uint8 descIndex, uint16 lang, uint8* output, uint32 outputMaxLength) override;

		bool SetIdle(uint8 ifIndex, uint8 reportId, uint8 duration) override { return true; }
		bool SetProtocol(uint8 ifIndex, uint8 protocol) override { return true; }
		bool SetReport(uint8 reportType, uint8 reportId, uint8* data, uint32 length) override { return true; }

	private:
		bool m_open = false;
	};
} // namespace nsyshid

namespace nsyshid::backend::emulated
{
	class BackendEmulated : public nsyshid::Backend
	{
	public:
		BackendEmulated();
		~BackendEmulated() override;

		bool IsInitialisedOk() override;

	protected:
		void AttachVisibleDevices() override;
	};
} // namespace nsyshid::backend::emulated
