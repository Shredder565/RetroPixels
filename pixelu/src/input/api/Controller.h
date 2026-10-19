// Original code for RetroPixels (PixelU). Not derived from Cemu.
#pragma once

// ControllerBase is one physical controller seen through a host API. It turns the
// raw readings of the device into a ControllerState: the state the device had when it
// was calibrated (at rest) is subtracted, dead zone and range are applied to the
// sticks and triggers, and every stick or trigger direction past the threshold is
// added as a button (kAxisXP ... kTriggerYN) so it can be mapped like one.

#include "input/InputManager.h"
#include "input/motion/MotionSample.h"
#include "input/api/ControllerState.h"

namespace pugi
{
	class xml_node;
}

// Button ids a controller reports. 0-31 are the API's own buttons; the rest are
// shared meanings. The numbers are stored in controller profiles.
enum Buttons2 : uint64
{
	kButton0, kButton1, kButton2, kButton3, kButton4, kButton5, kButton6, kButton7,
	kButton8, kButton9, kButton10, kButton11, kButton12, kButton13, kButton14, kButton15,
	kButton16, kButton17, kButton18, kButton19, kButton20, kButton21, kButton22, kButton23,
	kButton24, kButton25, kButton26, kButton27, kButton28, kButton29, kButton30, kButton31,

	kButtonZL, // 32
	kButtonZR,

	kButtonUp, // 34
	kButtonDown,
	kButtonLeft,
	kButtonRight,

	// stick and trigger directions, positive side (38-43)
	kAxisXP,
	kAxisYP,
	kRotationXP,
	kRotationYP,
	kTriggerXP,
	kTriggerYP,

	// negative side (44-49), each 6 after its positive partner
	kAxisXN,
	kAxisYN,
	kRotationXN,
	kRotationYN,
	kTriggerXN,
	kTriggerYN,

	kButtonMAX,

	kButtonNoneAxisMAX = kButtonRight,
	kButtonAxisStart = kAxisXP,
};

class ControllerBase
{
public:
	ControllerBase(std::string_view uuid, std::string_view display_name);
	virtual ~ControllerBase() = default;

	const std::string& uuid() const { return m_uuid; }
	const std::string& display_name() const { return m_display_name; }

	virtual std::string_view api_name() const = 0;
	virtual InputAPI::Type api() const = 0;

	virtual void update() {}

	virtual bool connect() { return is_connected(); }
	virtual bool is_connected() = 0;

	virtual bool has_battery() { return false; }
	virtual bool has_low_battery() { return false; }

	// remembers the current raw state as the resting state
	const ControllerState& calibrate();
	// reads the device and returns the processed state (also kept for get_state)
	const ControllerState& update_state();
	const ControllerState& get_state() const { return m_last_state; }
	const ControllerState& get_default_state() { return is_calibrated() ? m_default_state : calibrate(); }
	virtual ControllerState raw_state() = 0;

	bool is_calibrated() const { return m_is_calibrated; }

	// how far the mapped button or direction is pressed in the last state, 0 to 1
	float get_axis_value(uint64 button) const;
	virtual bool has_axis() const { return true; }

	bool use_motion() { return has_motion() && get_settings().motion; }
	virtual bool has_motion() { return false; }
	virtual MotionSample get_motion_sample() { return {}; }

	virtual bool has_position() { return false; }
	virtual glm::vec2 get_position() { return {}; }
	virtual glm::vec2 get_prev_position() { return {}; }
	virtual PositionVisibility GetPositionVisibility() { return PositionVisibility::NONE; }

	virtual bool has_rumble() { return false; }
	virtual void start_rumble() {}
	virtual void stop_rumble() {}

	virtual std::string get_button_name(uint64 button) const;

	// extra per-API settings stored in the controller's profile node
	virtual void save(pugi::xml_node& node) {}
	virtual void load(const pugi::xml_node& node) {}

	struct AxisSetting
	{
		AxisSetting(float deadzone = 0.25f) : deadzone(deadzone) {}
		float deadzone;
		float range = 1.0f;
	};
	struct Settings
	{
		AxisSetting axis{}, rotation{}, trigger{};
		float rumble = 0;
		bool motion = false; // only meaningful when has_motion()
	};
	Settings get_settings() const;
	void set_settings(const Settings& settings);
	void set_axis_settings(const AxisSetting& settings);
	void set_rotation_settings(const AxisSetting& settings);
	void set_trigger_settings(const AxisSetting& settings);
	void set_rumble(float rumble);
	void set_use_motion(bool state);

	// rescales `axis` around its resting value and applies dead zone and range
	void apply_axis_setting(glm::vec2& axis, const glm::vec2& default_value, const AxisSetting& setting) const;

	// the same device of the same API
	bool operator==(const ControllerBase& c) const;
	bool operator!=(const ControllerBase& c) const { return !(*this == c); }

protected:
	std::string m_uuid;
	std::string m_display_name;

	ControllerState m_last_state{};

	bool m_is_calibrated = false;
	ControllerState m_default_state{};

	mutable std::mutex m_settings_mutex;
	Settings m_settings{};
};

// Base for the controllers of one API: finds that API's provider in InputManager.
template<std::derived_from<ControllerProviderBase> TProvider>
class Controller : public ControllerBase
{
public:
	Controller(std::string_view uuid, std::string_view display_name)
		: ControllerBase(uuid, display_name)
	{
		m_provider = std::dynamic_pointer_cast<TProvider>(InputManager::instance().get_api_provider(TProvider::kAPIType));
		cemu_assert_debug(m_provider != nullptr);
	}

	// uses (and creates if needed) the provider with these settings
	Controller(std::string_view uuid, std::string_view display_name, const ControllerProviderSettings& settings)
		: ControllerBase(uuid, display_name)
	{
		m_provider = std::dynamic_pointer_cast<TProvider>(InputManager::instance().get_api_provider(TProvider::kAPIType, settings));
		cemu_assert_debug(m_provider != nullptr);
	}

	void update_provider(std::shared_ptr<TProvider> provider)
	{
		m_provider = std::move(provider);
	}

protected:
	using base_type = Controller<TProvider>;
	std::shared_ptr<TProvider> m_provider;
};

using ControllerPtr = std::shared_ptr<ControllerBase>;
