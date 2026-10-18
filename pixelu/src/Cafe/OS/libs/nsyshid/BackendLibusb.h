// Original code for RetroPixels (PixelU). Not derived from Cemu.
// Pass-through of real USB HID devices with libusb.
#pragma once
#include "nsyshid.h"

#if BOOST_OS_BSD
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif
#include "Backend.h"

#include <atomic>
#include <set>
#include <shared_mutex>
#include <thread>

namespace nsyshid::backend::libusb
{
	class BackendLibusb : public nsyshid::Backend
	{
	public:
		BackendLibusb();
		~BackendLibusb() override;

		bool IsInitialisedOk() override;

	protected:
		void AttachVisibleDevices() override;

	private:
		static int LIBUSB_CALL OnHotplugEvent(libusb_context* ctx, libusb_device* dev, libusb_hotplug_event event, void* user);
		void DeviceArrived(libusb_device* dev);
		void DeviceLeft(libusb_device* dev);
		std::shared_ptr<Device> CreateDevice(libusb_device* dev);

		libusb_context* m_ctx = nullptr;
		int m_initResult = 0;
		bool m_hotplugRegistered = false;
		libusb_hotplug_callback_handle m_hotplugHandle = 0;
		std::thread m_eventThread;
		std::atomic<bool> m_stopEvents{false};
	};

	class DeviceLibusb : public nsyshid::Device
	{
	public:
		DeviceLibusb(libusb_context* ctx, uint16 vendorId, uint16 productId, uint8 busNumber, uint8 deviceAddress,
					 std::vector<uint8> interfacesPerConfig, uint8 endpointIn, uint8 endpointOut);
		~DeviceLibusb() override;

		bool IsAt(uint16 vendorId, uint16 productId, uint8 busNumber, uint8 deviceAddress) const;

		bool Open() override;
		void Close() override;
		bool IsOpened() override;

		Status Read(uint8* buffer, uint32 length, sint32& bytesRead) override;
		Status Write(uint8* data, uint32 length, sint32& bytesWritten) override;

		bool GetDescriptor(uint8 descType, uint8 descIndex, uint16 lang, uint8* output, uint32 outputMaxLength) override;
		bool SetIdle(uint8 ifIndex, uint8 reportId, uint8 duration) override;
		bool SetProtocol(uint8 ifIndex, uint8 protocol) override;
		bool SetReport(uint8 reportType, uint8 reportId, uint8* data, uint32 length) override;

	private:
		bool ClaimInterfaces(size_t configIndex);
		void ReleaseInterfaces();
		bool BuildConfigDescriptor(uint8* output, uint32 outputMaxLength);

		libusb_context* m_ctx;
		uint8 m_bus;
		uint8 m_address;
		std::vector<uint8> m_interfacesPerConfig;
		uint8 m_endpointIn;
		uint8 m_endpointOut;

		// transfers hold the lock shared, Open/Close hold it exclusively
		std::shared_mutex m_handleLock;
		libusb_device_handle* m_handle = nullptr;
		std::atomic<bool> m_closing{false};
		std::set<int> m_claimed;
	};
} // namespace nsyshid::backend::libusb
