// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Nintendo's GameCube controller adapter for Wii U (USB 057e:0337), read directly via
// libusb. Each adapter has 4 ports. Only built when HAS_GAMECUBE is set.

#include "input/api/ControllerProvider.h"

#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)

#include "util/libusbWrapper/libusbWrapper.h"

class GameCubeControllerProvider : public ControllerProviderBase
{
public:
	constexpr static size_t kMaxAdapters = 4;
	constexpr static size_t kMaxIndex = 4; // ports per adapter

	GameCubeControllerProvider();
	~GameCubeControllerProvider();

	inline static InputAPI::Type kAPIType = InputAPI::GameCube;
	InputAPI::Type api() const override { return kAPIType; }

	// all 4 ports of every adapter (the adapter can't tell which ones are in use)
	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	uint32 get_adapter_count() const;
	// the adapter's second USB plug (rumble power) is connected
	bool has_rumble_connected(uint32 adapter_index) const;
	bool is_connected(uint32 adapter_index) const;

	void set_rumble_state(uint32 adapter_index, uint32 index, bool state);

	// one port as the adapter last reported it
	struct GCState
	{
		bool valid = false;
		uint16 button = 0;

		uint8 lstick_x = 0;
		uint8 lstick_y = 0;

		uint8 rstick_x = 0;
		uint8 rstick_y = 0;

		uint8 lstick = 0; // analog L trigger
		uint8 rstick = 0; // analog R trigger
	};
	GCState get_state(uint32 adapter_index, uint32 index);

private:
	struct UsbAdapter
	{
		libusb_device_handle* handle = nullptr;
		uint8 endpoint_in = 0xFF;
		uint8 endpoint_out = 0xFF;
	};
	// opens the adapter_index-th adapter and starts its polling
	UsbAdapter open_adapter(uint32 adapter_index) const;
	void reader_thread();
	void writer_thread();
	static int hotplug_event(struct libusb_context* ctx, struct libusb_device* dev, libusb_hotplug_event event, void* user_data);

	std::shared_ptr<libusbWrapper> m_libusb;
	libusb_context* m_context = nullptr;
	libusb_hotplug_callback_handle m_callback_handle = 0;

	std::atomic_bool m_running = false;
	std::thread m_reader_thread, m_writer_thread;

	std::mutex m_writer_mutex;
	std::condition_variable m_writer_cond;
	bool m_rumble_changed = false;

	struct Adapter
	{
		std::atomic<libusb_device_handle*> m_device_handle{};
		uint8 m_endpoint_reader = 0xFF, m_endpoint_writer = 0xFF;

		mutable std::mutex m_state_mutex;
		std::array<GCState, kMaxIndex> m_states{};
		bool m_rumble_connected = false;

		std::array<bool, kMaxIndex> rumble_states{}; // guarded by m_writer_mutex
	};
	std::array<Adapter, kMaxAdapters> m_adapters;
};

#endif
