// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/InputManager.h"
#include "config/ActiveSettings.h"
#include "input/ControllerFactory.h"
#include <boost/property_tree/ini_parser.hpp>
#include <pugixml.hpp>
#include <bit>
#include "Cafe/GameProfile/GameProfile.h"
#include "util/EventService.h"

// Profile layout (controllerProfiles/<name>.xml):
//
//   <emulated_controller>
//     <type>Wii U GamePad</type>
//     <profile>name</profile>              only for a named profile
//     ...settings of the emulated controller (toggle_display, device_type)
//     <controller>                         one per host controller
//       <api>SDLController</api> <uuid>..</uuid> <display_name>..</display_name>
//       <motion>, <rumble>                 only if the device has them
//       <axis>/<rotation>/<trigger> each with <deadzone> and <range>
//       ...settings of the host controller
//       <mappings> <entry><mapping>id</mapping><button>id</button></entry>... </mappings>
//     </controller>
//   </emulated_controller>

namespace
{
	constexpr std::string_view kProfileDir = "controllerProfiles";

	pugi::xml_node AddTextChild(pugi::xml_node parent, const char* name, std::string_view text)
	{
		pugi::xml_node node = parent.append_child(name);
		node.append_child(pugi::node_pcdata).set_value(std::string{ text }.c_str());
		return node;
	}

	template<typename T>
	pugi::xml_node AddValueChild(pugi::xml_node parent, const char* name, const T& value)
	{
		return AddTextChild(parent, name, fmt::format("{}", value));
	}

	void AddXmlDeclaration(pugi::xml_document& doc)
	{
		pugi::xml_node decl = doc.append_child(pugi::node_declaration);
		decl.append_attribute("version") = "1.0";
		decl.append_attribute("encoding") = "UTF-8";
	}

	void ReadAxisSetting(const pugi::xml_node& parent, const char* name, ControllerBase::AxisSetting& setting)
	{
		const pugi::xml_node node = parent.child(name);
		if (!node)
			return;
		if (const auto v = node.child("deadzone"))
			setting.deadzone = ConvertString<float>(v.child_value());
		if (const auto v = node.child("range"))
			setting.range = ConvertString<float>(v.child_value());
	}

	void WriteAxisSetting(pugi::xml_node parent, const char* name, const ControllerBase::AxisSetting& setting)
	{
		pugi::xml_node node = parent.append_child(name);
		AddValueChild(node, "deadzone", setting.deadzone);
		AddValueChild(node, "range", setting.range);
	}

	// reads one <controller> element into the emulated controller
	void LoadHostController(EmulatedController& emulated, const pugi::xml_node& node)
	{
		const pugi::xml_node api_node = node.child("api");
		const pugi::xml_node uuid_node = node.child("uuid");
		if (!api_node || !uuid_node)
			return;

		const auto api = InputAPI::from_string(api_node.child_value());
		const auto controller = ControllerFactory::CreateController(api, uuid_node.child_value(), node.child_value("display_name"));
		emulated.add_controller(controller);

		auto settings = controller->get_settings();
		ReadAxisSetting(node, "axis", settings.axis);
		ReadAxisSetting(node, "rotation", settings.rotation);
		ReadAxisSetting(node, "trigger", settings.trigger);
		if (const auto v = node.child("rumble"))
			settings.rumble = ConvertString<float>(v.child_value());
		if (const auto v = node.child("motion"))
			settings.motion = ConvertString<bool>(v.child_value());
		controller->set_settings(settings);

		controller->load(node);

		for (const pugi::xml_node entry : node.child("mappings").children("entry"))
		{
			const pugi::xml_node mapping = entry.child("mapping");
			const pugi::xml_node button = entry.child("button");
			if (!mapping || !button)
				continue;
			// PixelU: a mapping can be bound on several host controllers
			emulated.add_mapping(ConvertString<uint64>(mapping.child_value()), controller, ConvertString<uint64>(button.child_value()));
		}
	}

	// --- conversion of the old INI profiles ------------------------------------------

	// the old format had up to 27 mappings ("Controller.1" .. "Controller.27")
	constexpr int kLegacyMappingCount = 27;

	// old bit numbers of the axis directions
	constexpr std::pair<uint64, uint64> kLegacyAxisBits[] = {
		{32, kTriggerXP}, {33, kRotationXP}, {34, kRotationYP}, {35, kTriggerYP},
		{36, kAxisXN}, {37, kAxisYN}, {38, kTriggerXN}, {39, kRotationXN},
		{40, kRotationYN}, {41, kTriggerYN},
	};

	// old XInput button numbers -> XINPUT_GAMEPAD bit numbers
	constexpr std::pair<uint64, uint64> kLegacyXInputButtons[] = {
		{kButton0, 12}, {kButton1, 13}, {kButton2, 14}, {kButton3, 15}, // A B X Y
		{kButton4, 8}, {kButton5, 9}, // shoulders
		{kButton6, 4}, {kButton7, 5}, // start, back
		{kButton8, 6}, {kButton9, 7}, // thumbs
		{kButton10, 0}, {kButton11, 1}, {kButton12, 2}, {kButton13, 3}, // dpad
	};

	// old DSU button numbers -> current DSU button ids
	constexpr std::pair<uint64, uint64> kLegacyDSUButtons[] = {
		{7, kButton0}, {8, kButton1}, {9, kButton2}, {6, kButton3}, // select, L3, R3, start
		{4, kButton10}, {5, kButton11}, // L, R
		{0, kButton14}, {1, kButton13}, {2, kButton15}, {3, kButton12}, // A B X Y
	};

	template<size_t N>
	uint64 Translate(const std::pair<uint64, uint64> (&table)[N], uint64 value)
	{
		for (const auto& [from, to] : table)
		{
			if (from == value)
				return to;
		}
		return value;
	}

	// an old "button_<hex mask>" value -> current button id
	uint64 LegacyButtonId(uint64 mask, std::string_view api)
	{
		uint64 id = 0;
		if (mask != 0)
			id = (uint64)std::countr_zero(mask);

		if (id >= 24 && id <= 31) // these moved up by 8
			id += 8;
		else
			id = Translate(kLegacyAxisBits, id);

		if (api == to_string(InputAPI::XInput))
			id = Translate(kLegacyXInputButtons, id);
		else if (api == "DSU")
			id = Translate(kLegacyDSUButtons, id);
		return id;
	}

	void AddMappingEntry(pugi::xml_node mappings, int mapping, uint64 button)
	{
		pugi::xml_node entry = mappings.append_child("entry");
		AddValueChild(entry, "mapping", mapping);
		AddValueChild(entry, "button", button);
	}

	// first pressed of: main window mouse, main window touch, GamePad window mouse, GamePad window touch
	std::optional<glm::ivec2> TakePress(InputManager::MouseInfo* const (&sources)[4], bool right, bool* is_pad)
	{
		for (size_t i = 0; i < 4; ++i)
		{
			if (is_pad)
				*is_pad = i >= 2;
			InputManager::MouseInfo& info = *sources[i];
			std::unique_lock lock(info.m_mutex);
			bool& toggle = right ? info.right_down_toggle : info.left_down_toggle;
			const bool down = right ? info.right_down : info.left_down;
			if (std::exchange(toggle, false) || down)
				return info.position;
		}
		return std::nullopt;
	}
}

InputManager::InputManager()
{
#if HAS_KEYBOARD
	create_provider<KeyboardControllerProvider>();
#endif
#ifdef HAS_SDL
	create_provider<SDLControllerProvider>();
#endif
#if HAS_XINPUT
	create_provider<XInputControllerProvider>();
#endif
#if HAS_DIRECTINPUT
	create_provider<DirectInputControllerProvider>();
#endif
#if HAS_DSU
	create_provider<DSUControllerProvider>();
#endif
#if defined(HAS_GAMECUBE) && HAS_GAMECUBE && defined(HAS_LIBUSB)
	create_provider<GameCubeControllerProvider>();
#endif
#if HAS_WIIMOTE
	create_provider<WiimoteControllerProvider>();
#endif

	m_update_thread_shutdown.store(false);
	m_update_thread = std::thread(&InputManager::update_thread, this);
}

// the singleton lives until the process ends; Shutdown() releases everything
InputManager::~InputManager() = default;

void InputManager::load() noexcept
{
	for (size_t player = 0; player < kMaxController; ++player)
	{
		try
		{
			load(player);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "can't load controller profile: {}", ex.what());
		}
	}
}

bool InputManager::load(size_t player_index, std::string_view filename)
{
	fs::path xml_path = filename.empty()
		? ActiveSettings::GetConfigPath("controllerProfiles/controller{}", player_index)
		: ActiveSettings::GetConfigPath("controllerProfiles/{}", filename);
	fs::path txt_path = xml_path;
	txt_path.replace_extension(".txt");
	xml_path.replace_extension(".xml");

	if (fs::exists(txt_path) && !fs::exists(xml_path))
		migrate_config(txt_path);
	if (!fs::exists(xml_path))
		return false;

	try
	{
		const auto data = FileStream::LoadIntoMemory(xml_path);
		if (!data || data->empty())
			return false;

		pugi::xml_document doc;
		if (!doc.load_buffer(data->data(), data->size()))
			return false;

		const pugi::xml_node root = doc.document_element();
		const pugi::xml_node type_node = root.child("type");
		if (!type_node)
			return false;

		auto emulated = ControllerFactory::CreateEmulatedController(player_index, EmulatedController::type_from_string(type_node.child_value()));
		if (const auto profile = root.child("profile"))
			emulated->m_profile_name = profile.child_value();
		emulated->load(root);

		for (const pugi::xml_node node : root.children("controller"))
		{
			try
			{
				LoadHostController(*emulated, node);
			}
			catch (const std::exception& ex)
			{
				// skip this host controller, keep the rest of the profile
				cemuLog_log(LogType::Force, "can't load controller: {}", ex.what());
			}
		}

		set_controller(emulated);
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't load config file: {}", ex.what());
		return false;
	}
}

bool InputManager::migrate_config(const fs::path& file_path)
{
	try
	{
		const auto data = FileStream::LoadIntoMemory(file_path);
		if (!data || data->empty())
			return false;

		boost::property_tree::ptree ini;
		{
			std::stringstream stream(std::string((const char*)data->data(), data->size()));
			boost::property_tree::read_ini(stream, ini);
		}

		const auto emulate = ini.get<std::string>("General.emulate");
		const auto api = ini.get<std::string>("General.api");
		const auto device = ini.get_optional<std::string>("General.controller");
		const auto display_name = ini.get_optional<std::string>("General.display");

		const bool keyboard_only = api == to_string(InputAPI::Keyboard);
		std::string uuid;
		if (keyboard_only)
			uuid = to_string(InputAPI::Keyboard);
		else
		{
			if (!device)
				return false;
			uuid = *device;
			if (api == to_string(InputAPI::SDLController))
				uuid += "_0"; // SDL uuids now start with the device index
		}

		pugi::xml_document doc;
		AddXmlDeclaration(doc);
		pugi::xml_node root = doc.append_child("emulated_controller");
		AddTextChild(root, "type", emulate);

		const auto mapping_value = [&ini](int i)
		{
			return ini.get_optional<std::string>(fmt::format("Controller.{}", i));
		};

		bool uses_keyboard = keyboard_only;
		if (!keyboard_only)
		{
			pugi::xml_node controller = root.append_child("controller");
			AddTextChild(controller, "api", api);
			AddTextChild(controller, "uuid", uuid);
			if (display_name && !display_name->empty())
				AddTextChild(controller, "display_name", *display_name);

			AddTextChild(controller, "rumble", ini.get<std::string>("Controller.rumble"));
			pugi::xml_node axis = controller.append_child("axis");
			AddTextChild(axis, "deadzone", ini.get<std::string>("Controller.leftDeadzone"));
			AddTextChild(axis, "range", ini.get<std::string>("Controller.leftRange"));
			pugi::xml_node rotation = controller.append_child("rotation");
			AddTextChild(rotation, "deadzone", ini.get<std::string>("Controller.rightDeadzone"));
			AddTextChild(rotation, "range", ini.get<std::string>("Controller.rightRange"));

			pugi::xml_node mappings = controller.append_child("mappings");
			for (int i = 1; i <= kLegacyMappingCount; ++i)
			{
				const auto value = mapping_value(i);
				if (!value || value->empty())
					continue;
				if (boost::starts_with(*value, "key_"))
				{
					// keyboard keys go to an extra keyboard controller below
					uses_keyboard = true;
					continue;
				}
				if (!boost::starts_with(*value, "button_"))
					continue;
				const uint64 mask = ConvertString<uint64>(std::string_view{ *value }.substr(7), 16);
				AddMappingEntry(mappings, i, LegacyButtonId(mask, api));
			}
		}

		if (uses_keyboard)
		{
			pugi::xml_node controller = root.append_child("controller");
			AddTextChild(controller, "api", "Keyboard");
			AddTextChild(controller, "uuid", "Keyboard");
			pugi::xml_node mappings = controller.append_child("mappings");
			for (int i = 1; i <= kLegacyMappingCount; ++i)
			{
				const auto value = mapping_value(i);
				if (!value || !boost::starts_with(*value, "key_"))
					continue;
				AddMappingEntry(mappings, i, ConvertString<uint64>(std::string_view{ *value }.substr(4)));
			}
		}

		fs::path out_path = file_path;
		out_path.replace_extension(".xml");
		std::ofstream out(out_path, std::ios::out | std::ios::trunc);
		if (!out.is_open())
			return false;
		doc.save(out);
		return true;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "can't migrate config file {}: {}", file_path.string(), ex.what());
	}
	return false;
}

void InputManager::save() noexcept
{
	for (size_t player = 0; player < kMaxController; ++player)
	{
		try
		{
			save(player);
		}
		catch (const std::exception& ex)
		{
			cemuLog_log(LogType::Force, "can't save controller profile: {}", ex.what());
		}
	}
}

bool InputManager::save(size_t player_index, std::string_view filename)
{
	// a profile picked by the game profile is never written back
	if (m_is_gameprofile_set[player_index])
		return true;

	const auto emulated = get_controller(player_index);
	if (!emulated)
		return false;

	fs::path file_path = ActiveSettings::GetConfigPath(kProfileDir);
	fs::create_directories(file_path);
	const bool player_file = filename.empty();
	if (player_file)
		file_path /= fmt::format("controller{}", player_index);
	else
	{
		file_path /= _utf8ToPath(filename);
		emulated->m_profile_name = std::string{ filename };
	}
	file_path.replace_extension(".xml");

	pugi::xml_document doc;
	AddXmlDeclaration(doc);
	pugi::xml_node root = doc.append_child("emulated_controller");
	AddTextChild(root, "type", emulated->type_string());
	if (emulated->has_profile_name())
		AddTextChild(root, "profile", emulated->get_profile_name());
	emulated->save(root);

	for (const auto& controller : emulated->get_controllers())
	{
		pugi::xml_node node = root.append_child("controller");
		AddTextChild(node, "api", controller->api_name());
		AddTextChild(node, "uuid", controller->uuid());
		AddTextChild(node, "display_name", controller->display_name());

		const auto settings = controller->get_settings();
		if (controller->has_motion())
			AddValueChild(node, "motion", settings.motion);
		if (controller->has_rumble())
			AddValueChild(node, "rumble", settings.rumble);
		WriteAxisSetting(node, "axis", settings.axis);
		WriteAxisSetting(node, "rotation", settings.rotation);
		WriteAxisSetting(node, "trigger", settings.trigger);

		controller->save(node);

		pugi::xml_node mappings = node.append_child("mappings");
		for (const auto& [mapping, button] : emulated->bindings_of(*controller))
		{
			pugi::xml_node entry = mappings.append_child("entry");
			AddValueChild(entry, "mapping", mapping);
			AddValueChild(entry, "button", button);
		}
	}

	std::stringstream xml;
	doc.save(xml);
	const std::string text = xml.str();
	std::unique_ptr<FileStream> file(FileStream::createFile2(file_path));
	if (!file)
		return false;
	file->writeData(text.data(), text.size());
	return true;
}

bool InputManager::is_gameprofile_set(size_t player_index) const
{
	return m_is_gameprofile_set[player_index];
}

EmulatedControllerPtr InputManager::set_controller(EmulatedControllerPtr controller)
{
	auto previous = delete_controller(controller->player_index());
	if (previous && controller->get_controllers().empty())
	{
		for (const auto& c : previous->get_controllers())
			controller->add_controller(c);
	}

	std::scoped_lock lock(m_mutex);
	const std::span<EmulatedControllerPtr> slots = controller->type() == EmulatedController::Type::VPAD
		? std::span<EmulatedControllerPtr>(m_vpad)
		: std::span<EmulatedControllerPtr>(m_wpad);
	for (auto& slot : slots)
	{
		if (!slot)
		{
			slot = std::move(controller);
			return previous;
		}
	}
	cemu_assert_debug(false); // every slot of this kind is taken
	return previous;
}

EmulatedControllerPtr InputManager::set_controller(size_t player_index, EmulatedController::Type type)
{
	try
	{
		auto controller = ControllerFactory::CreateEmulatedController(player_index, type);
		set_controller(controller);
		return controller;
	}
	catch (const std::exception& ex)
	{
		cemuLog_log(LogType::Force, "Unable to set controller type {} on player index {}: {}", type, player_index, ex.what());
	}
	return {};
}

EmulatedControllerPtr InputManager::set_controller(size_t player_index, EmulatedController::Type type, const std::shared_ptr<ControllerBase>& controller)
{
	auto result = set_controller(player_index, type);
	if (result)
		result->add_controller(controller);
	return result;
}

EmulatedControllerPtr InputManager::get_controller(size_t player_index) const
{
	std::shared_lock lock(m_mutex);
	for (const std::span<const EmulatedControllerPtr> slots : { std::span<const EmulatedControllerPtr>(m_vpad), std::span<const EmulatedControllerPtr>(m_wpad) })
	{
		for (const auto& pad : slots)
		{
			if (pad && pad->player_index() == player_index)
				return pad;
		}
	}
	return {};
}

EmulatedControllerPtr InputManager::delete_controller(size_t player_index, bool delete_profile)
{
	std::scoped_lock lock(m_mutex);
	for (const std::span<EmulatedControllerPtr> slots : { std::span<EmulatedControllerPtr>(m_vpad), std::span<EmulatedControllerPtr>(m_wpad) })
	{
		for (auto& pad : slots)
		{
			if (!pad || pad->player_index() != player_index)
				continue;
			EmulatedControllerPtr removed = std::move(pad);
			pad.reset();
			if (delete_profile)
			{
				std::error_code ec;
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.xml", player_index), ec);
				fs::remove(ActiveSettings::GetConfigPath("controllerProfiles/controller{}.txt", player_index), ec);
			}
			return removed;
		}
	}
	return {};
}

std::shared_ptr<VPADController> InputManager::get_vpad_controller(size_t index) const
{
	if (index >= m_vpad.size())
		return {};
	std::shared_lock lock(m_mutex);
	return std::dynamic_pointer_cast<VPADController>(m_vpad[index]);
}

std::shared_ptr<WPADController> InputManager::get_wpad_controller(size_t index) const
{
	if (index >= m_wpad.size())
		return {};
	std::shared_lock lock(m_mutex);
	return std::dynamic_pointer_cast<WPADController>(m_wpad[index]);
}

std::pair<size_t, size_t> InputManager::get_controller_count() const
{
	std::shared_lock lock(m_mutex);
	const auto used = [](const auto& slots)
	{
		return (size_t)std::count_if(slots.begin(), slots.end(), [](const EmulatedControllerPtr& p) { return p != nullptr; });
	};
	return { used(m_vpad), used(m_wpad) };
}

void InputManager::on_device_changed()
{
	{
		std::shared_lock lock(m_mutex);
		for (const auto& pad : m_vpad)
			if (pad) pad->connect();
		for (const auto& pad : m_wpad)
			if (pad) pad->connect();
	}
	EventService::instance().signal<Events::ControllerChanged>();
}

ControllerProviderPtr InputManager::get_api_provider(InputAPI::Type api) const
{
	if (m_api_available[api].empty())
	{
		cemu_assert_debug(false);
		return {};
	}
	return m_api_available[api].front();
}

ControllerProviderPtr InputManager::get_api_provider(InputAPI::Type api, const ControllerProviderSettings& settings)
{
	auto& providers = m_api_available[api];
	for (const auto& provider : providers)
	{
		if (*provider == settings)
			return provider;
	}
	auto provider = ControllerFactory::CreateControllerProvider(api, settings);
	providers.emplace_back(provider);
	return provider;
}

void InputManager::apply_game_profile()
{
	const auto& profiles = g_current_game_profile->GetControllerProfile();
	for (size_t player = 0; player < kMaxController; ++player)
	{
		const auto& name = profiles[player];
		if (!name || name->empty() || !load(player, *name))
			continue;
		m_is_gameprofile_set[player] = true;
		if (const auto controller = get_controller(player); controller && !controller->has_profile_name())
			controller->m_profile_name = *name;
	}
}

std::vector<std::string> InputManager::get_profiles()
{
	const auto dir = ActiveSettings::GetConfigPath(kProfileDir);
	if (!fs::exists(dir))
		return {};

	std::set<std::string> names; // sorted, and a profile in both formats counts once
	for (const auto& entry : fs::directory_iterator(dir))
	{
		const fs::path& path = entry.path();
		const auto ext = path.extension();
		if (ext != ".xml" && ext != ".txt")
			continue;
		std::string name = _pathToUtf8(path.filename().stem());
		if (is_valid_profilename(name))
			names.insert(std::move(name));
	}
	return { names.begin(), names.end() };
}

bool InputManager::is_valid_profilename(const std::string& name)
{
	if (!IsValidFilename(name))
		return false;
	// controller0..7 are the per-player files
	for (size_t player = 0; player < kMaxController; ++player)
	{
		if (name == fmt::format("controller{}", player))
			return false;
	}
	return true;
}

glm::ivec2 InputManager::get_mouse_position(bool pad_window) const
{
	const MouseInfo& info = pad_window ? m_pad_mouse : m_main_mouse;
	std::shared_lock lock(info.m_mutex);
	return info.position;
}

std::optional<glm::ivec2> InputManager::get_left_down_mouse_info(bool* is_pad)
{
	MouseInfo* const sources[4] = { &m_main_mouse, &m_main_touch, &m_pad_mouse, &m_pad_touch };
	const auto result = TakePress(sources, false, is_pad);
	return result;
}

std::optional<glm::ivec2> InputManager::get_right_down_mouse_info(bool* is_pad)
{
	MouseInfo* const sources[4] = { &m_main_mouse, &m_main_touch, &m_pad_mouse, &m_pad_touch };
	return TakePress(sources, true, is_pad);
}

void InputManager::update_thread()
{
	SetThreadName("Input_update");
	while (!m_update_thread_shutdown.load(std::memory_order::relaxed))
	{
		{
			std::shared_lock lock(m_mutex);
			for (const auto& pad : m_vpad)
				if (pad) pad->update();
			for (const auto& pad : m_wpad)
				if (pad) pad->update();
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		std::this_thread::yield();
	}
}

void InputManager::Shutdown()
{
	m_update_thread_shutdown = true;
	if (m_update_thread.joinable())
		m_update_thread.join();

	{
		std::scoped_lock lock(m_mutex);
		for (auto& pad : m_vpad)
			pad.reset();
		for (auto& pad : m_wpad)
			pad.reset();
	}
	for (auto& providers : m_api_available)
		providers.clear();
}
