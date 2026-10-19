// Original code for RetroPixels (PixelU). Not derived from Cemu.
#include "input/emulated/ProController.h"

#include "input/api/Controller.h"
#ifdef HAS_SDL
#include "input/api/SDL/SDLController.h"
#endif

namespace
{
	using P = ProController;

	struct ButtonInfo
	{
		P::ButtonId id;
		uint32 flag;
		std::string_view name;
	};

	// every mapping id with its button bit and its name in the input settings
	constexpr ButtonInfo kButtons[] = {
		{ P::kButtonId_A, kProButton_A, "A" },
		{ P::kButtonId_B, kProButton_B, "B" },
		{ P::kButtonId_X, kProButton_X, "X" },
		{ P::kButtonId_Y, kProButton_Y, "Y" },
		{ P::kButtonId_L, kProButton_L, "L" },
		{ P::kButtonId_R, kProButton_R, "R" },
		{ P::kButtonId_ZL, kProButton_ZL, "ZL" },
		{ P::kButtonId_ZR, kProButton_ZR, "ZR" },
		{ P::kButtonId_Plus, kProButton_Plus, "+" },
		{ P::kButtonId_Minus, kProButton_Minus, "-" },
		{ P::kButtonId_Home, 0, TR_NOOP("home") }, // handled by the emulator, not passed to the game
		{ P::kButtonId_Up, kProButton_Up, TR_NOOP("up") },
		{ P::kButtonId_Down, kProButton_Down, TR_NOOP("down") },
		{ P::kButtonId_Left, kProButton_Left, TR_NOOP("left") },
		{ P::kButtonId_Right, kProButton_Right, TR_NOOP("right") },
		{ P::kButtonId_StickL, kProButton_StickL, TR_NOOP("click") },
		{ P::kButtonId_StickR, kProButton_StickR, TR_NOOP("click") },
		{ P::kButtonId_StickL_Up, 0, TR_NOOP("up") },
		{ P::kButtonId_StickL_Down, 0, TR_NOOP("down") },
		{ P::kButtonId_StickL_Left, 0, TR_NOOP("left") },
		{ P::kButtonId_StickL_Right, 0, TR_NOOP("right") },
		{ P::kButtonId_StickR_Up, 0, TR_NOOP("up") },
		{ P::kButtonId_StickR_Down, 0, TR_NOOP("down") },
		{ P::kButtonId_StickR_Left, 0, TR_NOOP("left") },
		{ P::kButtonId_StickR_Right, 0, TR_NOOP("right") },
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

	glm::vec2 CombineDirections(float left, float right, float up, float down)
	{
		return { (left > right) ? -left : right, (up > down) ? up : -down };
	}

	using Layout = std::pair<uint64, uint64>;

#ifdef HAS_SDL
	constexpr Layout kSDLLeftJoyCon[] = {
		{P::kButtonId_L, kButton9}, {P::kButtonId_ZL, kTriggerXP},
		{P::kButtonId_Minus, kButton4},
		{P::kButtonId_Up, kButton11}, {P::kButtonId_Down, kButton12}, {P::kButtonId_Left, kButton13}, {P::kButtonId_Right, kButton14},
		{P::kButtonId_StickL, kButton7},
		{P::kButtonId_StickL_Up, kAxisYN}, {P::kButtonId_StickL_Down, kAxisYP}, {P::kButtonId_StickL_Left, kAxisXN}, {P::kButtonId_StickL_Right, kAxisXP},
	};
	constexpr Layout kSDLRightJoyCon[] = {
		{P::kButtonId_A, kButton0}, {P::kButtonId_B, kButton1}, {P::kButtonId_X, kButton2}, {P::kButtonId_Y, kButton3},
		{P::kButtonId_R, kButton10}, {P::kButtonId_ZR, kTriggerYP},
		{P::kButtonId_Plus, kButton6},
		{P::kButtonId_StickR, kButton8},
		{P::kButtonId_StickR_Up, kRotationYN}, {P::kButtonId_StickR_Down, kRotationYP}, {P::kButtonId_StickR_Left, kRotationXN}, {P::kButtonId_StickR_Right, kRotationXP},
	};
	// SDL face buttons are positional, so A/B and X/Y swap to match the Pro Controller's labels
	constexpr Layout kSDLGeneric[] = {
		{P::kButtonId_A, kButton1}, {P::kButtonId_B, kButton0}, {P::kButtonId_X, kButton3}, {P::kButtonId_Y, kButton2},
		{P::kButtonId_L, kButton9}, {P::kButtonId_R, kButton10}, {P::kButtonId_ZL, kTriggerXP}, {P::kButtonId_ZR, kTriggerYP},
		{P::kButtonId_Plus, kButton6}, {P::kButtonId_Minus, kButton4},
		{P::kButtonId_Up, kButton11}, {P::kButtonId_Down, kButton12}, {P::kButtonId_Left, kButton13}, {P::kButtonId_Right, kButton14},
		{P::kButtonId_StickL, kButton7}, {P::kButtonId_StickR, kButton8},
		{P::kButtonId_StickL_Up, kAxisYN}, {P::kButtonId_StickL_Down, kAxisYP}, {P::kButtonId_StickL_Left, kAxisXN}, {P::kButtonId_StickL_Right, kAxisXP},
		{P::kButtonId_StickR_Up, kRotationYN}, {P::kButtonId_StickR_Down, kRotationYP}, {P::kButtonId_StickR_Left, kRotationXN}, {P::kButtonId_StickR_Right, kRotationXP},
	};
#endif
	// XInput button ids are the bit numbers of XINPUT_GAMEPAD::wButtons; its y axes point up
	constexpr Layout kXInput[] = {
		{P::kButtonId_A, kButton13}, {P::kButtonId_B, kButton12}, {P::kButtonId_X, kButton15}, {P::kButtonId_Y, kButton14},
		{P::kButtonId_L, kButton8}, {P::kButtonId_R, kButton9}, {P::kButtonId_ZL, kTriggerXP}, {P::kButtonId_ZR, kTriggerYP},
		{P::kButtonId_Plus, kButton4}, {P::kButtonId_Minus, kButton5},
		{P::kButtonId_Up, kButton0}, {P::kButtonId_Down, kButton1}, {P::kButtonId_Left, kButton2}, {P::kButtonId_Right, kButton3},
		{P::kButtonId_StickL, kButton6}, {P::kButtonId_StickR, kButton7},
		{P::kButtonId_StickL_Up, kAxisYP}, {P::kButtonId_StickL_Down, kAxisYN}, {P::kButtonId_StickL_Left, kAxisXN}, {P::kButtonId_StickL_Right, kAxisXP},
		{P::kButtonId_StickR_Up, kRotationYP}, {P::kButtonId_StickR_Down, kRotationYN}, {P::kButtonId_StickR_Left, kRotationXN}, {P::kButtonId_StickR_Right, kRotationXP},
	};
}

ProController::ProController(size_t player_index)
	: WPADController(player_index, kDataFormat_URCC)
{
}

uint32 ProController::s_get_emulated_button_flag(uint32 id)
{
	const ButtonInfo* b = FindButton(id);
	return b ? b->flag : 0;
}

std::string_view ProController::get_button_name(ButtonId id)
{
	if (const ButtonInfo* b = FindButton(id))
		return b->name;
	cemu_assert_debug(false);
	return "";
}

// the Pro Controller's sticks are not limited to the unit circle
glm::vec2 ProController::get_axis() const
{
	return CombineDirections(get_axis_value(kButtonId_StickL_Left), get_axis_value(kButtonId_StickL_Right),
		get_axis_value(kButtonId_StickL_Up), get_axis_value(kButtonId_StickL_Down));
}

glm::vec2 ProController::get_rotation() const
{
	return CombineDirections(get_axis_value(kButtonId_StickR_Left), get_axis_value(kButtonId_StickR_Right),
		get_axis_value(kButtonId_StickR_Up), get_axis_value(kButtonId_StickR_Down));
}

glm::vec2 ProController::get_trigger() const
{
	return { get_axis_value(kButtonId_ZL), get_axis_value(kButtonId_ZR) };
}

bool ProController::set_default_mapping(const std::shared_ptr<ControllerBase>& controller)
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
