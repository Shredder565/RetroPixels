// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// The emulated Wii U GamePad. VPADRead builds one VPAD sample from the mapped host
// controllers: buttons, sticks (also as stick-direction buttons), button repeat, the
// touch screen and motion. Touch comes from a host controller with a pointer (DSU
// touchpad, Wii remote IR) or else from the mouse / touch screen on the emulator's
// main window or GamePad window. Motion comes from a host controller with motion
// sensors or else from dragging with the right mouse button.

#include "input/emulated/EmulatedController.h"
#include "Cafe/OS/libs/vpad/vpad.h"

class VPADController : public EmulatedController
{
public:
	// mapping ids, stored in profiles
	enum ButtonId
	{
		kButtonId_None,

		kButtonId_A,
		kButtonId_B,
		kButtonId_X,
		kButtonId_Y,

		kButtonId_L,
		kButtonId_R,
		kButtonId_ZL,
		kButtonId_ZR,

		kButtonId_Plus,
		kButtonId_Minus,

		kButtonId_Up,
		kButtonId_Down,
		kButtonId_Left,
		kButtonId_Right,

		kButtonId_StickL,
		kButtonId_StickR,

		kButtonId_StickL_Up,
		kButtonId_StickL_Down,
		kButtonId_StickL_Left,
		kButtonId_StickL_Right,

		kButtonId_StickR_Up,
		kButtonId_StickR_Down,
		kButtonId_StickR_Left,
		kButtonId_StickR_Right,

		kButtonId_Mic,
		kButtonId_Screen,

		kButtonId_Home,

		kButtonId_Max,
	};

	using EmulatedController::EmulatedController;

	Type type() const override { return VPAD; }

	// adds to `status` (hold must start out as the caller wants it, usually 0)
	void VPADRead(VPADStatus_t& status, const BtnRepeat& repeat);

	// also plays queued rumble patterns while a title runs
	void update() override;

	uint32 get_emulated_button_flag(uint32 id) const override;

	glm::vec2 get_axis() const override;
	glm::vec2 get_rotation() const override;
	glm::vec2 get_trigger() const override;

	// state of the two buttons that have no VPAD bit (seen in the last VPADRead)
	bool is_mic_active() { return m_mic_active; }
	bool is_screen_active() { return m_screen_active; }
	// profile option: the screen button toggles instead of being held
	bool is_screen_active_toggle() { return m_screen_active_toggle; }
	void set_screen_toggle(bool toggle) { m_screen_active_toggle = toggle; }

	static std::string_view get_button_name(ButtonId id);

	void clear_rumble();
	// VPADControlMotor pattern; false when too many patterns are waiting
	bool push_rumble(uint8* pattern, uint8 length);

	size_t get_highest_mapping_id() const override { return kButtonId_Max; }
	bool is_axis_mapping(uint64 mapping) const override { return mapping >= kButtonId_StickL_Up && mapping <= kButtonId_StickR_Right; }

	// navigation of the emulator's own menus: d-pad, confirm/back, start and home
	bool is_up_down() const override { return is_mapping_down(kButtonId_Up); }
	bool is_down_down() const override { return is_mapping_down(kButtonId_Down); }
	bool is_left_down() const override { return is_mapping_down(kButtonId_Left); }
	bool is_right_down() const override { return is_mapping_down(kButtonId_Right); }
	bool is_a_down() const override { return is_mapping_down(kButtonId_A); }
	bool is_b_down() const override { return is_mapping_down(kButtonId_B); }
	bool is_start_down() const override { return is_mapping_down(kButtonId_Plus); }
	bool is_home_down() const override { return is_mapping_down(kButtonId_Home); }

	bool set_default_mapping(const std::shared_ptr<ControllerBase>& controller) override;

	void load(const pugi::xml_node& node) override;
	void save(pugi::xml_node& node) override;

private:
	using Clock = std::chrono::high_resolution_clock;

	void update_touch(VPADStatus_t& status);
	void update_motion(VPADStatus_t& status);
	void mouse_motion(VPADStatus_t& status, const glm::ivec2& mouse, bool padWindow);

	bool m_mic_active = false;
	bool m_screen_active = false;
	bool m_screen_active_toggle = false;

	// button repeat
	uint32 m_last_hold = 0;
	Clock::time_point m_hold_since{}, m_last_repeat{};

	// rumble: each pattern is a list of motor on/off steps played at 60 steps per second
	std::mutex m_rumble_mutex;
	std::queue<std::vector<bool>> m_rumble_queue;
	size_t m_rumble_step = 0;
	Clock::time_point m_last_rumble_step{};

	// the panel keeps reporting the last touched point after release
	glm::ivec2 m_last_touch_position{};

	// right mouse drag: last emulated rotation in degrees
	glm::vec3 m_mouse_rotation{};
};
