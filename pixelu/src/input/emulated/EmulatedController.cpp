// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/emulated/EmulatedController.h"

#include "input/api/Controller.h"

#ifdef SUPPORTS_WIIMOTE
#include "input/api/Wiimote/NativeWiimoteController.h"
#endif

namespace
{
	struct TypeName
	{
		EmulatedController::Type type;
		std::string_view name;
	};
	// as written to the <type> element of a profile
	constexpr TypeName kTypeNames[] = {
		{ EmulatedController::VPAD, "Wii U GamePad" },
		{ EmulatedController::Pro, "Wii U Pro Controller" },
		{ EmulatedController::Classic, "Wii U Classic Controller" },
		{ EmulatedController::Wiimote, "Wiimote" },
	};

	// the Nunchuk of a Wii remote reports its own motion data
	bool HasNunchuk([[maybe_unused]] const ControllerBase& controller)
	{
#ifdef SUPPORTS_WIIMOTE
		if (controller.api() == InputAPI::Wiimote)
			return static_cast<const NativeWiimoteController&>(controller).get_extension() == NativeWiimoteController::Nunchuck;
#endif
		return false;
	}

	MotionSample NunchukMotion([[maybe_unused]] const ControllerBase& controller)
	{
#ifdef SUPPORTS_WIIMOTE
		return static_cast<const NativeWiimoteController&>(controller).get_nunchuck_motion_sample();
#else
		return {};
#endif
	}
}

std::string_view EmulatedController::type_to_string(Type type)
{
	for (const auto& entry : kTypeNames)
	{
		if (entry.type == type)
			return entry.name;
	}
	throw std::runtime_error(fmt::format("unknown emulated controller: {}", stdx::to_underlying(type)));
}

EmulatedController::Type EmulatedController::type_from_string(std::string_view str)
{
	for (const auto& entry : kTypeNames)
	{
		if (entry.name == str)
			return entry.type;
	}
	throw std::runtime_error(fmt::format("unknown emulated controller: {}", str));
}

EmulatedController::EmulatedController(size_t player_index)
	: m_player_index(player_index)
{
}

void EmulatedController::calibrate()
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
		c->calibrate();
}

void EmulatedController::connect()
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
		c->connect();
}

void EmulatedController::update()
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
		c->update();
}

void EmulatedController::controllers_update_states()
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
		c->update_state();
}

void EmulatedController::start_rumble()
{
	m_rumble = true;
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
		c->start_rumble();
}

void EmulatedController::stop_rumble()
{
	if (!m_rumble)
		return;
	m_rumble = false;
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
		c->stop_rumble();
}

bool EmulatedController::is_battery_low() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
	{
		if (c->has_low_battery())
			return true;
	}
	return false;
}

bool EmulatedController::has_motion() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
	{
		if (c->use_motion())
			return true;
	}
	return false;
}

MotionSample EmulatedController::get_motion_data() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
	{
		if (c->use_motion())
			return c->get_motion_sample();
	}
	return {};
}

bool EmulatedController::has_second_motion() const
{
	std::shared_lock lock(m_mutex);
	int sources = 0;
	for (const auto& c : m_controllers)
	{
		if (!c->use_motion())
			continue;
		if (HasNunchuk(*c))
			return true;
		if (++sources >= 2)
			return true;
	}
	return false;
}

MotionSample EmulatedController::get_second_motion_data() const
{
	std::shared_lock lock(m_mutex);
	int sources = 0;
	for (const auto& c : m_controllers)
	{
		if (!c->use_motion())
			continue;
		if (HasNunchuk(*c))
			return NunchukMotion(*c);
		if (++sources == 2)
			return c->get_motion_sample();
	}
	return {};
}

bool EmulatedController::has_position() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
	{
		if (c->has_position())
			return true;
	}
	return false;
}

glm::vec2 EmulatedController::get_position() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
	{
		if (c->has_position())
			return c->get_position();
	}
	return {};
}

glm::vec2 EmulatedController::get_prev_position() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
	{
		if (c->has_position())
			return c->get_prev_position();
	}
	return {};
}

PositionVisibility EmulatedController::GetPositionVisibility() const
{
	std::shared_lock lock(m_mutex);
	for (const auto& c : m_controllers)
	{
		if (c->has_position())
			return c->GetPositionVisibility();
	}
	return PositionVisibility::NONE;
}

void EmulatedController::add_controller(std::shared_ptr<ControllerBase> controller)
{
	controller->connect();
#ifdef SUPPORTS_WIIMOTE
	// a Wii remote lights the LED of the player it belongs to
	if (const auto wiimote = std::dynamic_pointer_cast<NativeWiimoteController>(controller))
		wiimote->set_player_index(m_player_index);
#endif
	std::scoped_lock lock(m_mutex);
	m_controllers.emplace_back(std::move(controller));
}

void EmulatedController::remove_controller(const std::shared_ptr<ControllerBase>& controller)
{
	std::scoped_lock lock(m_mutex);
	const auto it = std::find(m_controllers.begin(), m_controllers.end(), controller);
	if (it == m_controllers.end())
		return;
	m_controllers.erase(it);

	for (auto entry = m_bindings.begin(); entry != m_bindings.end();)
	{
		BindingSet& set = entry->second;
		// a main binding on a controller that is already gone stays (it simply never fires)
		if (set.main)
		{
			if (const auto bound = set.main->controller.lock(); bound && *bound == *controller)
				set.main.reset();
		}
		std::erase_if(set.extra, [&controller](const Binding& b)
		{
			const auto bound = b.controller.lock();
			return !bound || *bound == *controller;
		});
		if (!set.main && set.extra.empty())
			entry = m_bindings.erase(entry);
		else
			++entry;
	}
}

void EmulatedController::clear_controllers()
{
	std::scoped_lock lock(m_mutex);
	m_controllers.clear();
}

float EmulatedController::get_axis_value(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto entry = m_bindings.find(mapping);
	if (entry == m_bindings.end())
		return 0;
	const BindingSet& set = entry->second;
	float value = 0;
	if (set.main)
	{
		if (const auto c = set.main->controller.lock())
			value = c->get_axis_value(set.main->button);
	}
	for (const Binding& b : set.extra)
	{
		if (const auto c = b.controller.lock())
			value = std::max(value, c->get_axis_value(b.button));
	}
	return value;
}

bool EmulatedController::is_mapping_down(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto entry = m_bindings.find(mapping);
	if (entry == m_bindings.end())
		return false;
	const auto held = [](const Binding& b)
	{
		const auto c = b.controller.lock();
		return c && c->get_state().buttons.GetButtonState(b.button);
	};
	const BindingSet& set = entry->second;
	if (set.main && held(*set.main))
		return true;
	return std::any_of(set.extra.begin(), set.extra.end(), held);
}

std::string EmulatedController::get_mapping_name(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto entry = m_bindings.find(mapping);
	if (entry == m_bindings.end() || !entry->second.main)
		return {};
	if (const auto c = entry->second.main->controller.lock())
		return c->get_button_name(entry->second.main->button);
	return {};
}

std::shared_ptr<ControllerBase> EmulatedController::get_mapping_controller(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto entry = m_bindings.find(mapping);
	if (entry == m_bindings.end() || !entry->second.main)
		return {};
	return entry->second.main->controller.lock();
}

bool EmulatedController::has_main_binding(uint64 mapping) const
{
	std::shared_lock lock(m_mutex);
	const auto entry = m_bindings.find(mapping);
	return entry != m_bindings.end() && entry->second.main.has_value();
}

void EmulatedController::delete_mapping(uint64 mapping)
{
	std::scoped_lock lock(m_mutex);
	m_bindings.erase(mapping);
}

void EmulatedController::clear_mappings()
{
	std::scoped_lock lock(m_mutex);
	m_bindings.clear();
}

void EmulatedController::set_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller, uint64 button)
{
	std::scoped_lock lock(m_mutex);
	BindingSet& set = m_bindings[mapping];
	set.main = Binding{ controller, button };
	set.extra.clear(); // the input settings show and edit a single binding
}

void EmulatedController::add_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller, uint64 button)
{
	std::scoped_lock lock(m_mutex);
	BindingSet& set = m_bindings[mapping];
	const auto mainController = set.main ? set.main->controller.lock() : nullptr;
	if (!mainController || *mainController == *controller)
	{
		set.main = Binding{ controller, button };
		return;
	}
	for (Binding& b : set.extra)
	{
		if (const auto c = b.controller.lock(); c && *c == *controller)
		{
			b.button = button;
			return;
		}
	}
	set.extra.push_back(Binding{ controller, button });
}

bool EmulatedController::apply_default_bindings(const std::shared_ptr<ControllerBase>& controller, std::span<const std::pair<uint64, uint64>> layout)
{
	bool changed = false;
	for (const auto& [mapping, button] : layout)
	{
		if (has_main_binding(mapping))
			continue;
		set_mapping(mapping, controller, button);
		changed = true;
	}
	return changed;
}

std::vector<std::pair<uint64, uint64>> EmulatedController::bindings_of(const ControllerBase& controller) const
{
	std::vector<std::pair<uint64, uint64>> result;
	std::shared_lock lock(m_mutex);
	const auto matches = [&controller](const Binding& b)
	{
		const auto c = b.controller.lock();
		return c && *c == controller;
	};
	for (const auto& [mapping, set] : m_bindings)
	{
		if (set.main && matches(*set.main))
			result.emplace_back(mapping, set.main->button);
		for (const Binding& b : set.extra)
		{
			if (matches(b))
				result.emplace_back(mapping, b.button);
		}
	}
	return result;
}

bool EmulatedController::operator==(const EmulatedController& o) const
{
	return type() == o.type() && m_player_index == o.m_player_index;
}

bool EmulatedController::operator!=(const EmulatedController& o) const
{
	return !(*this == o);
}
