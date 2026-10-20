// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/emulated/VPADController.h"
#include "input/api/Controller.h"
#ifdef HAS_SDL
#include "input/api/SDL/SDLController.h"
#endif
#include "WindowSystem.h"
#include "input/InputManager.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/CafeSystem.h"

namespace
{
	// VPAD hold/trig/release bits
	namespace Bit
	{
		constexpr uint32 Home = 0x00000002;
		constexpr uint32 Minus = 0x00000004;
		constexpr uint32 Plus = 0x00000008;
		constexpr uint32 R = 0x00000010;
		constexpr uint32 L = 0x00000020;
		constexpr uint32 ZR = 0x00000040;
		constexpr uint32 ZL = 0x00000080;
		constexpr uint32 Down = 0x00000100;
		constexpr uint32 Up = 0x00000200;
		constexpr uint32 Right = 0x00000400;
		constexpr uint32 Left = 0x00000800;
		constexpr uint32 Y = 0x00001000;
		constexpr uint32 X = 0x00002000;
		constexpr uint32 B = 0x00004000;
		constexpr uint32 A = 0x00008000;
		constexpr uint32 StickR = 0x00020000;
		constexpr uint32 StickL = 0x00040000;
		constexpr uint32 RStickDown = 0x00800000;
		constexpr uint32 RStickUp = 0x01000000;
		constexpr uint32 RStickRight = 0x02000000;
		constexpr uint32 RStickLeft = 0x04000000;
		constexpr uint32 LStickDown = 0x08000000;
		constexpr uint32 LStickUp = 0x10000000;
		constexpr uint32 LStickRight = 0x20000000;
		constexpr uint32 LStickLeft = 0x40000000;
		constexpr uint32 Repeat = 0x80000000;
	}

	// a stick direction counts as pressed past half way, and stays pressed down to 10%
	constexpr float kStickPress = 0.5f;
	constexpr float kStickRelease = 0.1f;

	uint32 StickBits(const glm::vec2& stick, uint32 previous, uint32 left, uint32 right, uint32 down, uint32 up)
	{
		uint32 bits = 0;
		const auto pushed = [previous](float v, uint32 bit, bool negative)
		{
			const float along = negative ? -v : v;
			return along >= kStickPress || ((previous & bit) != 0 && along >= kStickRelease);
		};
		if (pushed(stick.x, left, true))
			bits |= left;
		else if (pushed(stick.x, right, false))
			bits |= right;
		if (pushed(stick.y, down, true))
			bits |= down;
		else if (pushed(stick.y, up, false))
			bits |= up;
		return bits;
	}

	// raw touch panel units as the x86 build produced them (out of range and NaN give 0)
	uint16 PanelUnits(float v)
	{
		if (!(v > -2147483648.0f && v < 2147483648.0f))
			return 0;
		return (uint16)(sint32)v;
	}

	// relative position (0..1) on the screen image to raw panel coordinates; VPADGetTPCalibratedPoint undoes this
	void TouchFromRelative(VPADTPData_t& tp, float rx, float ry)
	{
		tp.x = PanelUnits((rx * 3883.0f) + 92.0f);
		tp.y = PanelUnits(4095.0f - (ry * 3694.0f) - 254.0f);
	}

	glm::vec2 CombineDirections(float left, float right, float up, float down)
	{
		return { (left > right) ? -left : right, (up > down) ? up : -down };
	}

	glm::vec2 ClampLength(const glm::vec2& v)
	{
		return length(v) > 1.0f ? normalize(v) : v;
	}

	using Layout = std::pair<uint64, uint64>;
	using V = VPADController;

#ifdef HAS_SDL
	constexpr Layout kSDLLeftJoyCon[] = {
		{V::kButtonId_L, kButton9}, {V::kButtonId_ZL, kTriggerXP},
		{V::kButtonId_Minus, kButton4},
		{V::kButtonId_Up, kButton11}, {V::kButtonId_Down, kButton12}, {V::kButtonId_Left, kButton13}, {V::kButtonId_Right, kButton14},
		{V::kButtonId_StickL, kButton7},
		{V::kButtonId_StickL_Up, kAxisYN}, {V::kButtonId_StickL_Down, kAxisYP}, {V::kButtonId_StickL_Left, kAxisXN}, {V::kButtonId_StickL_Right, kAxisXP},
		{V::kButtonId_Mic, kButton15},
	};
	constexpr Layout kSDLRightJoyCon[] = {
		{V::kButtonId_A, kButton0}, {V::kButtonId_B, kButton1}, {V::kButtonId_X, kButton2}, {V::kButtonId_Y, kButton3},
		{V::kButtonId_R, kButton10}, {V::kButtonId_ZR, kTriggerYP},
		{V::kButtonId_Plus, kButton6},
		{V::kButtonId_StickR, kButton8},
		{V::kButtonId_StickR_Up, kRotationYN}, {V::kButtonId_StickR_Down, kRotationYP}, {V::kButtonId_StickR_Left, kRotationXN}, {V::kButtonId_StickR_Right, kRotationXP},
	};
	// SDL's face buttons are positional; the Switch Pro Controller's labels match the GamePad's positions,
	// for other pads A/B and X/Y swap places
	constexpr Layout kSDLSwitchPro[] = {
		{V::kButtonId_A, kButton0}, {V::kButtonId_B, kButton1}, {V::kButtonId_X, kButton2}, {V::kButtonId_Y, kButton3},
		{V::kButtonId_L, kButton9}, {V::kButtonId_R, kButton10}, {V::kButtonId_ZL, kTriggerXP}, {V::kButtonId_ZR, kTriggerYP},
		{V::kButtonId_Plus, kButton6}, {V::kButtonId_Minus, kButton4},
		{V::kButtonId_Up, kButton11}, {V::kButtonId_Down, kButton12}, {V::kButtonId_Left, kButton13}, {V::kButtonId_Right, kButton14},
		{V::kButtonId_StickL, kButton7}, {V::kButtonId_StickR, kButton8},
		{V::kButtonId_StickL_Up, kAxisYN}, {V::kButtonId_StickL_Down, kAxisYP}, {V::kButtonId_StickL_Left, kAxisXN}, {V::kButtonId_StickL_Right, kAxisXP},
		{V::kButtonId_StickR_Up, kRotationYN}, {V::kButtonId_StickR_Down, kRotationYP}, {V::kButtonId_StickR_Left, kRotationXN}, {V::kButtonId_StickR_Right, kRotationXP},
	};
	constexpr Layout kSDLGeneric[] = {
		{V::kButtonId_A, kButton1}, {V::kButtonId_B, kButton0}, {V::kButtonId_X, kButton3}, {V::kButtonId_Y, kButton2},
		{V::kButtonId_L, kButton9}, {V::kButtonId_R, kButton10}, {V::kButtonId_ZL, kTriggerXP}, {V::kButtonId_ZR, kTriggerYP},
		{V::kButtonId_Plus, kButton6}, {V::kButtonId_Minus, kButton4},
		{V::kButtonId_Up, kButton11}, {V::kButtonId_Down, kButton12}, {V::kButtonId_Left, kButton13}, {V::kButtonId_Right, kButton14},
		{V::kButtonId_StickL, kButton7}, {V::kButtonId_StickR, kButton8},
		{V::kButtonId_StickL_Up, kAxisYN}, {V::kButtonId_StickL_Down, kAxisYP}, {V::kButtonId_StickL_Left, kAxisXN}, {V::kButtonId_StickL_Right, kAxisXP},
		{V::kButtonId_StickR_Up, kRotationYN}, {V::kButtonId_StickR_Down, kRotationYP}, {V::kButtonId_StickR_Left, kRotationXN}, {V::kButtonId_StickR_Right, kRotationXP},
	};
#endif
	// XInput button ids are the bit numbers of XINPUT_GAMEPAD::wButtons; its y axes point up
	constexpr Layout kXInput[] = {
		{V::kButtonId_A, kButton13}, {V::kButtonId_B, kButton12}, {V::kButtonId_X, kButton15}, {V::kButtonId_Y, kButton14},
		{V::kButtonId_L, kButton8}, {V::kButtonId_R, kButton9}, {V::kButtonId_ZL, kTriggerXP}, {V::kButtonId_ZR, kTriggerYP},
		{V::kButtonId_Plus, kButton4}, {V::kButtonId_Minus, kButton5},
		{V::kButtonId_Up, kButton0}, {V::kButtonId_Down, kButton1}, {V::kButtonId_Left, kButton2}, {V::kButtonId_Right, kButton3},
		{V::kButtonId_StickL, kButton6}, {V::kButtonId_StickR, kButton7},
		{V::kButtonId_StickL_Up, kAxisYP}, {V::kButtonId_StickL_Down, kAxisYN}, {V::kButtonId_StickL_Left, kAxisXN}, {V::kButtonId_StickL_Right, kAxisXP},
		{V::kButtonId_StickR_Up, kRotationYP}, {V::kButtonId_StickR_Down, kRotationYN}, {V::kButtonId_StickR_Left, kRotationXN}, {V::kButtonId_StickR_Right, kRotationXP},
	};
}

void VPADController::VPADRead(VPADStatus_t& status, const BtnRepeat& repeat)
{
	controllers_update_states();

	m_mic_active = false;
	m_screen_active = false;
	uint32 hold = status.hold;
	for (uint32 id = kButtonId_A; id < kButtonId_Max; ++id)
	{
		if (is_axis_mapping(id) || !is_mapping_down(id))
			continue; // stick directions come from the stick values below
		const uint32 bit = get_emulated_button_flag(id);
		if (bit != 0)
			hold |= bit;
		else if (id == kButtonId_Mic)
			m_mic_active = true;
		else if (id == kButtonId_Screen)
			m_screen_active = true;
	}

	m_homebutton_down |= is_home_down();

	const glm::vec2 left = get_axis();
	status.leftStick.x = left.x;
	status.leftStick.y = left.y;
	hold |= StickBits(left, m_last_hold, Bit::LStickLeft, Bit::LStickRight, Bit::LStickDown, Bit::LStickUp);

	const glm::vec2 right = get_rotation();
	status.rightStick.x = right.x;
	status.rightStick.y = right.y;
	hold |= StickBits(right, m_last_hold, Bit::RStickLeft, Bit::RStickRight, Bit::RStickDown, Bit::RStickUp);

	// button repeat: after `delay` ms of an unchanged hold, flag a repeat every `pulse` ms
	const auto now = Clock::now();
	if (hold != m_last_hold)
		m_hold_since = m_last_repeat = now;
	uint32 reported = hold;
	if (repeat.pulse > 0 && hold != 0 && m_hold_since + std::chrono::milliseconds(repeat.delay) <= now
		&& m_last_repeat + std::chrono::milliseconds(repeat.pulse) < now)
	{
		m_last_repeat = now;
		reported |= Bit::Repeat;
	}
	status.hold = reported;
	status.release = m_last_hold & ~hold;
	status.trig = ~m_last_hold & hold;
	m_last_hold = hold;

	update_touch(status);

	// level and at rest unless a motion source says otherwise
	status.dir.x = { 1.0f, 0.0f, 0.0f };
	status.dir.y = { 0.0f, 1.0f, 0.0f };
	status.dir.z = { 0.0f, 0.0f, 1.0f };
	status.accXY = { 1.0f, 0.0f };
	update_motion(status);
}

void VPADController::update()
{
	EmulatedController::update();

	if (!CafeSystem::IsTitleRunning())
		return;

	std::unique_lock lock(m_rumble_mutex);
	if (m_rumble_queue.empty())
	{
		m_rumble_step = 0;
		lock.unlock();
		stop_rumble();
		return;
	}

	const auto now = now_cached();
	if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_rumble_step).count() < 1000 / 60)
		return;
	m_last_rumble_step = now;

	const std::vector<bool>& pattern = m_rumble_queue.front();
	if (pattern[m_rumble_step])
		start_rumble();
	else
		stop_rumble();
	if (++m_rumble_step >= pattern.size())
	{
		m_rumble_queue.pop();
		m_rumble_step = 0;
	}
}

void VPADController::update_touch(VPADStatus_t& status)
{
	VPADTPData_t& tp = status.tpData;
	tp.touch = kTpTouchOff;
	tp.validity = kTpInvalid;
	// after release the panel keeps the last position; some games (NGDK titles such as
	// The Mysterious Cities of Gold) read it then
	tp.x = (uint16)m_last_touch_position.x;
	tp.y = (uint16)m_last_touch_position.y;

	bool touched = false;
	if (has_position())
	{
		const glm::vec2 pos = get_position();
		tp.x = PanelUnits(pos.x * 3883.0f + 92.0f);
		tp.y = PanelUnits(4095.0f - pos.y * 3694.0f - 254.0f);
		touched = true;
	}
	else
	{
		// mouse or touch on the main window (which may show the GamePad image) or on the GamePad window
		bool padWindow = false;
		if (const auto point = InputManager::instance().get_left_down_mouse_info(&padWindow))
		{
			glm::ivec2 origin, size;
			LatteRenderTarget_getScreenImageArea(&origin.x, &origin.y, &size.x, &size.y, nullptr, nullptr, padWindow);
			glm::vec2 rel = *point - origin;
			rel = { std::min(rel.x, (float)size.x), std::min(rel.y, (float)size.y) };
			rel = { std::max(rel.x, 0.0f), std::max(rel.y, 0.0f) };
			rel /= size;
			TouchFromRelative(tp, rel.x, rel.y);
			touched = true;
		}
	}
	if (touched)
	{
		tp.touch = kTpTouchOn;
		tp.validity = kTpValid;
		m_last_touch_position = glm::ivec2{ (uint16)tp.x, (uint16)tp.y };
	}

	status.tpProcessed1 = tp;
	status.tpProcessed2 = tp;
}

void VPADController::update_motion(VPADStatus_t& status)
{
	if (has_motion())
	{
		MotionSample sample = get_motion_data();

		float v[3];
		sample.getVPADAccelerometer(v);
		status.acc.x = v[0];
		status.acc.y = v[1];
		status.acc.z = v[2];
		status.accMagnitude = sample.getVPADAccMagnitude();
		status.accAcceleration = sample.getVPADAccAcceleration();

		sample.getVPADGyroChange(v);
		status.gyroChange.x = v[0];
		status.gyroChange.y = v[1];
		status.gyroChange.z = v[2];

		sample.getVPADOrientation(v);
		status.gyroOrientation.x = v[0];
		status.gyroOrientation.y = v[1];
		status.gyroOrientation.z = v[2];

		float m[9];
		sample.getVPADAttitudeMatrix(m);
		beVec3D_t* rows[3] = { &status.dir.x, &status.dir.y, &status.dir.z };
		for (int r = 0; r < 3; ++r)
		{
			rows[r]->x = m[r * 3 + 0];
			rows[r]->y = m[r * 3 + 1];
			rows[r]->z = m[r * 3 + 2];
		}
		return;
	}

	bool padWindow = false;
	if (const auto point = InputManager::instance().get_right_down_mouse_info(&padWindow))
		mouse_motion(status, *point, padWindow);
}

// Right mouse drag tilts the GamePad: the cursor position across the window gives pitch
// (up/down, +-135 degrees) and yaw (left/right, +-180 degrees), the wheel adds roll.
void VPADController::mouse_motion(VPADStatus_t& status, const glm::ivec2& mouse, bool padWindow)
{
	int w, h;
	if (padWindow)
		WindowSystem::GetPadWindowPhysSize(w, h);
	else
		WindowSystem::GetWindowPhysSize(w, h);

	auto& input = InputManager::instance();
	const float fx = (float)mouse.x / w;
	const float fy = (float)mouse.y / h;
	const float pitch = (fy * 2 - 1.0f) * 135.0f;
	const float yaw = (fx * 2 - 1.0f) * -180.0f;
	const float roll = input.m_mouse_wheel * 14.0f + m_mouse_rotation.z;
	input.m_mouse_wheel = 0.0f;

	Quaternion<float> attitude(pitch, yaw, roll);
	const auto axes = attitude.GetTransposedRotationMatrix();
	status.dir.x = std::get<0>(axes);
	status.dir.y = std::get<1>(axes);
	status.dir.z = std::get<2>(axes);

	glm::vec3 change(pitch - m_mouse_rotation.x, (yaw - m_mouse_rotation.y) * 15.0f, roll - m_mouse_rotation.z);
	for (int i = 0; i < 3; ++i)
		change[i] = std::min(1.0f, std::max(-1.0f, change[i] / 360.0f));

	status.gyroChange = { change.x, change.y, change.z };
	status.gyroOrientation = { change.x, change.y, change.z };
	status.acc = { change.x, change.y, change.z };
	status.accAcceleration = 1.0f;
	status.accMagnitude = 1.0f;
	status.accXY = { 1.0f, 0.0f };

	m_mouse_rotation = { pitch, yaw, roll };
}

std::string_view VPADController::get_button_name(ButtonId id)
{
	switch (id)
	{
	// the d-pad and both sticks share direction names, the stick buttons are "click"
	case kButtonId_Up: case kButtonId_StickL_Up: case kButtonId_StickR_Up: return TR_NOOP("up");
	case kButtonId_Down: case kButtonId_StickL_Down: case kButtonId_StickR_Down: return TR_NOOP("down");
	case kButtonId_Left: case kButtonId_StickL_Left: case kButtonId_StickR_Left: return TR_NOOP("left");
	case kButtonId_Right: case kButtonId_StickL_Right: case kButtonId_StickR_Right: return TR_NOOP("right");
	case kButtonId_StickL: case kButtonId_StickR: return TR_NOOP("click");
	case kButtonId_Home: return TR_NOOP("home");
	// face, shoulder and menu buttons are named as printed on the GamePad
	case kButtonId_A: return "A"; case kButtonId_B: return "B";
	case kButtonId_X: return "X"; case kButtonId_Y: return "Y";
	case kButtonId_L: return "L"; case kButtonId_R: return "R";
	case kButtonId_ZL: return "ZL"; case kButtonId_ZR: return "ZR";
	case kButtonId_Plus: return "+"; case kButtonId_Minus: return "-";
	default: break;
	}
	cemu_assert_debug(false);
	return "";
}

void VPADController::clear_rumble()
{
	std::scoped_lock lock(m_rumble_mutex);
	m_rumble_step = 0;
	m_rumble_queue = {};
}

bool VPADController::push_rumble(uint8* pattern, uint8 length)
{
	// an empty pattern stops the motor
	const bool stop = !pattern || length == 0;
	if (stop)
	{
		clear_rumble();
		return true;
	}

	constexpr size_t kMaxQueuedPatterns = 5;
	std::scoped_lock lock(m_rumble_mutex);
	if (m_rumble_queue.size() >= kMaxQueuedPatterns)
	{
		cemuLog_logDebugOnce(LogType::Force, "VPADControlMotor(): Pattern too long");
		return false;
	}

	// `length` counts pattern bits; each pair of bits is one step, on if either bit is set
	std::vector<bool> steps;
	for (int bit = 0; bit < (int)length; bit += 2)
	{
		const uint8 byte = pattern[bit / 8];
		steps.push_back((byte & (3 << (bit % 8))) != 0);
	}
	m_rumble_queue.push(std::move(steps));
	m_last_rumble_step = {};
	return true;
}

uint32 VPADController::get_emulated_button_flag(uint32 id) const
{
	switch (id)
	{
	case kButtonId_A: return Bit::A;
	case kButtonId_B: return Bit::B;
	case kButtonId_X: return Bit::X;
	case kButtonId_Y: return Bit::Y;
	case kButtonId_L: return Bit::L;
	case kButtonId_R: return Bit::R;
	case kButtonId_ZL: return Bit::ZL;
	case kButtonId_ZR: return Bit::ZR;
	case kButtonId_Plus: return Bit::Plus;
	case kButtonId_Minus: return Bit::Minus;
	case kButtonId_Up: return Bit::Up;
	case kButtonId_Down: return Bit::Down;
	case kButtonId_Left: return Bit::Left;
	case kButtonId_Right: return Bit::Right;
	case kButtonId_StickL: return Bit::StickL;
	case kButtonId_StickR: return Bit::StickR;
	case kButtonId_StickL_Up: return Bit::LStickUp;
	case kButtonId_StickL_Down: return Bit::LStickDown;
	case kButtonId_StickL_Left: return Bit::LStickLeft;
	case kButtonId_StickL_Right: return Bit::LStickRight;
	case kButtonId_StickR_Up: return Bit::RStickUp;
	case kButtonId_StickR_Down: return Bit::RStickDown;
	case kButtonId_StickR_Left: return Bit::RStickLeft;
	case kButtonId_StickR_Right: return Bit::RStickRight;
	// the home button is reported through was_home_button_down, mic and screen have no bit
	default: return 0;
	}
}

glm::vec2 VPADController::get_axis() const
{
	return ClampLength(CombineDirections(get_axis_value(kButtonId_StickL_Left), get_axis_value(kButtonId_StickL_Right),
		get_axis_value(kButtonId_StickL_Up), get_axis_value(kButtonId_StickL_Down)));
}

glm::vec2 VPADController::get_rotation() const
{
	return ClampLength(CombineDirections(get_axis_value(kButtonId_StickR_Left), get_axis_value(kButtonId_StickR_Right),
		get_axis_value(kButtonId_StickR_Up), get_axis_value(kButtonId_StickR_Down)));
}

glm::vec2 VPADController::get_trigger() const
{
	return { get_axis_value(kButtonId_ZL), get_axis_value(kButtonId_ZR) };
}

bool VPADController::set_default_mapping(const std::shared_ptr<ControllerBase>& controller)
{
	std::span<const Layout> layout;
	switch (controller->api())
	{
#ifdef HAS_SDL
	case InputAPI::SDLController:
	{
		const auto& guid = std::static_pointer_cast<SDLController>(controller)->get_guid();
		if (guid == SDLController::kLeftJoyCon)
			layout = kSDLLeftJoyCon;
		else if (guid == SDLController::kRightJoyCon)
			layout = kSDLRightJoyCon;
		else if (guid == SDLController::kSwitchProController)
			layout = kSDLSwitchPro;
		else
			layout = kSDLGeneric;
		break;
	}
#endif
	case InputAPI::XInput:
		layout = kXInput;
		break;
	default:
		break;
	}
	return apply_default_bindings(controller, layout);
}

void VPADController::load(const pugi::xml_node& node)
{
	if (const auto value = node.child("toggle_display"))
		m_screen_active_toggle = ConvertString<bool>(value.child_value());
}

void VPADController::save(pugi::xml_node& node)
{
	node.append_child("toggle_display").append_child(pugi::node_pcdata).set_value(fmt::format("{}", (int)m_screen_active_toggle).c_str());
}
