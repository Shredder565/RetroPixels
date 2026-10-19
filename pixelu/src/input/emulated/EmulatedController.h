// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// An emulated controller is what a game sees in one player slot: a GamePad (VPAD),
// a Pro Controller, a Classic Controller or a Wii remote. It is driven by one or more
// host controllers, and a mapping table says which host button (or stick direction)
// presses which emulated button.
//
// Terms used here:
//   mapping  the emulated button id (the ButtonId enums of the derived classes)
//   button   the host controller's button id (Buttons2 / the API's numbers)
//
// PixelU lets one emulated button be bound on several host controllers at once (for
// example a gamepad button and a keyboard key). Each mapping has one main binding,
// which is what the input settings window shows and edits, plus any number of extra
// bindings on other controllers. All of them are saved in the profile.

#include <pugixml.hpp>

class ControllerBase;

#include "input/motion/MotionSample.h"

#include "input/api/ControllerState.h"
#include "input/api/InputAPI.h"

#include "util/helpers/helpers.h"

class EmulatedController
{
	friend class InputManager;
public:
	EmulatedController(size_t player_index);
	virtual ~EmulatedController() = default;

	// settings of the emulated controller itself in its profile
	virtual void load(const pugi::xml_node& node) {}
	virtual void save(pugi::xml_node& node) {}

	enum Type
	{
		VPAD,
		Pro,
		Classic,
		Wiimote,

		MAX
	};
	virtual Type type() const = 0;
	std::string_view type_string() const { return type_to_string(type()); }

	// the names used in profiles
	static std::string_view type_to_string(Type type);
	static Type type_from_string(std::string_view str);

	size_t player_index() const { return m_player_index; }
	const std::string& get_profile_name() const { return m_profile_name; }
	bool has_profile_name() const { return !m_profile_name.empty() && m_profile_name != "default"; }

	void calibrate();

	void connect();
	virtual void update();
	// reads every host controller once; done before each sample a game asks for
	void controllers_update_states();

	virtual glm::vec2 get_axis() const = 0;
	virtual glm::vec2 get_rotation() const = 0;
	virtual glm::vec2 get_trigger() const = 0;

	void start_rumble();
	void stop_rumble();

	bool is_battery_low() const;

	bool has_motion() const;
	MotionSample get_motion_data() const;

	// a second motion source: the Nunchuk of a Wii remote, or else the second motion controller
	bool has_second_motion() const;
	MotionSample get_second_motion_data() const;

	// pointer / touch position from the first host controller that has one
	bool has_position() const;
	glm::vec2 get_position() const;
	glm::vec2 get_prev_position() const;
	PositionVisibility GetPositionVisibility() const;

	void add_controller(std::shared_ptr<ControllerBase> controller);
	// also drops every binding on that controller
	void remove_controller(const std::shared_ptr<ControllerBase>& controller);
	void clear_controllers();
	const std::vector<std::shared_ptr<ControllerBase>>& get_controllers() const { return m_controllers; }

	bool is_mapping_down(uint64 mapping) const;
	// name and controller of the main binding
	std::string get_mapping_name(uint64 mapping) const;
	std::shared_ptr<ControllerBase> get_mapping_controller(uint64 mapping) const;
	void delete_mapping(uint64 mapping);
	void clear_mappings();
	// makes this the only binding of the mapping
	void set_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller_base, uint64 button);
	// PixelU: binds the mapping on this controller as well, keeping the bindings other controllers have for it
	void add_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller_base, uint64 button);

	virtual uint32 get_emulated_button_flag(uint32 mapping) const = 0;

	bool operator==(const EmulatedController& o) const;
	bool operator!=(const EmulatedController& o) const;

	virtual size_t get_highest_mapping_id() const = 0;
	virtual bool is_axis_mapping(uint64 mapping) const = 0;

	// used by the emulator's own menus and hotkeys
	virtual bool is_start_down() const = 0;
	virtual bool is_left_down() const = 0;
	virtual bool is_right_down() const = 0;
	virtual bool is_up_down() const = 0;
	virtual bool is_down_down() const = 0;
	virtual bool is_a_down() const = 0;
	virtual bool is_b_down() const = 0;
	virtual bool is_home_down() const = 0;

	// true once after the home button was seen in a game sample
	bool was_home_button_down() { return std::exchange(m_homebutton_down, false); }

	// fills the mappings that are still empty with the usual layout for this host controller
	virtual bool set_default_mapping(const std::shared_ptr<ControllerBase>& controller) { return false; }

protected:
	struct Binding
	{
		std::weak_ptr<ControllerBase> controller;
		uint64 button;
	};

	// strongest value of all bindings of the mapping, 0 to 1
	float get_axis_value(uint64 mapping) const;
	// true when the mapping has a main binding (even one whose controller is gone)
	bool has_main_binding(uint64 mapping) const;
	// set_mapping for every entry whose mapping has no main binding yet
	bool apply_default_bindings(const std::shared_ptr<ControllerBase>& controller, std::span<const std::pair<uint64, uint64>> layout);

	size_t m_player_index;
	std::string m_profile_name = "default";

	mutable std::shared_mutex m_mutex;
	std::vector<std::shared_ptr<ControllerBase>> m_controllers;

	bool m_rumble = false;
	bool m_homebutton_down = false;

private:
	struct BindingSet
	{
		std::optional<Binding> main;
		std::vector<Binding> extra; // each on another controller than `main`
	};
	std::map<uint64, BindingSet> m_bindings;

	// (mapping, button) of every live binding on `controller`, for saving the profile
	std::vector<std::pair<uint64, uint64>> bindings_of(const ControllerBase& controller) const;
};

using EmulatedControllerPtr = std::shared_ptr<EmulatedController>;

// display names, used in log lines and messages
template <>
struct fmt::formatter<EmulatedController::Type> : formatter<string_view> {
	template <typename FormatContext>
	auto format(EmulatedController::Type v, FormatContext& ctx) const {
		std::string_view name;
		switch (v)
		{
		case EmulatedController::Type::VPAD: name = "Wii U Gamepad"; break;
		case EmulatedController::Type::Pro: name = "Wii U Pro Controller"; break;
		case EmulatedController::Type::Classic: name = "Wii U Classic Controller Pro"; break;
		case EmulatedController::Type::Wiimote: name = "Wiimote"; break;
		default: throw std::invalid_argument(fmt::format("invalid emulated controller type with value {}", stdx::to_underlying(v)));
		}
		return formatter<string_view>::format(name, ctx);
	}
};
