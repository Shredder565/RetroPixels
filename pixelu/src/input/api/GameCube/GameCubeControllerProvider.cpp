// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/GameCube/GameCubeControllerProvider.h"
#include "input/api/GameCube/GameCubeController.h"
#include "util/libusbWrapper/libusbWrapper.h"

#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)

#ifdef interface
#undef interface // windows.h macro, clashes with libusb's field name
#endif

namespace
{
	constexpr uint16_t kAdapterVendor = 0x057e;
	constexpr uint16_t kAdapterProduct = 0x0337;

	constexpr uint8 kCommandStartPolling = 0x13;
	constexpr uint8 kCommandRumble = 0x11;
	constexpr unsigned kTransferTimeoutMs = 25;

	// input report: 1 byte id, then 9 bytes per port:
	// status, buttons (2), stick x/y, c-stick x/y, L, R
	constexpr size_t kReportSize = 37;
	constexpr size_t kPortStride = 9;
	constexpr uint8 kStatusRumblePower = 0x04;
}

GameCubeControllerProvider::GameCubeControllerProvider()
{
	m_libusb = libusbWrapper::getInstance();
	m_libusb->init();
	if (!m_libusb->isAvailable())
		throw std::runtime_error("libusbWrapper not available");
	m_libusb->p_libusb_init(&m_context);

	for (uint32 i = 0; i < kMaxAdapters; ++i)
	{
		const UsbAdapter usb = open_adapter(i);
		if (!usb.handle)
			continue;
		m_adapters[i].m_device_handle = usb.handle;
		m_adapters[i].m_endpoint_reader = usb.endpoint_in;
		m_adapters[i].m_endpoint_writer = usb.endpoint_out;
	}

	if (m_libusb->p_libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG))
	{
		const auto events = static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT);
		m_libusb->p_libusb_hotplug_register_callback(m_context, events, LIBUSB_HOTPLUG_NO_FLAGS, kAdapterVendor, kAdapterProduct,
			LIBUSB_HOTPLUG_MATCH_ANY, &GameCubeControllerProvider::hotplug_event, this, &m_callback_handle);
	}

	m_running = true;
	m_reader_thread = std::thread(&GameCubeControllerProvider::reader_thread, this);
	m_writer_thread = std::thread(&GameCubeControllerProvider::writer_thread, this);
}

GameCubeControllerProvider::~GameCubeControllerProvider()
{
	if (m_running.exchange(false))
	{
		m_writer_thread.join();
		m_reader_thread.join();
	}
	if (m_callback_handle)
	{
		m_libusb->p_libusb_hotplug_deregister_callback(m_context, m_callback_handle);
		m_callback_handle = 0;
	}
	for (auto& adapter : m_adapters)
		m_libusb->p_libusb_close(adapter.m_device_handle);
	if (m_context)
	{
		m_libusb->p_libusb_exit(m_context);
		m_context = nullptr;
	}
}

std::vector<ControllerPtr> GameCubeControllerProvider::get_controllers()
{
	std::vector<ControllerPtr> result;
	const uint32 adapters = std::min<uint32>(get_adapter_count(), kMaxAdapters);
	for (uint32 adapter = 0; adapter < adapters; ++adapter)
	{
		for (uint32 port = 0; port < kMaxIndex; ++port)
			result.emplace_back(std::make_shared<GameCubeController>(adapter, port));
	}
	return result;
}

uint32 GameCubeControllerProvider::get_adapter_count() const
{
	libusb_device** devices;
	const auto count = m_libusb->p_libusb_get_device_list(nullptr, &devices);
	if (count < 0)
	{
		cemuLog_log(LogType::Force, "libusb error {} at libusb_get_device_list: {}", static_cast<int>(count), m_libusb->p_libusb_error_name(static_cast<int>(count)));
		return 0;
	}

	uint32 adapters = 0;
	for (ssize_t i = 0; i < count; ++i)
	{
		if (!devices[i])
			continue;
		libusb_device_descriptor desc;
		const int ret = m_libusb->p_libusb_get_device_descriptor(devices[i], &desc);
		if (ret != 0)
		{
			cemuLog_log(LogType::Force, "libusb error {} at libusb_get_device_descriptor: {}", ret, m_libusb->p_libusb_error_name(ret));
			continue;
		}
		if (desc.idVendor == kAdapterVendor && desc.idProduct == kAdapterProduct)
			++adapters;
	}
	m_libusb->p_libusb_free_device_list(devices, 1);
	return adapters;
}

bool GameCubeControllerProvider::has_rumble_connected(uint32 adapter_index) const
{
	if (adapter_index >= m_adapters.size())
		return false;
	std::scoped_lock lock(m_adapters[adapter_index].m_state_mutex);
	return m_adapters[adapter_index].m_rumble_connected;
}

bool GameCubeControllerProvider::is_connected(uint32 adapter_index) const
{
	return adapter_index < m_adapters.size() && m_adapters[adapter_index].m_device_handle != nullptr;
}

void GameCubeControllerProvider::set_rumble_state(uint32 adapter_index, uint32 index, bool state)
{
	if (adapter_index >= m_adapters.size() || index >= kMaxIndex)
		return;
	std::scoped_lock lock(m_writer_mutex);
	m_adapters[adapter_index].rumble_states[index] = state;
	m_rumble_changed = true;
	m_writer_cond.notify_all();
}

GameCubeControllerProvider::GCState GameCubeControllerProvider::get_state(uint32 adapter_index, uint32 index)
{
	if (adapter_index >= m_adapters.size() || index >= kMaxIndex)
		return {};
	std::scoped_lock lock(m_adapters[adapter_index].m_state_mutex);
	return m_adapters[adapter_index].m_states[index];
}

GameCubeControllerProvider::UsbAdapter GameCubeControllerProvider::open_adapter(uint32 adapter_index) const
{
	UsbAdapter result;

	libusb_device** devices;
	const auto count = m_libusb->p_libusb_get_device_list(nullptr, &devices);
	if (count < 0)
	{
		cemuLog_log(LogType::Force, "libusb error {} at libusb_get_device_list: {}", static_cast<int>(count), m_libusb->p_libusb_error_name(static_cast<int>(count)));
		return result;
	}

	const auto log_error = [this](int ret, const char* what)
	{
		cemuLog_log(LogType::Force, "libusb error {} at {}: {}", ret, what, m_libusb->p_libusb_error_name(ret));
	};

	uint32 seen = 0;
	for (ssize_t i = 0; i < count; ++i)
	{
		if (!devices[i])
			continue;
		libusb_device_descriptor desc;
		int ret = m_libusb->p_libusb_get_device_descriptor(devices[i], &desc);
		if (ret != 0)
		{
			log_error(ret, "libusb_get_device_descriptor");
			continue;
		}
		if (desc.idVendor != kAdapterVendor || desc.idProduct != kAdapterProduct)
			continue;
		if (seen++ != adapter_index)
			continue;

		libusb_device_handle* handle;
		ret = m_libusb->p_libusb_open(devices[i], &handle);
		if (ret != 0)
		{
			log_error(ret, "libusb_open");
			continue;
		}
		if (m_libusb->p_libusb_kernel_driver_active(handle, 0) == 1)
		{
			ret = m_libusb->p_libusb_detach_kernel_driver(handle, 0);
			if (ret != 0)
			{
				log_error(ret, "libusb_detach_kernel_driver");
				m_libusb->p_libusb_close(handle);
				continue;
			}
		}
		ret = m_libusb->p_libusb_claim_interface(handle, 0);
		if (ret != 0)
		{
			log_error(ret, "libusb_claim_interface");
			m_libusb->p_libusb_close(handle);
			continue;
		}

		// the last IN and the last OUT endpoint of the configuration
		libusb_config_descriptor* config = nullptr;
		m_libusb->p_libusb_get_config_descriptor(devices[i], 0, &config);
		for (uint8 n = 0; n < config->bNumInterfaces; ++n)
		{
			const libusb_interface& usb_interface = config->interface[n];
			for (int alt = 0; alt < usb_interface.num_altsetting; ++alt)
			{
				const libusb_interface_descriptor& setting = usb_interface.altsetting[alt];
				for (uint8 e = 0; e < setting.bNumEndpoints; ++e)
				{
					const uint8 address = setting.endpoint[e].bEndpointAddress;
					if (address & LIBUSB_ENDPOINT_IN)
						result.endpoint_in = address;
					else
						result.endpoint_out = address;
				}
			}
		}
		m_libusb->p_libusb_free_config_descriptor(config);

		uint8_t command = kCommandStartPolling;
		int transferred = 0;
		m_libusb->p_libusb_interrupt_transfer(handle, result.endpoint_out, &command, sizeof(command), &transferred, kTransferTimeoutMs);

		result.handle = handle;
		break;
	}

	m_libusb->p_libusb_free_device_list(devices, 1);
	return result;
}

void GameCubeControllerProvider::reader_thread()
{
	SetThreadName("GCControllerAdapter::reader_thread");
	while (m_running.load(std::memory_order_relaxed))
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::this_thread::yield();

		for (auto& adapter : m_adapters)
		{
			if (!adapter.m_device_handle)
				continue;

			std::array<uint8_t, kReportSize> report{};
			int received;
			const int ret = m_libusb->p_libusb_interrupt_transfer(adapter.m_device_handle, adapter.m_endpoint_reader, report.data(), (int)report.size(), &received, kTransferTimeoutMs);
			if (ret != 0)
			{
				cemuLog_log(LogType::Force, "libusb error {} at libusb_interrupt_transfer: {}", ret, m_libusb->p_libusb_error_name(ret));
				if (ret == LIBUSB_ERROR_NO_DEVICE || ret == LIBUSB_ERROR_IO)
				{
					if (auto* gone = adapter.m_device_handle.exchange(nullptr))
						m_libusb->p_libusb_close(gone);
				}
				continue;
			}

			for (size_t port = 0; port < kMaxIndex; ++port)
			{
				const uint8* p = report.data() + 1 + port * kPortStride;
				GCState state;
				state.valid = true;
				state.button = (uint16)(p[1] | (p[2] << 8));
				state.lstick_x = p[3];
				state.lstick_y = p[4];
				state.rstick_x = p[5];
				state.rstick_y = p[6];
				state.lstick = p[7];
				state.rstick = p[8];

				std::scoped_lock lock(adapter.m_state_mutex);
				adapter.m_rumble_connected = (report[1] & kStatusRumblePower) != 0;
				adapter.m_states[port] = state;
			}
		}
	}
}

void GameCubeControllerProvider::writer_thread()
{
	SetThreadName("GCControllerAdapter::writer_thread");

	std::array<std::array<bool, kMaxIndex>, kMaxAdapters> sent{}; // what each adapter last got
	while (m_running.load(std::memory_order_relaxed))
	{
		std::unique_lock lock(m_writer_mutex);
		if (!m_rumble_changed && m_writer_cond.wait_for(lock, std::chrono::milliseconds(250)) == std::cv_status::timeout)
			continue;

		bool wrote = false;
		for (size_t i = 0; i < kMaxAdapters; ++i)
		{
			Adapter& adapter = m_adapters[i];
			if (!adapter.m_device_handle || adapter.rumble_states == sent[i])
				continue;
			sent[i] = adapter.rumble_states;
			m_rumble_changed = false;

			lock.unlock();
			std::array<uint8, 5> command{ kCommandRumble, sent[i][0], sent[i][1], sent[i][2], sent[i][3] };
			int written;
			const int ret = m_libusb->p_libusb_interrupt_transfer(adapter.m_device_handle, adapter.m_endpoint_writer, command.data(), (int)command.size(), &written, kTransferTimeoutMs);
			if (ret != 0)
				cemuLog_log(LogType::Force, "libusb error {} at libusb_interrupt_transfer: {}", ret, m_libusb->p_libusb_error_name(ret));
			wrote = true;
			lock.lock();
		}

		if (wrote)
		{
			lock.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}
}

int GameCubeControllerProvider::hotplug_event(libusb_context* ctx, libusb_device* dev, libusb_hotplug_event event, void* user_data)
{
	if (event != LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
		return 0; // removal shows up as a failing transfer in the reader

	auto* self = static_cast<GameCubeControllerProvider*>(user_data);
	for (uint32 i = 0; i < kMaxAdapters; ++i)
	{
		Adapter& adapter = self->m_adapters[i];
		if (adapter.m_device_handle)
			continue;
		const UsbAdapter usb = self->open_adapter(i);
		if (!usb.handle)
			continue;
		adapter.m_endpoint_reader = usb.endpoint_in;
		adapter.m_endpoint_writer = usb.endpoint_out;
		adapter.m_device_handle = usb.handle;
	}
	return 0;
}

#endif
