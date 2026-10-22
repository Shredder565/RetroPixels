// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/emulated/ClassicController.h"

#include "input/api/Controller.h"
#ifdef HAS_SDL
#include "input/api/SDL/SDLController.h"
#endif

namespace
{
	using C = ClassicController;

	struct ButtonInfo
	{
		C::ButtonId id;
		uint32 flag;
		std::string_view name;
	};

	// every mapping id with its button bit and its name in the input settings
	constexpr ButtonInfo kButtons[] = {
		{ C::kButtonId_A, kCLButton_A, "A" },
		{ C::kButtonId_B, kCLButton_B, "B" },
		{ C::kButtonId_X, kCLButton_X, "X" },
		{ C::kButtonId_Y, kCLButton_Y, "Y" },
		{ C::kButtonId_L, kCLButton_L, "L" },
		{ C::kButtonId_R, kCLButton_R, "R" },
		{ C::kButtonId_ZL, kCLButton_ZL, "ZL" },
		{ C::kButtonId_ZR, kCLButton_ZR, "ZR" },
		{ C::kButtonId_Plus, kCLButton_Plus, "+" },
		{ C::kButtonId_Minus, kCLButton_Minus, "-" },
		{ C::kButtonId_Home, 0, TR_NOOP("home") }, // handled by the emulator, not passed to the game
		{ C::kButtonId_Up, kCLButton_Up, TR_NOOP("up") },
		{ C::kButtonId_Down, kCLButton_Down, TR_NOOP("down") },
		{ C::kButtonId_Left, kCLButton_Left, TR_NOOP("left") },
		{ C::kButtonId_Right, kCLButton_Right, TR_NOOP("right") },
		{ C::kButtonId_StickL_Up, 0, TR_NOOP("up") },
		{ C::kButtonId_StickL_Down, 0, TR_NOOP("down") },
		{ C::kButtonId_StickL_Left, 0, TR_NOOP("left") },
		{ C::kButtonId_StickL_Right, 0, TR_NOOP("right") },
		{ C::kButtonId_StickR_Up, 0, TR_NOOP("up") },
		{ C::kButtonId_StickR_Down, 0, TR_NOOP("down") },
		{ C::kButtonId_StickR_Left, 0, TR_NOOP("left") },
		{ C::kButtonId_StickR_Right, 0, TR_NOOP("right") },
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

	// strongest direction per axis, kept inside the unit circle
	glm::vec2 StickFromDirections(float left, float right, float up, float down)
	{
		const glm::vec2 v{ (left > right) ? -left : right, (up > down) ? up : -down };
		return length(v) > 1.0f ? normalize(v) : v;
	}

	using Layout = std::pair<uint64, uint64>;

#ifdef HAS_SDL
	constexpr Layout kSDLLeftJoyCon[] = {
		{C::kButtonId_L, kButton9}, {C::kButtonId_ZL, kTriggerXP},
		{C::kButtonId_Minus, kButton4},
		{C::kButtonId_Up, kButton11}, {C::kButtonId_Down, kButton12}, {C::kButtonId_Left, kButton13}, {C::kButtonId_Right, kButton14},
		{C::kButtonId_StickL_Up, kAxisYN}, {C::kButtonId_StickL_Down, kAxisYP}, {C::kButtonId_StickL_Left, kAxisXN}, {C::kButtonId_StickL_Right, kAxisXP},
	};
	constexpr Layout kSDLRightJoyCon[] = {
		{C::kButtonId_A, kButton0}, {C::kButtonId_B, kButton1}, {C::kButtonId_X, kButton2}, {C::kButtonId_Y, kButton3},
		{C::kButtonId_R, kButton10}, {C::kButtonId_ZR, kTriggerYP},
		{C::kButtonId_Plus, kButton6},
		{C::kButtonId_StickR_Up, kRotationYN}, {C::kButtonId_StickR_Down, kRotationYP}, {C::kButtonId_StickR_Left, kRotationXN}, {C::kButtonId_StickR_Right, kRotationXP},
	};
	// SDL face buttons are positional, so A/B and X/Y swap to match the Classic Controller's labels
	constexpr Layout kSDLGeneric[] = {
		{C::kButtonId_A, kButton1}, {C::kButtonId_B, kButton0}, {C::kButtonId_X, kButton3}, {C::kButtonId_Y, kButton2},
		{C::kButtonId_L, kButton9}, {C::kButtonId_R, kButton10}, {C::kButtonId_ZL, kTriggerXP}, {C::kButtonId_ZR, kTriggerYP},
		{C::kButtonId_Plus, kButton6}, {C::kButtonId_Minus, kButton4},
		{C::kButtonId_Up, kButton11}, {C::kButtonId_Down, kButton12}, {C::kButtonId_Left, kButton13}, {C::kButtonId_Right, kButton14},
		{C::kButtonId_StickL_Up, kAxisYN}, {C::kButtonId_StickL_Down, kAxisYP}, {C::kButtonId_StickL_Left, kAxisXN}, {C::kButtonId_StickL_Right, kAxisXP},
		{C::kButtonId_StickR_Up, kRotationYN}, {C::kButtonId_StickR_Down, kRotationYP}, {C::kButtonId_StickR_Left, kRotationXN}, {C::kButtonId_StickR_Right, kRotationXP},
	};
#endif
	// XInput button ids are the bit numbers of XINPUT_GAMEPAD::wButtons; its y axes point up
	constexpr Layout kXInput[] = {
		{C::kButtonId_A, kButton13}, {C::kButtonId_B, kButton12}, {C::kButtonId_X, kButton15}, {C::kButtonId_Y, kButton14},
		{C::kButtonId_L, kButton8}, {C::kButtonId_R, kButton9}, {C::kButtonId_ZL, kTriggerXP}, {C::kButtonId_ZR, kTriggerYP},
		{C::kButtonId_Plus, kButton4}, {C::kButtonId_Minus, kButton5},
		{C::kButtonId_Up, kButton0}, {C::kButtonId_Down, kButton1}, {C::kButtonId_Left, kButton2}, {C::kButtonId_Right, kButton3},
		{C::kButtonId_StickL_Up, kAxisYP}, {C::kButtonId_StickL_Down, kAxisYN}, {C::kButtonId_StickL_Left, kAxisXN}, {C::kButtonId_StickL_Right, kAxisXP},
		{C::kButtonId_StickR_Up, kRotationYP}, {C::kButtonId_StickR_Down, kRotationYN}, {C::kButtonId_StickR_Left, kRotationXN}, {C::kButtonId_StickR_Right, kRotationXP},
	};
}

ClassicController::ClassicController(size_t player_index)
	: WPADController(player_index, kDataFormat_CLASSIC)
{
}

uint32 ClassicController::s_get_emulated_button_flag(uint32 id)
{
	const ButtonInfo* b = FindButton(id);
	return b ? b->flag : 0;
}

std::string_view ClassicController::get_button_name(ButtonId id)
{
	const ButtonInfo* b = FindButton(id);
	return b ? b->name : std::string_view{};
}

glm::vec2 ClassicController::get_axis() const
{
	return StickFromDirections(get_axis_value(kButtonId_StickL_Left), get_axis_value(kButtonId_StickL_Right),
		get_axis_value(kButtonId_StickL_Up), get_axis_value(kButtonId_StickL_Down));
}

glm::vec2 ClassicController::get_rotation() const
{
	return StickFromDirections(get_axis_value(kButtonId_StickR_Left), get_axis_value(kButtonId_StickR_Right),
		get_axis_value(kButtonId_StickR_Up), get_axis_value(kButtonId_StickR_Down));
}

glm::vec2 ClassicController::get_trigger() const
{
	return { get_axis_value(kButtonId_ZL), get_axis_value(kButtonId_ZR) };
}

bool ClassicController::set_default_mapping(const std::shared_ptr<ControllerBase>& controller)
{
	std::span<const Layout> layout;
	switch (controller->api())
	{
#ifdef HAS_SDL
	case InputAPI::SDLController:
	{
		// (an SDL controller used to fall through to the XInput layout here)
		const auto& guid = std::static_pointer_cast<SDLController>(controller)->get_guid();
		if (guid == SDLController::kLeftJoyCon)
			layout = kSDLLeftJoyCon;
		else if (guid == SDLController::kRightJoyCon)
			layout = kSDLRightJoyCon;
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
