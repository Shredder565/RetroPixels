// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "BackendLibusb.h"
#include "util/helpers/helpers.h"

namespace nsyshid::backend::libusb
{
	// HID class requests (HID 1.11, section 7.2)
	constexpr uint8 HID_REQ_SET_REPORT = 0x09;
	constexpr uint8 HID_REQ_SET_IDLE = 0x0A;
	constexpr uint8 HID_REQ_SET_PROTOCOL = 0x0B;

	constexpr uint8 HID_CLASS_OUT = LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT;

	// polling interval of interrupt-in reads, so a closing device is noticed
	constexpr unsigned int READ_POLL_MS = 50;

	// ---- backend ----------------------------------------------------------------

	BackendLibusb::BackendLibusb()
	{
		m_initResult = libusb_init(&m_ctx);
		if (m_initResult < 0)
		{
			m_ctx = nullptr;
			cemuLog_log(LogType::Force, "nsyshid: libusb_init failed ({})", m_initResult);
			return;
		}
		if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG))
		{
			cemuLog_logDebug(LogType::Force, "nsyshid: libusb has no hotplug support");
			return;
		}
		int r = libusb_hotplug_register_callback(m_ctx,
												 (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
												 (libusb_hotplug_flag)0, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
												 LIBUSB_HOTPLUG_MATCH_ANY, &BackendLibusb::OnHotplugEvent, this, &m_hotplugHandle);
		if (r != LIBUSB_SUCCESS)
		{
			cemuLog_logDebug(LogType::Force, "nsyshid: hotplug registration failed ({})", r);
			return;
		}
		m_hotplugRegistered = true;
		m_eventThread = std::thread([this]() {
			SetThreadName("nsyshid-usb");
			while (!m_stopEvents)
			{
				timeval wait{};
				wait.tv_sec = 1;
				if (libusb_handle_events_timeout_completed(m_ctx, &wait, nullptr) != 0)
					std::this_thread::sleep_for(std::chrono::seconds(1));
			}
		});
	}

	BackendLibusb::~BackendLibusb()
	{
		if (m_hotplugRegistered)
		{
			m_stopEvents = true;
			libusb_hotplug_deregister_callback(m_ctx, m_hotplugHandle);
			m_eventThread.join();
		}
		DetachAllDevices();
		if (m_ctx)
			libusb_exit(m_ctx);
		m_ctx = nullptr;
	}

	bool BackendLibusb::IsInitialisedOk()
	{
		return m_initResult == 0;
	}

	void BackendLibusb::AttachVisibleDevices()
	{
		libusb_device** list = nullptr;
		if (libusb_get_device_list(m_ctx, &list) < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid: cannot list USB devices");
			return;
		}
		for (size_t i = 0; list[i]; i++)
			DeviceArrived(list[i]);
		libusb_free_device_list(list, 1);
	}

	int LIBUSB_CALL BackendLibusb::OnHotplugEvent(libusb_context* ctx, libusb_device* dev, libusb_hotplug_event event, void* user)
	{
		auto* self = static_cast<BackendLibusb*>(user);
		if (!self)
			return 0;
		if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
			self->DeviceArrived(dev);
		else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
			self->DeviceLeft(dev);
		return 0; // stay registered
	}

	void BackendLibusb::DeviceArrived(libusb_device* dev)
	{
		std::shared_ptr<Device> device = CreateDevice(dev);
		if (!device)
			return;
		if (!IsDeviceWhitelisted(device->VendorId(), device->ProductId()))
		{
			cemuLog_log(LogType::Force, "nsyshid: USB device {:04x}:{:04x} is not on the whitelist", device->VendorId(), device->ProductId());
			return;
		}
		if (!AttachDevice(device))
			cemuLog_log(LogType::Force, "nsyshid: could not attach USB device {:04x}:{:04x}", device->VendorId(), device->ProductId());
	}

	void BackendLibusb::DeviceLeft(libusb_device* dev)
	{
		libusb_device_descriptor desc{};
		if (libusb_get_device_descriptor(dev, &desc) < 0)
			return;
		const uint8 bus = libusb_get_bus_number(dev);
		const uint8 address = libusb_get_device_address(dev);
		auto device = FindDevice([&](const std::shared_ptr<Device>& d) {
			auto usb = std::dynamic_pointer_cast<DeviceLibusb>(d);
			return usb && usb->IsAt(desc.idVendor, desc.idProduct, bus, address);
		});
		if (device)
			DetachDevice(device);
	}

	std::shared_ptr<Device> BackendLibusb::CreateDevice(libusb_device* dev)
	{
		libusb_device_descriptor desc{};
		if (int r = libusb_get_device_descriptor(dev, &desc); r < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid: cannot read a USB device descriptor ({})", r);
			return nullptr;
		}

		// number of interfaces of every configuration (used to claim them)
		std::vector<uint8> interfaces;
		for (uint8 i = 0; i < desc.bNumConfigurations; i++)
		{
			libusb_config_descriptor* cfg = nullptr;
			int r = libusb_get_config_descriptor(dev, i, &cfg);
			if (r != LIBUSB_SUCCESS || !cfg)
			{
				cemuLog_log(LogType::Force, "nsyshid: config {} of {:04x}:{:04x} unreadable: {}", i, desc.idVendor, desc.idProduct, libusb_error_name(r));
				interfaces.push_back(0);
				continue;
			}
			interfaces.push_back(cfg->bNumInterfaces);
			libusb_free_config_descriptor(cfg);
		}

		// the first interrupt endpoint of each direction in the active configuration
		libusb_config_descriptor* active = nullptr;
		if (libusb_get_active_config_descriptor(dev, &active) != 0 || !active)
		{
			cemuLog_log(LogType::Force, "nsyshid: no active configuration on {:04x}:{:04x}", desc.idVendor, desc.idProduct);
			return nullptr;
		}
		std::optional<std::pair<uint8, uint16>> in, out;
		for (uint8 i = 0; i < active->bNumInterfaces; i++)
		{
			const libusb_interface& itf = active->interface[i];
			for (int a = 0; a < itf.num_altsetting; a++)
			{
				const libusb_interface_descriptor& alt = itf.altsetting[a];
				for (uint8 e = 0; e < alt.bNumEndpoints; e++)
				{
					const libusb_endpoint_descriptor& ep = alt.endpoint[e];
					auto& slot = (ep.bEndpointAddress & LIBUSB_ENDPOINT_IN) ? in : out;
					if (!slot)
						slot = std::make_pair(ep.bEndpointAddress, ep.wMaxPacketSize);
				}
			}
		}
		libusb_free_config_descriptor(active);

		auto device = std::make_shared<DeviceLibusb>(m_ctx, desc.idVendor, desc.idProduct, libusb_get_bus_number(dev),
													 libusb_get_device_address(dev), std::move(interfaces),
													 in ? in->first : 0, out ? out->first : 0);
		device->SetMaxPacketSizes(in ? in->second : 0, out ? out->second : 0);
		return device;
	}

	// ---- device -----------------------------------------------------------------

	DeviceLibusb::DeviceLibusb(libusb_context* ctx, uint16 vendorId, uint16 productId, uint8 busNumber, uint8 deviceAddress,
							   std::vector<uint8> interfacesPerConfig, uint8 endpointIn, uint8 endpointOut)
		: Device(vendorId, productId, 0, 2, 0),
		  m_ctx(ctx), m_bus(busNumber), m_address(deviceAddress), m_interfacesPerConfig(std::move(interfacesPerConfig)),
		  m_endpointIn(endpointIn), m_endpointOut(endpointOut)
	{
	}

	DeviceLibusb::~DeviceLibusb()
	{
		Close();
	}

	bool DeviceLibusb::IsAt(uint16 vendorId, uint16 productId, uint8 busNumber, uint8 deviceAddress) const
	{
		return VendorId() == vendorId && ProductId() == productId && m_bus == busNumber && m_address == deviceAddress;
	}

	bool DeviceLibusb::ClaimInterfaces(size_t configIndex)
	{
		if (configIndex >= m_interfacesPerConfig.size() || m_interfacesPerConfig[configIndex] == 0)
			return false;
		for (int i = 0; i < m_interfacesPerConfig[configIndex]; i++)
		{
			if (m_claimed.count(i))
				continue;
#if !defined(__APPLE__)
			// on macOS detaching needs privileges; the user is expected to run without a kernel driver bound
			if (libusb_kernel_driver_active(m_handle, i) == 1)
			{
				int r = libusb_detach_kernel_driver(m_handle, i);
				if (r < LIBUSB_SUCCESS && r != LIBUSB_ERROR_NOT_FOUND && r != LIBUSB_ERROR_NOT_SUPPORTED)
				{
					cemuLog_log(LogType::Force, "nsyshid: cannot detach kernel driver: {}", libusb_error_name(r));
					return false;
				}
			}
#endif
			int r = libusb_claim_interface(m_handle, i);
			if (r < LIBUSB_SUCCESS)
			{
				cemuLog_log(LogType::Force, "nsyshid: cannot claim interface {} of config {}: {}", i, configIndex, libusb_error_name(r));
				return false;
			}
			m_claimed.insert(i);
		}
		return true;
	}

	void DeviceLibusb::ReleaseInterfaces()
	{
		for (int i : m_claimed)
			libusb_release_interface(m_handle, i);
		m_claimed.clear();
	}

	bool DeviceLibusb::Open()
	{
		std::unique_lock _l(m_handleLock);
		if (m_handle)
			return true;

		libusb_device** list = nullptr;
		if (libusb_get_device_list(m_ctx, &list) < 0)
		{
			cemuLog_log(LogType::Force, "nsyshid: cannot list USB devices");
			return false;
		}
		libusb_device* match = nullptr;
		for (size_t i = 0; list[i] && !match; i++)
		{
			libusb_device_descriptor desc{};
			if (libusb_get_device_descriptor(list[i], &desc) < 0)
				continue;
			if (IsAt(desc.idVendor, desc.idProduct, libusb_get_bus_number(list[i]), libusb_get_device_address(list[i])))
				match = list[i];
		}
		bool ok = false;
		if (match)
		{
			int r = libusb_open(match, &m_handle);
			if (r < 0)
			{
				m_handle = nullptr;
				cemuLog_log(LogType::Force, "nsyshid: cannot open USB device: {}", libusb_strerror((libusb_error)r));
			}
			else if (!ClaimInterfaces(0))
			{
				ReleaseInterfaces();
				libusb_close(m_handle);
				m_handle = nullptr;
			}
			else
			{
				// interfaces of other configurations are claimed when possible
				for (size_t c = 1; c < m_interfacesPerConfig.size(); c++)
					ClaimInterfaces(c);
				ok = true;
			}
		}
		libusb_free_device_list(list, 1);
		return ok;
	}

	void DeviceLibusb::Close()
	{
		m_closing = true; // ends reads that are polling
		{
			std::unique_lock _l(m_handleLock);
			if (m_handle)
			{
				ReleaseInterfaces();
				libusb_close(m_handle);
				m_handle = nullptr;
			}
		}
		m_closing = false;
	}

	bool DeviceLibusb::IsOpened()
	{
		std::shared_lock _l(m_handleLock);
		return m_handle != nullptr;
	}

	Device::Status DeviceLibusb::Read(uint8* buffer, uint32 length, sint32& bytesRead)
	{
		std::shared_lock _l(m_handleLock);
		bytesRead = 0;
		if (!m_handle)
			return Status::Error;
		int got = 0;
		int r;
		do
		{
			r = libusb_interrupt_transfer(m_handle, m_endpointIn, buffer, (int)length, &got, READ_POLL_MS);
		} while (r == LIBUSB_ERROR_TIMEOUT && got == 0 && !m_closing);
		if (r == 0 || r == LIBUSB_ERROR_TIMEOUT)
		{
			bytesRead = got;
			return Status::Ok;
		}
		cemuLog_logDebug(LogType::Force, "nsyshid: read on endpoint 0x{:02x} failed: {}", m_endpointIn, libusb_error_name(r));
		return Status::Error;
	}

	Device::Status DeviceLibusb::Write(uint8* data, uint32 length, sint32& bytesWritten)
	{
		std::shared_lock _l(m_handleLock);
		bytesWritten = 0;
		if (!m_handle)
			return Status::Error;
		int sent = 0;
		int r = libusb_interrupt_transfer(m_handle, m_endpointOut, data, (int)length, &sent, 0);
		if (r != 0)
		{
			cemuLog_logDebug(LogType::Force, "nsyshid: write failed: {}", libusb_error_name(r));
			return Status::Error;
		}
		bytesWritten = sent;
		return Status::Ok;
	}

	// The configuration descriptor is reassembled from libusb's parsed form,
	// with multi-byte fields big-endian as the console delivers them. Of the
	// class-specific descriptors only HID descriptors are kept.
	bool DeviceLibusb::BuildConfigDescriptor(uint8* output, uint32 outputMaxLength)
	{
		libusb_config_descriptor* cfg = nullptr;
		if (libusb_get_active_config_descriptor(libusb_get_device(m_handle), &cfg) != 0 || !cfg)
			return false;

		std::vector<uint8> out;
		auto put = [&out](size_t bytes) -> uint8* {
			size_t at = out.size();
			out.resize(at + bytes, 0);
			return out.data() + at;
		};
		auto be16 = [](uint8* p, uint16 v) {
			p[0] = (uint8)(v >> 8);
			p[1] = (uint8)v;
		};

		{
			uint8* p = put(std::max<size_t>(cfg->bLength, 9));
			p[0] = cfg->bLength;
			p[1] = cfg->bDescriptorType;
			be16(p + 2, cfg->wTotalLength);
			p[4] = cfg->bNumInterfaces;
			p[5] = cfg->bConfigurationValue;
			p[6] = cfg->iConfiguration;
			p[7] = cfg->bmAttributes;
			p[8] = cfg->MaxPower;
		}
		for (uint8 i = 0; i < cfg->bNumInterfaces; i++)
		{
			const libusb_interface& itf = cfg->interface[i];
			for (int a = 0; a < itf.num_altsetting; a++)
			{
				const libusb_interface_descriptor& alt = itf.altsetting[a];
				uint8* p = put(std::max<size_t>(alt.bLength, 9));
				p[0] = alt.bLength;
				p[1] = alt.bDescriptorType;
				p[2] = alt.bInterfaceNumber;
				p[3] = alt.bAlternateSetting;
				p[4] = alt.bNumEndpoints;
				p[5] = alt.bInterfaceClass;
				p[6] = alt.bInterfaceSubClass;
				p[7] = alt.bInterfaceProtocol;
				p[8] = alt.iInterface;

				// class-specific descriptors following the interface descriptor
				for (int off = 0; off + 2 <= alt.extra_length;)
				{
					const uint8* src = alt.extra + off;
					const uint8 len = src[0];
					if (len == 0 || off + len > alt.extra_length)
						break;
					if (src[1] == LIBUSB_DT_HID && len == 9)
					{
						uint8* h = put(9);
						h[0] = src[0];
						h[1] = src[1];
						be16(h + 2, (uint16)(src[2] | (src[3] << 8))); // bcdHID
						h[4] = src[4];
						h[5] = src[5];
						h[6] = src[6];
						be16(h + 7, (uint16)(src[7] | (src[8] << 8))); // wDescriptorLength
					}
					off += len;
				}

				for (uint8 e = 0; e < alt.bNumEndpoints; e++)
				{
					const libusb_endpoint_descriptor& ep = alt.endpoint[e];
					uint8* q = put(std::max<size_t>(ep.bLength, 7));
					q[0] = ep.bLength;
					q[1] = ep.bDescriptorType;
					q[2] = ep.bEndpointAddress;
					q[3] = ep.bmAttributes;
					be16(q + 4, ep.wMaxPacketSize);
					q[6] = ep.bInterval;
					if (ep.bLength == LIBUSB_DT_ENDPOINT_AUDIO_SIZE)
					{
						q[7] = ep.bRefresh;
						q[8] = ep.bSynchAddress;
					}
				}
			}
		}
		libusb_free_config_descriptor(cfg);
		memcpy(output, out.data(), std::min<size_t>(outputMaxLength, out.size()));
		return true;
	}

	bool DeviceLibusb::GetDescriptor(uint8 descType, uint8 descIndex, uint16 lang, uint8* output, uint32 outputMaxLength)
	{
		std::shared_lock _l(m_handleLock);
		if (!m_handle)
			return false;
		if (descType == LIBUSB_DT_CONFIG)
			return BuildConfigDescriptor(output, outputMaxLength);
		const uint16 wValue = (uint16)((descType << 8) | descIndex);
		int r = libusb_control_transfer(m_handle, LIBUSB_REQUEST_TYPE_STANDARD | LIBUSB_ENDPOINT_IN, LIBUSB_REQUEST_GET_DESCRIPTOR,
										wValue, lang, output, (uint16)outputMaxLength, 0);
		if (r < 0 || (uint32)r != outputMaxLength)
		{
			cemuLog_logDebug(LogType::Force, "nsyshid: GET_DESCRIPTOR failed: {}", libusb_error_name(r));
			return false;
		}
		return true;
	}

	bool DeviceLibusb::SetIdle(uint8 ifIndex, uint8 reportId, uint8 duration)
	{
		std::shared_lock _l(m_handleLock);
		if (!m_handle)
			return false;
		int r = libusb_control_transfer(m_handle, HID_CLASS_OUT, HID_REQ_SET_IDLE, (uint16)((duration << 8) | reportId), ifIndex, nullptr, 0, 0);
		return r == 0;
	}

	bool DeviceLibusb::SetProtocol(uint8 ifIndex, uint8 protocol)
	{
		std::shared_lock _l(m_handleLock);
		if (!m_handle)
			return false;
		int r = libusb_control_transfer(m_handle, HID_CLASS_OUT, HID_REQ_SET_PROTOCOL, protocol, ifIndex, nullptr, 0, 0);
		return r == 0;
	}

	bool DeviceLibusb::SetReport(uint8 reportType, uint8 reportId, uint8* data, uint32 length)
	{
		std::shared_lock _l(m_handleLock);
		if (!m_handle)
			return false;
		int r = libusb_control_transfer(m_handle, HID_CLASS_OUT, HID_REQ_SET_REPORT, (uint16)((reportType << 8) | reportId),
										InterfaceIndex(), data, (uint16)(length & 0xFFFF), 0);
		if (r < 0 || (uint32)r != length)
		{
			cemuLog_logDebug(LogType::Force, "nsyshid: SET_REPORT failed: {}", libusb_error_name(r));
			return false;
		}
		return true;
	}
} // namespace nsyshid::backend::libusb
