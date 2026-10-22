// Original code for RetroPixels (PixelU). Not derived from Cemu.
//
// nsyshid exports. Games register "clients" (HIDAddClient) and get an attach
// callback for every device, then talk to a device through its handle.
// Transfers either complete synchronously (no callback given) or report their
// result later through the game's callback.
#include "Cafe/OS/common/OSCommon.h"
#include "Cafe/HW/Espresso/PPCCallback.h"
#include "Cafe/OS/libs/coreinit/coreinit.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "nsyshid.h"
#include "Backend.h"
#include "Whitelist.h"

#include <future>
#include <list>
#include <thread>

namespace nsyshid
{
	// guest structure registered by HIDAddClient
	struct HIDClient
	{
		uint32be attachCallback; // (client, device, attach) -> int
	};

	constexpr uint32 CALLBACK_DETACH = 0;
	constexpr uint32 CALLBACK_ATTACH = 1;
	constexpr sint32 RESULT_ERROR = -1;
	constexpr sint32 RESULT_TIMEOUT = -108;
	constexpr size_t MAX_DEVICES = 128;

	// ---- registry -------------------------------------------------------------

	std::recursive_mutex s_lock;
	std::list<std::shared_ptr<Backend>> s_backends;
	std::list<std::shared_ptr<Device>> s_devices;
	std::list<HIDClient*> s_clients;
	uint32 s_lastHandle = 1;

	SysAllocator<HIDDeviceRecord, MAX_DEVICES> s_recordPool;
	std::vector<bool> s_recordUsed(MAX_DEVICES, false);

	HIDDeviceRecord* AllocRecord()
	{
		for (size_t i = 0; i < MAX_DEVICES; i++)
		{
			if (!s_recordUsed[i])
			{
				s_recordUsed[i] = true;
				return s_recordPool.GetPtr() + i;
			}
		}
		return nullptr;
	}

	void FreeRecord(HIDDeviceRecord* record)
	{
		size_t i = (size_t)(record - s_recordPool.GetPtr());
		if (i < MAX_DEVICES)
			s_recordUsed[i] = false;
	}

	MPTR GuestAddr(const void* p)
	{
		return memory_getVirtualOffsetFromPointer(const_cast<void*>(p));
	}

	void NotifyClientAsync(HIDClient* client, const Device& device, uint32 kind)
	{
		coreinitAsyncCallback_add(client->attachCallback, 3, GuestAddr(client), GuestAddr(device.Record()), kind);
	}

	void NotifyClientNow(HIDClient* client, const Device& device, uint32 kind)
	{
		PPCCoreCallback((MPTR)client->attachCallback, GuestAddr(client), GuestAddr(device.Record()), kind);
	}

	bool PublishDevice(const std::shared_ptr<Device>& device)
	{
		std::unique_lock _l(s_lock);
		if (std::find(s_devices.begin(), s_devices.end(), device) != s_devices.end())
			return false;
		HIDDeviceRecord* record = AllocRecord();
		if (!record)
		{
			cemuLog_log(LogType::Force, "nsyshid: no free device slot for {:04x}:{:04x}", device->VendorId(), device->ProductId());
			return false;
		}
		record->handle = ++s_lastHandle;
		device->BindRecord(record);
		s_devices.push_back(device);
		for (HIDClient* client : s_clients)
			NotifyClientAsync(client, *device, CALLBACK_ATTACH);
		cemuLog_logDebug(LogType::Force, "nsyshid: attached {:04x}:{:04x}", device->VendorId(), device->ProductId());
		return true;
	}

	void UnpublishDevice(const std::shared_ptr<Device>& device)
	{
		{
			std::unique_lock _l(s_lock);
			auto it = std::find(s_devices.begin(), s_devices.end(), device);
			if (it == s_devices.end())
				return;
			s_devices.erase(it);
			for (HIDClient* client : s_clients)
				NotifyClientAsync(client, *device, CALLBACK_DETACH);
			FreeRecord(device->Record());
		}
		device->Close();
		cemuLog_logDebug(LogType::Force, "nsyshid: detached {:04x}:{:04x}", device->VendorId(), device->ProductId());
	}

	std::shared_ptr<Device> FindPublishedById(uint16 vendorId, uint16 productId)
	{
		std::unique_lock _l(s_lock);
		for (const auto& d : s_devices)
			if (d->VendorId() == vendorId && d->ProductId() == productId)
				return d;
		return nullptr;
	}

	// the device behind a handle, opened on first use
	std::shared_ptr<Device> DeviceForHandle(uint32 handle)
	{
		std::shared_ptr<Device> device;
		{
			std::unique_lock _l(s_lock);
			for (const auto& d : s_devices)
			{
				if (d->Handle() == handle)
				{
					device = d;
					break;
				}
			}
		}
		if (device && !device->IsOpened() && !device->Open())
			return nullptr;
		return device;
	}

	void AttachBackend(const std::shared_ptr<Backend>& backend)
	{
		{
			std::unique_lock _l(s_lock);
			s_backends.push_back(backend);
		}
		backend->OnAttach();
	}

	void DetachBackend(const std::shared_ptr<Backend>& backend)
	{
		{
			std::unique_lock _l(s_lock);
			s_backends.remove(backend);
		}
		backend->OnDetach();
	}

	// ---- Device / Backend -------------------------------------------------------

	Device::Device(uint16 vendorId, uint16 productId, uint8 interfaceIndex, uint8 subClass, uint8 protocol)
		: m_vendorId(vendorId), m_productId(productId), m_interfaceIndex(interfaceIndex), m_subClass(subClass), m_protocol(protocol)
	{
	}

	void Device::BindRecord(HIDDeviceRecord* record)
	{
		m_record = record;
		if (!record)
			return;
		record->physicalId = 0x11223344;
		record->vendorId = m_vendorId;
		record->productId = m_productId;
		record->interfaceIndex = m_interfaceIndex;
		record->subClass = m_subClass;
		record->protocol = m_protocol;
		record->reserved = 0;
		record->maxPacketSizeRx = m_maxPacketRx;
		record->maxPacketSizeTx = m_maxPacketTx;
	}

	void Backend::OnAttach()
	{
		std::unique_lock _l(m_lock);
		m_attached = true;
		AttachVisibleDevices();
	}

	void Backend::OnDetach()
	{
		std::unique_lock _l(m_lock);
		DetachAllDevices();
		m_attached = false;
	}

	bool Backend::IsBackendAttached()
	{
		std::unique_lock _l(m_lock);
		return m_attached;
	}

	void Backend::DetachAllDevices()
	{
		std::unique_lock _l(m_lock);
		if (!m_attached)
			return;
		for (const auto& d : m_devices)
			UnpublishDevice(d);
		m_devices.clear();
	}

	bool Backend::AttachDevice(const std::shared_ptr<Device>& device)
	{
		std::unique_lock _l(m_lock);
		if (!m_attached || !PublishDevice(device))
			return false;
		m_devices.push_back(device);
		return true;
	}

	void Backend::DetachDevice(const std::shared_ptr<Device>& device)
	{
		std::unique_lock _l(m_lock);
		if (!m_attached)
			return;
		UnpublishDevice(device);
		m_devices.erase(std::remove(m_devices.begin(), m_devices.end(), device), m_devices.end());
	}

	std::shared_ptr<Device> Backend::FindDevice(const std::function<bool(const std::shared_ptr<Device>&)>& match)
	{
		std::unique_lock _l(m_lock);
		for (const auto& d : m_devices)
			if (match(d))
				return d;
		return nullptr;
	}

	std::shared_ptr<Device> Backend::FindDeviceById(uint16 vendorId, uint16 productId)
	{
		return FindPublishedById(vendorId, productId);
	}

	bool Backend::IsDeviceWhitelisted(uint16 vendorId, uint16 productId)
	{
		return Whitelist::GetInstance().IsDeviceWhitelisted(vendorId, productId);
	}

	// ---- transfer helpers -------------------------------------------------------

	// run a potentially slow device operation on a host thread while the
	// calling guest thread sleeps on an event
	sint32 WaitForHostWork(const std::function<sint32()>& work)
	{
		StackAllocator<coreinit::OSEvent> done;
		coreinit::OSInitEvent(done.GetPointer(), coreinit::OSEvent::EVENT_STATE::STATE_NOT_SIGNALED, coreinit::OSEvent::EVENT_MODE::MODE_AUTO);
		coreinit::OSEvent* event = done.GetPointer();
		std::future<sint32> result = std::async(std::launch::async, [&work, event]() {
			sint32 r = work();
			coreinit::OSSignalEvent(event);
			return r;
		});
		coreinit::OSWaitEvent(event);
		return result.get();
	}

	// report a finished asynchronous transfer: callback(handle, error, buffer, length, context)
	void ReportTransfer(MPTR callback, MPTR context, uint32 handle, sint32 error, MPTR buffer, sint32 length)
	{
		coreinitAsyncCallback_add(callback, 5, handle, (uint32)error, buffer, (uint32)length, context);
	}

	// run a control request either now or on a host thread with a completion callback
	sint32 RunControl(const std::shared_ptr<Device>& device, MPTR callback, MPTR context, sint32 successValue, std::function<bool()> request)
	{
		if (!callback)
			return request() ? successValue : RESULT_ERROR;
		const uint32 handle = device->Handle();
		std::thread([device, callback, context, handle, request]() {
			bool ok = request();
			ReportTransfer(callback, context, handle, ok ? 0 : RESULT_ERROR, 0, 0);
		}).detach();
		return 0;
	}

	sint32 DoRead(Device& device, uint8* data, uint32 length)
	{
		if (!device.IsOpened())
			return RESULT_ERROR;
		memset(data, 0, length);
		sint32 got = 0;
		switch (device.Read(data, length, got))
		{
		case Device::Status::Ok: return got;
		case Device::Status::Timeout: return RESULT_TIMEOUT;
		default: return RESULT_ERROR;
		}
	}

	sint32 DoWrite(Device& device, uint8* data, uint32 length)
	{
		if (!device.IsOpened())
			return RESULT_ERROR;
		sint32 sent = 0;
		switch (device.Write(data, length, sent))
		{
		case Device::Status::Ok: return sent;
		case Device::Status::Timeout: return RESULT_TIMEOUT;
		default: return RESULT_ERROR;
		}
	}

	// interrupt transfers: blocking on a host thread, or reported through the callback
	sint32 RunTransfer(const std::shared_ptr<Device>& device, uint8* data, uint32 length, MPTR callback, MPTR context,
					   sint32 (*op)(Device&, uint8*, uint32))
	{
		if (!callback)
			return WaitForHostWork([&]() { return op(*device, data, length); });
		const uint32 handle = device->Handle();
		std::thread([device, data, length, callback, context, handle, op]() {
			sint32 r = op(*device, data, length);
			ReportTransfer(callback, context, handle, r < 0 ? r : 0, GuestAddr(data), r > 0 ? r : 0);
		}).detach();
		return 0;
	}

	std::shared_ptr<Device> LookupOrLog(uint32 handle, const char* fn)
	{
		auto device = DeviceForHandle(handle);
		if (!device)
			cemuLog_log(LogType::Force, "nsyshid.{}: no device with handle {}", fn, handle);
		return device;
	}

	// ---- exports ------------------------------------------------------------------

	sint32 HIDAddClient(HIDClient* client, MPTR callback)
	{
		if (!client)
			return RESULT_ERROR;
		client->attachCallback = callback;
		std::unique_lock _l(s_lock);
		s_clients.push_front(client);
		for (const auto& d : s_devices)
			NotifyClientNow(client, *d, CALLBACK_ATTACH);
		return 0;
	}

	sint32 HIDDelClient(HIDClient* client)
	{
		std::unique_lock _l(s_lock);
		s_clients.remove(client);
		if (client)
		{
			for (const auto& d : s_devices)
				NotifyClientNow(client, *d, CALLBACK_DETACH);
		}
		return 0;
	}

	sint32 HIDGetDescriptor(uint32 handle, uint8 descType, uint8 descIndex, uint16 lang, uint8* output, uint32 maxLength, MPTR callback, MPTR context)
	{
		auto device = LookupOrLog(handle, "HIDGetDescriptor");
		if (!device || !output)
			return RESULT_ERROR;
		return RunControl(device, callback, context, (sint32)maxLength, [device, descType, descIndex, lang, output, maxLength]() {
			return device->GetDescriptor(descType, descIndex, lang, output, maxLength);
		});
	}

	sint32 HIDSetIdle(uint32 handle, uint8 ifIndex, uint8 reportId, uint8 duration, MPTR callback, MPTR context)
	{
		auto device = LookupOrLog(handle, "HIDSetIdle");
		if (!device)
			return RESULT_ERROR;
		return RunControl(device, callback, context, 0, [device, ifIndex, reportId, duration]() {
			return device->SetIdle(ifIndex, reportId, duration);
		});
	}

	sint32 HIDSetProtocol(uint32 handle, uint8 ifIndex, uint8 protocol, MPTR callback, MPTR context)
	{
		auto device = LookupOrLog(handle, "HIDSetProtocol");
		if (!device)
			return RESULT_ERROR;
		return RunControl(device, callback, context, 0, [device, ifIndex, protocol]() {
			return device->SetProtocol(ifIndex, protocol);
		});
	}

	sint32 HIDSetReport(uint32 handle, uint8 reportType, uint8 reportId, uint8* data, uint32 length, MPTR callback, MPTR context)
	{
		auto device = LookupOrLog(handle, "HIDSetReport");
		if (!device)
			return RESULT_ERROR;
		if (!callback)
		{
			// blocking form: the number of bytes sent, 0 on failure
			return WaitForHostWork([&]() -> sint32 {
				return device->SetReport(reportType, reportId, data, length) ? (sint32)length : 0;
			});
		}
		const uint32 deviceHandle = device->Handle();
		std::thread([device, reportType, reportId, data, length, callback, context, deviceHandle]() {
			bool ok = device->SetReport(reportType, reportId, data, length);
			ReportTransfer(callback, context, deviceHandle, ok ? 0 : RESULT_ERROR, GuestAddr(data), (sint32)length);
		}).detach();
		return 0;
	}

	sint32 HIDRead(uint32 handle, uint8* data, uint32 maxLength, MPTR callback, MPTR context)
	{
		auto device = LookupOrLog(handle, "HIDRead");
		if (!device || !data)
			return RESULT_ERROR;
		return RunTransfer(device, data, maxLength, callback, context, &DoRead);
	}

	sint32 HIDWrite(uint32 handle, uint8* data, uint32 length, MPTR callback, MPTR context)
	{
		auto device = LookupOrLog(handle, "HIDWrite");
		if (!device || !data)
			return RESULT_ERROR;
		return RunTransfer(device, data, length, callback, context, &DoWrite);
	}

	sint32 HIDDecodeError(uint32 errorCode, uint32be* outA, uint32be* outB)
	{
		// not decoded; these are the values games accept as "generic error"
		if (outA)
			*outA = 0x3FF;
		if (outB)
			*outB = (uint32)-0x7FFF;
		return 0;
	}

	class NsyshidModule : public COSModule
	{
	public:
		std::string_view GetName() override
		{
			return "nsyshid";
		}

		void RPLMapped() override
		{
			cafeExportRegisterFunc(HIDAddClient, "nsyshid", "HIDAddClient", LogType::Placeholder);
			cafeExportRegisterFunc(HIDDelClient, "nsyshid", "HIDDelClient", LogType::Placeholder);
			cafeExportRegisterFunc(HIDGetDescriptor, "nsyshid", "HIDGetDescriptor", LogType::Placeholder);
			cafeExportRegisterFunc(HIDSetIdle, "nsyshid", "HIDSetIdle", LogType::Placeholder);
			cafeExportRegisterFunc(HIDSetProtocol, "nsyshid", "HIDSetProtocol", LogType::Placeholder);
			cafeExportRegisterFunc(HIDSetReport, "nsyshid", "HIDSetReport", LogType::Placeholder);
			cafeExportRegisterFunc(HIDRead, "nsyshid", "HIDRead", LogType::Placeholder);
			cafeExportRegisterFunc(HIDWrite, "nsyshid", "HIDWrite", LogType::Placeholder);
			cafeExportRegisterFunc(HIDDecodeError, "nsyshid", "HIDDecodeError", LogType::Placeholder);
		}

		void rpl_entry(uint32 moduleHandle, coreinit::RplEntryReason reason) override
		{
			if (reason == coreinit::RplEntryReason::Loaded)
			{
				Whitelist::GetInstance();
				backend::AttachDefaultBackends();
			}
		}
	};

	NsyshidModule s_module;

	COSModule* GetModule()
	{
		return &s_module;
	}
} // namespace nsyshid
