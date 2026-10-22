// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// Real Wii remotes, talked to directly (no Wii in between). A connection thread looks
// for remotes every 2 seconds, a reader thread decodes their input reports into a
// WiimoteState per remote and a writer thread sends our output reports, spaced out by
// a per remote packet delay because the remotes drop reports that come too fast.

#include "input/motion/MotionHandler.h"
#include "input/api/Wiimote/WiimoteDevice.h"
#include "input/api/Wiimote/WiimoteMessages.h"

#include "input/api/ControllerProvider.h"
#include "input/api/ControllerState.h"

#include <list>
#include <variant>
#include <boost/ptr_container/ptr_vector.hpp>

#ifndef HAS_WIIMOTE
#define HAS_WIIMOTE 1
#endif

#define WIIMOTE_DEBUG 1

class WiimoteControllerProvider : public ControllerProviderBase
{
	friend class WiimoteController;
public:
	constexpr static uint32 kDefaultPacketDelay = 25; // ms between two reports to one remote

	WiimoteControllerProvider();
	~WiimoteControllerProvider();

	inline static InputAPI::Type kAPIType = InputAPI::Wiimote;
	InputAPI::Type api() const override { return kAPIType; }

	// registers newly found remotes and returns one controller per known slot
	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;

	bool is_connected(size_t index);
	// the slot exists (the remote may have gone)
	bool is_registered_device(size_t index);
	void set_rumble(size_t index, bool state);
	void request_status(size_t index);
	// lights the LED(s) of a player number
	void set_led(size_t index, size_t player_index);

	uint32 get_packet_delay(size_t index);
	void set_packet_delay(size_t index, uint32 delay);

	struct WiimoteState
	{
		uint16 buttons = 0;
		uint8 flags = 0; // StatusBitmask
		uint8 battery_level = 0;

		glm::vec3 m_acceleration{}, m_prev_acceleration{}; // raw minus zero point
		float m_roll = 0; // from gravity, used to straighten the IR dots

		std::chrono::high_resolution_clock::time_point m_last_motion_timestamp{};
		MotionSample motion_sample{};
		WiiUMotionHandler motion_handler{};

		bool m_calibrated = false;
		Calibration m_calib_acceleration{};

		struct IRCamera
		{
			IRMode mode = kIRDisabled;
			std::array<IRDot, 4> dots{}, prev_dots{};

			// pointer: middle of the tracked pair of dots
			glm::vec2 position{}, m_prev_position{};
			PositionVisibility m_positionVisibility;
			glm::vec2 middle{};
			float distance = 0;
			std::pair<sint32, sint32> indices{ 0, 1 };
		} ir_camera{};

		std::optional<MotionPlusData> m_motion_plus;
		std::variant<std::monostate, NunchuckData, ClassicData> m_extension{};
	};
	WiimoteState get_state(size_t index);

private:
	struct Wiimote
	{
		Wiimote(WiimoteDevicePtr device)
			: device(std::move(device)) {}

		WiimoteDevicePtr device; // null once the remote is gone; the slot stays
		std::atomic_bool rumble = false;

		std::shared_mutex mutex;
		WiimoteState state{};

		std::atomic_uint32_t data_delay = kDefaultPacketDelay;
		std::chrono::high_resolution_clock::time_point data_ts{}; // last report sent
	};

	void reader_thread();
	void writer_thread();
	void connectionThread();

	// decodes one input report into `state`; false drops the report
	bool handle_report(size_t index, const std::vector<uint8>& report, WiimoteState& state, bool& update_report);
	bool handle_read_reply(size_t index, const uint8* data, WiimoteState& state);
	static void update_motion_sample(WiimoteState& state);

	void calibrate(size_t index);
	IRMode set_ir_camera(size_t index, bool state);

	void send_packet(size_t index, std::vector<uint8> data);
	void send_read_packet(size_t index, MemoryType type, RegisterAddress address, uint16 size);
	void send_write_packet(size_t index, MemoryType type, RegisterAddress address, const std::vector<uint8>& data);

	void parse_acceleration(WiimoteState& wiimote_state, const uint8*& data);

	void rotate_ir(WiimoteState& wiimote_state);
	void calculate_ir_position(WiimoteState& wiimote_state);
	int parse_ir(WiimoteState& wiimote_state, const uint8* data);

	void request_extension(size_t index);
	void detect_motion_plus(size_t index);
	void set_motion_plus(size_t index, bool state);

	void update_report_type(size_t index);

	std::atomic_bool m_running = false;
	std::thread m_reader_thread, m_writer_thread;
	std::shared_mutex m_device_mutex; // guards m_wiimotes

	std::thread m_connectionThread;
	std::mutex m_connectionMutex;
	std::condition_variable m_connectionCond;
	std::vector<WiimoteDevicePtr> m_connectedDevices; // what the last search found
	std::mutex m_connectedDeviceMutex;

	boost::ptr_vector<Wiimote> m_wiimotes;

	std::list<std::pair<size_t, std::vector<uint8>>> m_write_queue;
	std::mutex m_writer_mutex;
	std::condition_variable m_writer_cond;
};
