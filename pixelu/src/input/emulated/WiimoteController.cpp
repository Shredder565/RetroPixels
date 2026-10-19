// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/emulated/WiimoteController.h"

#include "input/api/Controller.h"
#include "input/api/Wiimote/NativeWiimoteController.h"

namespace
{
	using W = WiimoteController;

	struct ButtonInfo
	{
		W::ButtonId id;
		uint32 flag;
		std::string_view name;
	};

	// every mapping id with its button bit and its name in the input settings
	constexpr ButtonInfo kButtons[] = {
		{ W::kButtonId_A, kWPADButton_A, "A" },
		{ W::kButtonId_B, kWPADButton_B, "B" },
		{ W::kButtonId_1, kWPADButton_1, "1" },
		{ W::kButtonId_2, kWPADButton_2, "2" },
		{ W::kButtonId_Nunchuck_Z, kWPADButton_Z, "Z" },
		{ W::kButtonId_Nunchuck_C, kWPADButton_C, "C" },
		{ W::kButtonId_Plus, kWPADButton_Plus, "+" },
		{ W::kButtonId_Minus, kWPADButton_Minus, "-" },
		{ W::kButtonId_Up, kWPADButton_Up, TR_NOOP("up") },
		{ W::kButtonId_Down, kWPADButton_Down, TR_NOOP("down") },
		{ W::kButtonId_Left, kWPADButton_Left, TR_NOOP("left") },
		{ W::kButtonId_Right, kWPADButton_Right, TR_NOOP("right") },
		{ W::kButtonId_Nunchuck_Up, 0, TR_NOOP("up") },
		{ W::kButtonId_Nunchuck_Down, 0, TR_NOOP("down") },
		{ W::kButtonId_Nunchuck_Left, 0, TR_NOOP("left") },
		{ W::kButtonId_Nunchuck_Right, 0, TR_NOOP("right") },
		{ W::kButtonId_Home, kWPADButton_Home, TR_NOOP("home") },
	};

	const ButtonInfo* FindButton(uint32 id)
	{
		for (const auto& b : kButtons)
		{
			if (b.id == id)
				return &b;
		}
		return nullptr;
	}

	// a real Wii remote (input API "Wiimote") maps one to one
	constexpr std::pair<uint64, uint64> kNativeLayout[] = {
		{W::kButtonId_A, kWiimoteButton_A}, {W::kButtonId_B, kWiimoteButton_B},
		{W::kButtonId_1, kWiimoteButton_One}, {W::kButtonId_2, kWiimoteButton_Two},
		{W::kButtonId_Home, kWiimoteButton_Home},
		{W::kButtonId_Plus, kWiimoteButton_Plus}, {W::kButtonId_Minus, kWiimoteButton_Minus},
		{W::kButtonId_Up, kWiimoteButton_Up}, {W::kButtonId_Down, kWiimoteButton_Down},
		{W::kButtonId_Left, kWiimoteButton_Left}, {W::kButtonId_Right, kWiimoteButton_Right},
		{W::kButtonId_Nunchuck_Z, kWiimoteButton_Z}, {W::kButtonId_Nunchuck_C, kWiimoteButton_C},
		{W::kButtonId_Nunchuck_Up, kAxisYP}, {W::kButtonId_Nunchuck_Down, kAxisYN},
		{W::kButtonId_Nunchuck_Left, kAxisXN}, {W::kButtonId_Nunchuck_Right, kAxisXP},
	};
}

WiimoteController::WiimoteController(size_t player_index)
	: WPADController(player_index, kDataFormat_CORE_ACC_DPD)
{
}

void WiimoteController::set_device_type(WPADDeviceType device_type)
{
	m_device_type = device_type;
	m_data_format = get_default_data_format();
}

bool WiimoteController::is_mpls_attached()
{
	switch (m_device_type)
	{
	case kWAPDevMPLS:
	case kWAPDevMPLSFreeStyle:
	case kWAPDevMPLSClassic:
		return true;
	default:
		return false;
	}
}

uint32 WiimoteController::s_get_emulated_button_flag(uint32 id)
{
	const ButtonInfo* b = FindButton(id);
	return b ? b->flag : 0;
}

std::string_view WiimoteController::get_button_name(ButtonId id)
{
	const ButtonInfo* b = FindButton(id);
	return b ? b->name : std::string_view{};
}

bool WiimoteController::set_default_mapping(const std::shared_ptr<ControllerBase>& controller)
{
	if (controller->api() != InputAPI::Wiimote)
		return false;
	return apply_default_bindings(controller, kNativeLayout);
}

glm::vec2 WiimoteController::get_axis() const
{
	const float left = get_axis_value(kButtonId_Nunchuck_Left);
	const float right = get_axis_value(kButtonId_Nunchuck_Right);
	const float up = get_axis_value(kButtonId_Nunchuck_Up);
	const float down = get_axis_value(kButtonId_Nunchuck_Down);
	return { (left > right) ? -left : right, (up > down) ? up : -down };
}

void WiimoteController::load(const pugi::xml_node& node)
{
	// only the device type; the data format stays until a game sets one
	if (const auto value = node.child("device_type"))
		m_device_type = ConvertString<WPADDeviceType>(value.child_value());
}

void WiimoteController::save(pugi::xml_node& node)
{
	node.append_child("device_type").append_child(pugi::node_pcdata).set_value(fmt::format("{}", (int)m_device_type).c_str());
}
