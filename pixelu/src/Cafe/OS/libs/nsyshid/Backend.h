// Original code for RetroPixels (PixelU). Not derived from Cemu.
// nsyshid device model: a Device is one USB HID interface the game can talk
// to; a Backend is a source of devices (real USB devices through libusb,
// emulated toy portals) that attaches and detaches them at runtime.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "Common/precompiled.h"

namespace nsyshid
{
	// the device record handed to the game. Its layout must stay as it is;
	// vendor and product id are stored in host order, as games expect here.
	struct HIDDeviceRecord
	{
		/* +0x00 */ uint32be handle;
		/* +0x04 */ uint32 physicalId;
		/* +0x08 */ uint16 vendorId;
		/* +0x0A */ uint16 productId;
		/* +0x0C */ uint8 interfaceIndex;
		/* +0x0D */ uint8 subClass;
		/* +0x0E */ uint8 protocol;
		/* +0x0F */ uint8 reserved;
		/* +0x10 */ uint16be maxPacketSizeRx;
		/* +0x12 */ uint16be maxPacketSizeTx;
	};
	static_assert(sizeof(HIDDeviceRecord) == 0x14);
	static_assert(offsetof(HIDDeviceRecord, vendorId) == 0x08);
	static_assert(offsetof(HIDDeviceRecord, interfaceIndex) == 0x0C);
	static_assert(offsetof(HIDDeviceRecord, maxPacketSizeRx) == 0x10);

	class Device
	{
	public:
		enum class Status
		{
			Ok,
			Error,
			Timeout,
		};

		Device(uint16 vendorId, uint16 productId, uint8 interfaceIndex, uint8 subClass, uint8 protocol);
		Device(const Device&) = delete;
		Device& operator=(const Device&) = delete;
		virtual ~Device() = default;

		uint16 VendorId() const { return m_vendorId; }
		uint16 ProductId() const { return m_productId; }
		uint8 InterfaceIndex() const { return m_interfaceIndex; }

		void SetMaxPacketSizes(uint16 rx, uint16 tx)
		{
			m_maxPacketRx = rx;
			m_maxPacketTx = tx;
		}

		// the guest record of an attached device (nullptr while detached)
		HIDDeviceRecord* Record() const { return m_record; }
		uint32 Handle() const { return m_record ? (uint32)m_record->handle : 0; }
		void BindRecord(HIDDeviceRecord* record);

		virtual bool Open() = 0;
		virtual void Close() = 0;
		virtual bool IsOpened() = 0;

		// interrupt transfers
		virtual Status Read(uint8* buffer, uint32 length, sint32& bytesRead) = 0;
		virtual Status Write(uint8* data, uint32 length, sint32& bytesWritten) = 0;

		// control requests
		virtual bool GetDescriptor(uint8 descType, uint8 descIndex, uint16 lang, uint8* output, uint32 outputMaxLength) = 0;
		virtual bool SetIdle(uint8 ifIndex, uint8 reportId, uint8 duration) = 0;
		virtual bool SetProtocol(uint8 ifIndex, uint8 protocol) = 0;
		virtual bool SetReport(uint8 reportType, uint8 reportId, uint8* data, uint32 length) = 0;

	private:
		uint16 m_vendorId;
		uint16 m_productId;
		uint8 m_interfaceIndex;
		uint8 m_subClass;
		uint8 m_protocol;
		uint16 m_maxPacketRx = 0x20;
		uint16 m_maxPacketTx = 0x20;
		HIDDeviceRecord* m_record = nullptr;
	};

	class Backend
	{
	public:
		Backend() = default;
		Backend(const Backend&) = delete;
		Backend& operator=(const Backend&) = delete;
		virtual ~Backend() = default;

		virtual bool IsInitialisedOk() = 0;

		// called by nsyshid when the backend is registered / unregistered
		void OnAttach();
		void OnDetach();
		bool IsBackendAttached();

		void DetachAllDevices();

	protected:
		// publish a device to the game; fails while the backend is not attached
		bool AttachDevice(const std::shared_ptr<Device>& device);
		void DetachDevice(const std::shared_ptr<Device>& device);

		// search the devices of this backend
		std::shared_ptr<Device> FindDevice(const std::function<bool(const std::shared_ptr<Device>&)>& match);
		// search the devices of all backends
		std::shared_ptr<Device> FindDeviceById(uint16 vendorId, uint16 productId);

		bool IsDeviceWhitelisted(uint16 vendorId, uint16 productId);

		// called from OnAttach(): attach the devices this backend can see
		virtual void AttachVisibleDevices() = 0;

	private:
		std::recursive_mutex m_lock;
		std::vector<std::shared_ptr<Device>> m_devices;
		bool m_attached = false;
	};

	namespace backend
	{
		void AttachDefaultBackends();
	}
} // namespace nsyshid
