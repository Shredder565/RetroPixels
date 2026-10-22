// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/api/Wiimote/WiimoteControllerProvider.h"
#include "input/api/Wiimote/NativeWiimoteController.h"
#include "input/api/Wiimote/WiimoteMessages.h"

#ifdef HAS_HIDAPI
#include "input/api/Wiimote/hidapi/HidapiWiimote.h"
#endif
#ifdef HAS_BLUEZ
#include "input/api/Wiimote/l2cap/L2CapWiimote.h"
#endif

#include <numbers>

namespace
{
	// bits of the button bytes that carry accelerometer LSBs instead of buttons
	constexpr uint16 kNonButtonBits = 0x60E0;

	uint16 ReadButtons(const uint8* p)
	{
		return (uint16)((p[0] | (p[1] << 8)) & ~kNonButtonBits);
	}

	uint16 ReadBE16(const uint8* p)
	{
		return (uint16)((p[0] << 8) | p[1]);
	}

	uint64 ReadBE48(const uint8* p)
	{
		uint64 v = 0;
		for (int i = 0; i < 6; ++i)
			v = (v << 8) | p[i];
		return v;
	}

	// 10 bit values: 8 high bits in their own bytes, the 2 low bits packed as --XXYYZZ
	void ReadCalibration(const uint8* p, Calibration& calibration)
	{
		calibration.zero.x = (uint16)((p[0] << 2) | ((p[3] >> 4) & 3));
		calibration.zero.y = (uint16)((p[1] << 2) | ((p[3] >> 2) & 3));
		calibration.zero.z = (uint16)((p[2] << 2) | (p[3] & 3));
		calibration.gravity.x = (uint16)((p[4] << 2) | ((p[7] >> 4) & 3));
		calibration.gravity.y = (uint16)((p[5] << 2) | ((p[7] >> 2) & 3));
		calibration.gravity.z = (uint16)((p[6] << 2) | (p[7] & 3));
	}

	// acceleration in g, with the axes in the order the GamePad uses, plus how much it changed
	MotionSample AccelerometerSample(const glm::vec3& acceleration, const glm::vec3& previous, const Calibration& calibration)
	{
		float acc[3]{ -acceleration.x, -acceleration.z, acceleration.y };
		glm::vec3 now = acceleration;
		glm::vec3 before = previous;
		const glm::vec<3, uint16> one_g = calibration.gravity - calibration.zero;
		if (one_g != glm::vec<3, uint16>{})
		{
			for (int i = 0; i < 3; ++i)
			{
				const float g = (float)one_g[i];
				acc[i] /= g;
				now[i] /= g;
				before[i] /= g;
			}
		}
		const float none3[3]{};
		const float none4[4]{};
		return MotionSample(acc, glm::length(now - before), none3, none3, none4);
	}

	void ParseNunchuk(const uint8* p, NunchuckData& nunchuk)
	{
		nunchuk.raw_axis.x = p[0];
		nunchuk.raw_axis.y = p[1];

		glm::vec<3, uint16> raw;
		raw.x = (uint16)((p[2] << 2) | ((p[5] >> 2) & 3));
		raw.y = (uint16)((p[3] << 2) | ((p[5] >> 4) & 3));
		raw.z = (uint16)((p[4] << 2) | ((p[5] >> 6) & 3));
		nunchuk.z = (p[5] & 1) == 0;
		nunchuk.c = (p[5] & 2) == 0;

		// each half of the stick range scaled on its own
		const auto& cal = nunchuk.calibration;
		const auto& stick = nunchuk.raw_axis;
		if (stick.x < cal.center.x)
			nunchuk.axis.x = ((float)stick.x - cal.min.x) / ((float)cal.center.x - cal.min.x + 0.012f) - 1.0f;
		else
			nunchuk.axis.x = (float)(stick.x - cal.center.x) / (cal.max.x - cal.center.x + 0.012f);
		if (stick.y <= cal.center.y)
			nunchuk.axis.y = ((float)stick.y - cal.min.y) / ((float)cal.center.y - cal.min.y + 0.012f) - 1.0f;
		else
			nunchuk.axis.y = (float)(stick.y - cal.center.y) / (cal.max.y - cal.center.y);

		nunchuk.prev_acceleration = nunchuk.acceleration;
		nunchuk.acceleration = glm::vec3(raw) - glm::vec3(cal.zero);
		nunchuk.motion_sample = AccelerometerSample(nunchuk.acceleration, nunchuk.prev_acceleration, cal);
	}

	void ParseClassic(const uint8* p, ClassicData& classic)
	{
		// the right stick and the triggers are spread over several bytes
		classic.left_raw_axis.x = p[0] & 0x3F;
		classic.left_raw_axis.y = p[1] & 0x3F;
		classic.right_raw_axis.x = (uint8)(((p[0] & 0xC0) >> 3) | ((p[1] & 0xC0) >> 5) | ((p[2] & 0x80) >> 7));
		classic.right_raw_axis.y = p[2] & 0x1F;
		classic.raw_trigger.x = (uint8)(((p[2] & 0x60) >> 2) | ((p[3] & 0xE0) >> 5));
		classic.raw_trigger.y = p[3] & 0x1F;
		classic.buttons = (uint16)~(p[4] | (p[5] << 8)); // active low

		classic.left_axis = glm::vec2(classic.left_raw_axis) / 63.0f * 2.0f - 1.0f;
		classic.right_axis = glm::vec2(classic.right_raw_axis) / 31.0f * 2.0f - 1.0f;
		classic.trigger = glm::vec2(classic.raw_trigger) / 31.0f;
	}

	void ParseNunchukCalibration(const uint8* p, NunchuckData& nunchuk)
	{
		// an all-zero block means the extension didn't give us one
		const std::array<uint8, 14> zero{};
		if (memcmp(zero.data(), p, zero.size()) == 0)
			return;
		auto& cal = nunchuk.calibration;
		ReadCalibration(p, cal);
		cal.max.x = p[8];
		cal.min.x = p[9];
		cal.center.x = p[10];
		cal.max.y = p[11];
		cal.min.y = p[12];
		cal.center.y = p[13];
	}

	// a dot the camera doesn't see reports 0x3ff, 0x3ff
	void SetDot(IRDot& dot, uint16 x, uint16 y, uint32 size)
	{
		dot.raw = { x, y };
		dot.size = size;
		dot.visible = dot.raw != glm::vec<2, uint16>(0x3ff, 0x3ff);
		dot.pos = dot.visible ? glm::vec2(1.0f - dot.raw.x / 1023.0f, (float)dot.raw.y / 768.0f) : glm::vec2{};
	}
}

WiimoteControllerProvider::WiimoteControllerProvider()
	: m_running(true)
{
	m_reader_thread = std::thread(&WiimoteControllerProvider::reader_thread, this);
	m_writer_thread = std::thread(&WiimoteControllerProvider::writer_thread, this);
	m_connectionThread = std::thread(&WiimoteControllerProvider::connectionThread, this);
}

WiimoteControllerProvider::~WiimoteControllerProvider()
{
	if (!m_running)
		return;
	m_running = false;
	{
		std::scoped_lock lock(m_writer_mutex);
		m_writer_cond.notify_all();
	}
	{
		std::scoped_lock lock(m_connectionMutex);
		m_connectionCond.notify_all();
	}
	for (std::thread* t : { &m_writer_thread, &m_reader_thread, &m_connectionThread })
	{
		if (t->joinable())
			t->join();
	}
}

std::vector<std::shared_ptr<ControllerBase>> WiimoteControllerProvider::get_controllers()
{
	std::vector<WiimoteDevicePtr> found;
	{
		std::scoped_lock lock(m_connectedDeviceMutex);
		found = m_connectedDevices;
	}

	std::scoped_lock lock(m_device_mutex);
	for (const auto& device : found)
	{
		// also tells whether the remote still answers
		if (!device->write_data({ kStatusRequest, 0x00 }))
			continue;

		// known remotes keep their slot; a new one takes the first free slot, or a new one
		bool known = false;
		std::optional<size_t> free_slot;
		for (size_t i = m_wiimotes.size(); i-- > 0;)
		{
			const auto& slot_device = m_wiimotes[i].device;
			if (!slot_device)
				free_slot = i;
			else if (*slot_device == *device)
			{
				known = true;
				break;
			}
		}
		if (known)
			continue;
		if (free_slot)
			m_wiimotes.replace(*free_slot, std::make_unique<Wiimote>(device));
		else
			m_wiimotes.push_back(std::make_unique<Wiimote>(device));
	}

	std::vector<std::shared_ptr<ControllerBase>> result;
	result.reserve(m_wiimotes.size());
	for (size_t i = 0; i < m_wiimotes.size(); ++i)
		result.emplace_back(std::make_shared<NativeWiimoteController>(i));
	return result;
}

bool WiimoteControllerProvider::is_registered_device(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	return index < m_wiimotes.size();
}

// registered and its device is currently open
bool WiimoteControllerProvider::is_connected(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	if (index >= m_wiimotes.size())
		return false;
	return static_cast<bool>(m_wiimotes[index].device);
}

void WiimoteControllerProvider::set_rumble(size_t index, bool state)
{
	{
		std::shared_lock lock(m_device_mutex);
		if (index >= m_wiimotes.size())
			return;
		m_wiimotes[index].rumble = state;
	}
	// any report carries the rumble bit; a status request is harmless
	send_packet(index, { kStatusRequest, 0x00 });
}

void WiimoteControllerProvider::request_status(size_t index)
{
	send_packet(index, { kStatusRequest, 0x00 });
}

void WiimoteControllerProvider::set_led(size_t index, size_t player_index)
{
	// players 1-4 get one LED; 5-8 light LED 4 plus another one
	uint8 leds = (uint8)(1 << (4 + (player_index % 4)));
	if (player_index >= 4)
		leds |= (uint8)(1 << (4 + ((player_index - 3) % 4)));
	send_packet(index, { kLED, leds });
}

uint32 WiimoteControllerProvider::get_packet_delay(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	return index < m_wiimotes.size() ? m_wiimotes[index].data_delay.load() : kDefaultPacketDelay;
}

void WiimoteControllerProvider::set_packet_delay(size_t index, uint32 delay)
{
	std::shared_lock lock(m_device_mutex);
	if (index < m_wiimotes.size())
		m_wiimotes[index].data_delay = delay;
}

WiimoteControllerProvider::WiimoteState WiimoteControllerProvider::get_state(size_t index)
{
	std::shared_lock lock(m_device_mutex);
	if (index >= m_wiimotes.size())
		return {};
	std::shared_lock state_lock(m_wiimotes[index].mutex);
	return m_wiimotes[index].state;
}

void WiimoteControllerProvider::connectionThread()
{
	SetThreadName("Wiimote-connect");
	while (m_running.load(std::memory_order_relaxed))
	{
		std::vector<WiimoteDevicePtr> devices;
#ifdef HAS_HIDAPI
		for (auto& d : HidapiWiimote::get_devices())
			devices.emplace_back(std::move(d));
#endif
#ifdef HAS_BLUEZ
		for (auto& d : L2CapWiimote::get_devices())
			devices.emplace_back(std::move(d));
#endif
		{
			std::scoped_lock lock(m_connectedDeviceMutex);
			m_connectedDevices = std::move(devices);
		}
		std::unique_lock lock(m_connectionMutex);
		m_connectionCond.wait_for(lock, std::chrono::seconds(2));
	}
}

void WiimoteControllerProvider::reader_thread()
{
	SetThreadName("Wiimote-reader");
	std::chrono::steady_clock::time_point last_search{};
	while (m_running.load(std::memory_order_relaxed))
	{
		if (std::chrono::steady_clock::now() - last_search >= std::chrono::seconds(1))
		{
			get_controllers(); // picks up remotes the connection thread found
			last_search = std::chrono::steady_clock::now();
		}

		bool got_report = false;
		std::shared_lock lock(m_device_mutex);
		for (size_t index = 0; index < m_wiimotes.size(); ++index)
		{
			Wiimote& wiimote = m_wiimotes[index];
			if (!wiimote.device)
				continue;

			const auto report = wiimote.device->read_data();
			if (!report)
			{
				wiimote.device.reset(); // gone
				continue;
			}
			if (report->empty())
				continue;
			got_report = true;

			WiimoteState state;
			{
				std::shared_lock state_lock(wiimote.mutex);
				state = wiimote.state;
			}

			bool update_report = false;
			if (!handle_report(index, *report, state, update_report))
				continue;
			update_motion_sample(state);

			{
				std::unique_lock state_lock(wiimote.mutex);
				wiimote.state = state;
			}
			if (update_report)
				update_report_type(index);
		}
		lock.unlock();

		if (!got_report)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

bool WiimoteControllerProvider::handle_report(size_t index, const std::vector<uint8>& report, WiimoteState& state, bool& update_report)
{
	const auto id = (InputReportId)report[0];
	const uint8* data = report.data() + 1;
	switch (id)
	{
	case kStatus:
		// BB BB FF 00 00 VV
		state.buttons = ReadButtons(data);
		state.flags = data[2];
		state.battery_level = data[5];

		state.ir_camera.mode = set_ir_camera(index, true);
		if (!state.m_calibrated)
			calibrate(index);
		if (!state.m_motion_plus)
			detect_motion_plus(index);

		if (state.flags & kExtensionConnected)
		{
			if (state.m_extension.index() == 0)
				request_extension(index);
		}
		else
			state.m_extension = {};

		update_report = true;
		return true;

	case kRead:
		state.buttons = ReadButtons(data);
		if (!handle_read_reply(index, data + 2, state))
			return false;
		update_report = true;
		return true;

	case kAcknowledge:
		state.buttons = ReadButtons(data);
		if (data[3] != 0)
			cemuLog_logDebug(LogType::Force, "Error {:#x} from output report {:#x}", data[3], data[2]);
		return true;

	case kDataCore:
	case kDataCoreExt8:
	case kDataCoreExt19:
	case kDataCoreIRExt:
		state.buttons = ReadButtons(data);
		return true;

	case kDataCoreAcc:
	case kDataCoreAccExt:
		state.buttons = ReadButtons(data);
		parse_acceleration(state, data);
		return true;

	case kDataCoreAccIR:
		state.buttons = ReadButtons(data);
		parse_acceleration(state, data);
		parse_ir(state, data);
		return true;

	case kDataCoreAccIRExt:
	{
		state.buttons = ReadButtons(data);
		parse_acceleration(state, data);
		const uint8* extension = data + parse_ir(state, data);
		if (auto* nunchuk = std::get_if<NunchuckData>(&state.m_extension))
			ParseNunchuk(extension, *nunchuk);
		else if (auto* classic = std::get_if<ClassicData>(&state.m_extension))
			ParseClassic(extension, *classic);
		return true;
	}

	case kDataExt:
		return true;

	default:
		cemuLog_logDebug(LogType::Force, "unhandled input packet id {} for wiimote {}", id, index);
		return true;
	}
}

// data: SE AA AA followed by up to 16 bytes (S = size - 1, E = error)
bool WiimoteControllerProvider::handle_read_reply(size_t index, const uint8* data, WiimoteState& state)
{
	const uint8 error = data[0] & 0xF;
	const uint8 size = (uint8)((data[0] >> 4) + 1);
	if (error)
	{
		// 7: no such memory, e.g. no MotionPlus
		cemuLog_logDebug(LogType::Force, "Received error on data read {:#x}", error);
		return false;
	}
	const uint16 address = ReadBE16(data + 1);
	const uint8* payload = data + 3;

	if (address == (kRegisterCalibration & 0xFFFF))
	{
		cemu_assert(size == 8);
		ReadCalibration(payload, state.m_calib_acceleration);
		state.m_calibrated = true;
		return true;
	}

	if (address == (kRegisterExtensionType & 0xFFFF))
	{
		if (size == 0xf)
			return false; // nothing plugged in
		cemu_assert(size == 6);
		switch (ReadBE48(payload))
		{
		case kExtensionNunchuck:
			state.m_extension = NunchuckData{};
			break;
		case kExtensionClassic:
		case kExtensionClassicPro:
			state.m_extension = ClassicData{};
			break;
		case kExtensionMotionPlus:
			set_motion_plus(index, true);
			state.m_motion_plus = MotionPlusData{};
			break;
		case kExtensionPartialyInserted:
			state.m_extension = {};
			request_status(index); // ask again once it sits
			break;
		case kExtensionGuitar:
		case kExtensionDrums:
		case kExtensionBalanceBoard:
		case kExtensionMotionPlusInactive:
			break; // not supported, keep what we had
		default:
			state.m_extension = {};
			break;
		}
		if (state.m_extension.index() != 0)
			send_read_packet(index, kRegisterMemory, kRegisterExtensionCalibration, 0x10);
		return true;
	}

	if (address == (kRegisterExtensionCalibration & 0xFFFF))
	{
		cemu_assert(size == 0x10);
		if (auto* nunchuk = std::get_if<NunchuckData>(&state.m_extension))
			ParseNunchukCalibration(payload, *nunchuk);
		return true;
	}

	cemuLog_logDebug(LogType::Force, "Unhandled read data received");
	return false;
}

void WiimoteControllerProvider::update_motion_sample(WiimoteState& state)
{
	state.motion_sample = AccelerometerSample(state.m_acceleration, state.m_prev_acceleration, state.m_calib_acceleration);
}

// BB BB AA AA AA: the high 8 bits per axis, low bits hidden in the button bytes
void WiimoteControllerProvider::parse_acceleration(WiimoteState& wiimote_state, const uint8*& data)
{
	glm::vec<3, uint16> raw;
	raw.x = (uint16)(((data[0] >> 5) & 3) | (data[2] << 2));
	raw.y = (uint16)(((data[1] >> 4) & 2) | (data[3] << 2));
	raw.z = (uint16)(((data[1] >> 5) & 2) | (data[4] << 2));
	data += 5;

	const auto& calib = wiimote_state.m_calib_acceleration;
	wiimote_state.m_prev_acceleration = wiimote_state.m_acceleration;
	wiimote_state.m_acceleration = glm::vec3(raw) - glm::vec3(calib.zero);

	glm::vec3 one_g = calib.gravity;
	one_g -= calib.zero;
	const glm::vec3 in_g = wiimote_state.m_acceleration / one_g;
	wiimote_state.m_roll = std::atan2(in_g.z, in_g.x) - (float)std::numbers::pi / 2.0f;
}

// turns the dots around the centre of the image so a tilted remote still points straight
void WiimoteControllerProvider::rotate_ir(WiimoteState& wiimote_state)
{
	const float angle = wiimote_state.m_roll;
	if (angle == 0.0f)
		return;
	const float s = std::sin(angle);
	const float c = std::cos(angle);
	for (IRDot& dot : wiimote_state.ir_camera.dots)
	{
		if (!dot.visible)
			continue;
		const glm::vec2 p = dot.pos - 0.5f;
		dot.pos.x = (p.x * c) + (p.y * (-s));
		dot.pos.y = (p.x * s) + (p.y * c);
		dot.pos += 0.5f;
	}
}

// The pointer is the middle between the two sensor bar dots. Keep tracking the pair
// whose direction and spacing match the last one best; with only one of them in view,
// move the last pointer by how far that dot moved.
void WiimoteControllerProvider::calculate_ir_position(WiimoteState& wiimote_state)
{
	auto& ir = wiimote_state.ir_camera;
	ir.m_prev_position = ir.position;

	auto pair = ir.indices;
	if (ir.middle.x != 0)
	{
		const float last_angle = std::atan(ir.middle.y / ir.middle.x);
		float best = std::numeric_limits<float>::max();
		for (size_t i = 0; i < ir.dots.size(); ++i)
		{
			if (!ir.dots[i].visible)
				continue;
			for (size_t j = i + 1; j < ir.dots.size(); ++j)
			{
				if (!ir.dots[j].visible)
					continue;
				const glm::vec2 mid = (ir.dots[i].pos + ir.dots[j].pos) / 2.0f;
				if (mid.x == 0)
					continue;
				if (std::abs(last_angle - std::atan(mid.y / mid.x)) > DegToRad(10.0f))
					continue;
				const float spacing_change = std::abs(ir.distance - glm::length(ir.dots[i].pos - ir.dots[j].pos));
				if (spacing_change > 0.1f && spacing_change > best)
					continue;
				best = spacing_change;
				pair = { (sint32)i, (sint32)j };
			}
		}
	}

	const IRDot& first = ir.dots[pair.first];
	const IRDot& second = ir.dots[pair.second];
	if (first.visible && second.visible)
	{
		ir.prev_dots[pair.first] = first;
		ir.prev_dots[pair.second] = second;
		ir.position = (first.pos + second.pos) / 2.0f;
		ir.middle = ir.position;
		ir.distance = glm::length(first.pos - second.pos);
		ir.indices = pair;
		ir.m_positionVisibility = PositionVisibility::FULL;
	}
	else if (first.visible)
	{
		ir.position = ir.middle + (first.pos - ir.prev_dots[pair.first].pos);
		ir.m_positionVisibility = PositionVisibility::PARTIAL;
	}
	else if (second.visible)
	{
		ir.position = ir.middle + (second.pos - ir.prev_dots[pair.second].pos);
		ir.m_positionVisibility = PositionVisibility::PARTIAL;
	}
	else
		ir.m_positionVisibility = PositionVisibility::NONE;
}

// returns the number of IR bytes used
int WiimoteControllerProvider::parse_ir(WiimoteState& wiimote_state, const uint8* data)
{
	auto& dots = wiimote_state.ir_camera.dots;
	switch (wiimote_state.ir_camera.mode)
	{
	case kIRDisabled:
		dots = {};
		return 0;
	case kBasicIR:
		// two groups of 5 bytes: X1 Y1 [y1 x1 y2 x2 high bits] X2 Y2
		for (int group = 0; group < 2; ++group)
		{
			const uint8* p = data + group * 5;
			const uint8 high = p[2];
			SetDot(dots[group * 2], (uint16)(p[0] | (((high >> 4) & 3) << 8)), (uint16)(p[1] | (((high >> 6) & 3) << 8)), 0);
			SetDot(dots[group * 2 + 1], (uint16)(p[3] | ((high & 3) << 8)), (uint16)(p[4] | (((high >> 2) & 3) << 8)), 0);
		}
		rotate_ir(wiimote_state);
		calculate_ir_position(wiimote_state);
		return 10;
	case kExtendedIR:
		// 3 bytes per dot: X Y [y x size]
		for (int i = 0; i < 4; ++i)
		{
			const uint8* p = data + i * 3;
			SetDot(dots[i], (uint16)(p[0] | (((p[2] >> 4) & 3) << 8)), (uint16)(p[1] | (((p[2] >> 6) & 3) << 8)), p[2] & 0xF);
		}
		rotate_ir(wiimote_state);
		calculate_ir_position(wiimote_state);
		return 12;
	default:
		cemu_assert(false);
		return 0;
	}
}

void WiimoteControllerProvider::request_extension(size_t index)
{
	// start it unencrypted, then read its id
	send_write_packet(index, kRegisterMemory, kRegisterExtension1, { 0x55 });
	send_write_packet(index, kRegisterMemory, kRegisterExtension2, { 0x00 });
	send_read_packet(index, kRegisterMemory, kRegisterExtensionType, 6);
}

void WiimoteControllerProvider::detect_motion_plus(size_t index)
{
	send_read_packet(index, kRegisterMemory, kRegisterMotionPlusDetect, 6);
}

void WiimoteControllerProvider::set_motion_plus(size_t index, bool state)
{
	if (state)
	{
		send_write_packet(index, kRegisterMemory, kRegisterMotionPlusInit, { 0x55 });
		send_write_packet(index, kRegisterMemory, kRegisterMotionPlusEnable, { 0x04 });
	}
	else
		send_write_packet(index, kRegisterMemory, kRegisterExtension1, { 0x55 });
}

void WiimoteControllerProvider::writer_thread()
{
	SetThreadName("Wiimote-writer");
	while (m_running.load(std::memory_order_relaxed))
	{
		std::unique_lock queue_lock(m_writer_mutex);
		while (m_write_queue.empty())
		{
			if (m_writer_cond.wait_for(queue_lock, std::chrono::milliseconds(250)) == std::cv_status::timeout && !m_running.load(std::memory_order_relaxed))
				return;
		}

		std::shared_lock device_lock(m_device_mutex);
		// the oldest report for a remote whose packet delay has passed
		std::optional<size_t> target;
		std::vector<uint8> report;
		const auto now = std::chrono::high_resolution_clock::now();
		for (auto it = m_write_queue.begin(); it != m_write_queue.end(); ++it)
		{
			if (it->first >= m_wiimotes.size())
				continue;
			const Wiimote& w = m_wiimotes[it->first];
			if (now < w.data_ts + std::chrono::milliseconds(w.data_delay.load(std::memory_order_relaxed)))
				continue;
			target = it->first;
			report = std::move(it->second);
			m_write_queue.erase(it);
			break;
		}
		queue_lock.unlock();

		if (target && !report.empty())
		{
			Wiimote& wiimote = m_wiimotes[*target];
			if (!wiimote.device)
				continue;
			if (wiimote.rumble)
				report[1] |= 1;
			if (wiimote.device->write_data(report))
				wiimote.data_ts = std::chrono::high_resolution_clock::now();
			else
			{
				wiimote.device.reset();
				wiimote.rumble = false;
			}
		}
		device_lock.unlock();

		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::this_thread::yield();
	}
}

void WiimoteControllerProvider::calibrate(size_t index)
{
	send_read_packet(index, kEEPROMMemory, kRegisterCalibration, 8);
}

// always with accelerometer data; IR and extension bytes when those are in use
void WiimoteControllerProvider::update_report_type(size_t index)
{
	InputReportId report;
	{
		std::shared_lock lock(m_wiimotes[index].mutex);
		const auto& state = m_wiimotes[index].state;
		const bool extension = state.m_extension.index() != 0;
		const bool ir = state.ir_camera.mode != kIRDisabled;
		if (extension)
			report = ir ? kDataCoreAccIRExt : kDataCoreAccExt;
		else
			report = ir ? kDataCoreAccIR : kDataCoreAcc;
	}
	cemuLog_logDebug(LogType::Force, "Setting report type to {}", report);
	send_packet(index, { kType, 0x04, report }); // 0x04: report continuously
	set_ir_camera(index, true);
}

IRMode WiimoteControllerProvider::set_ir_camera(size_t index, bool state)
{
	IRMode mode = kIRDisabled;
	{
		std::shared_lock lock(m_wiimotes[index].mutex);
		auto& camera_state = m_wiimotes[index].state;
		// with an extension attached only the shorter basic format fits in the report
		if (state)
			mode = camera_state.m_extension.index() == 0 ? kExtendedIR : kBasicIR;
		if (camera_state.ir_camera.mode == mode)
			return mode;
		camera_state.ir_camera.mode = mode;
	}

	const uint8 enable = state ? 0x04 : 0x00;
	send_packet(index, { kIR, enable });
	send_packet(index, { kIR2, enable });
	if (state)
	{
		// sensitivity block 3 from wiibrew ("Wii level 3")
		send_write_packet(index, kRegisterMemory, kRegisterIR, { 0x08 });
		send_write_packet(index, kRegisterMemory, kRegisterIRSensitivity1, { 0x02, 0x00, 0x00, 0x71, 0x01, 0x00, 0xaa, 0x00, 0x64 });
		send_write_packet(index, kRegisterMemory, kRegisterIRSensitivity2, { 0x63, 0x03 });
		send_write_packet(index, kRegisterMemory, kRegisterIRMode, { (uint8)mode });
		send_write_packet(index, kRegisterMemory, kRegisterIR, { 0x08 });
	}
	update_report_type(index);
	return mode;
}

void WiimoteControllerProvider::send_packet(size_t index, std::vector<uint8> data)
{
	cemu_assert(data.size() > 1);
	{
		std::shared_lock lock(m_device_mutex);
		if (index >= m_wiimotes.size())
			return;
	}
	std::scoped_lock lock(m_writer_mutex);
	m_write_queue.emplace_back(index, std::move(data));
	m_writer_cond.notify_one();
}

// 17 MM AA AA AA SS SS
void WiimoteControllerProvider::send_read_packet(size_t index, MemoryType type, RegisterAddress address, uint16 size)
{
	std::vector<uint8> report{
		kReadMemory, type,
		(uint8)(address >> 16), (uint8)(address >> 8), (uint8)address,
		(uint8)(size >> 8), (uint8)size,
	};
	send_packet(index, std::move(report));
}

// 16 MM AA AA AA SS DD*16
void WiimoteControllerProvider::send_write_packet(size_t index, MemoryType type, RegisterAddress address, const std::vector<uint8>& data)
{
	cemu_assert(data.size() <= 16);
	std::vector<uint8> report(6 + 16);
	report[0] = kWriteMemory;
	report[1] = type;
	report[2] = (uint8)(address >> 16);
	report[3] = (uint8)(address >> 8);
	report[4] = (uint8)address;
	report[5] = (uint8)data.size();
	std::copy(data.begin(), data.end(), report.begin() + 6);
	send_packet(index, std::move(report));
}
