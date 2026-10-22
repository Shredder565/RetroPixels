// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "Inputs.h"

#include "Supermodel.h"
#include "InputSystem.h"
#include "InputTypes.h"
#include "Game.h"
#include "Util/Format.h"
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

template <class T>
std::shared_ptr<T> CInputs::Keep(std::shared_ptr<T> input)
{
	m_inputs.push_back(input);
	return input;
}

std::shared_ptr<CSwitchInput> CInputs::Switch(const char *id, const char *label, unsigned gameFlags, const char *defaultMapping)
{
	return Keep(std::make_shared<CSwitchInput>(id, label, gameFlags, defaultMapping));
}

std::shared_ptr<CAnalogInput> CInputs::Analog(const char *id, const char *label, unsigned gameFlags)
{
	return Keep(std::make_shared<CAnalogInput>(id, label, gameFlags, "NONE"));
}

std::shared_ptr<CAxisInput> CInputs::Axis(const char *id, const char *label, unsigned gameFlags,
	std::shared_ptr<CAnalogInput> neg, std::shared_ptr<CAnalogInput> pos, UINT16 minVal, UINT16 offVal, UINT16 maxVal)
{
	return Keep(std::make_shared<CAxisInput>(id, label, gameFlags, "NONE", neg, pos, minVal, offVal, maxVal));
}

// The ids are the ini names (Input<id>), so they must not change. Only the
// UI inputs have default mappings; the rest come from the ini file.
CInputs::CInputs(std::shared_ptr<CInputSystem> system)
	: m_system(system)
{
	const unsigned UI = Game::INPUT_UI;
	uiExit             = Switch("UIExit",             "Exit UI",               UI, "KEY_ESCAPE");
	uiReset            = Switch("UIReset",            "Reset",                 UI, "KEY_ALT+KEY_R");
	uiPause            = Switch("UIPause",            "Pause",                 UI, "KEY_ALT+KEY_P");
	uiFullScreen       = Switch("UIFullScreen",       "Toggle Fullscreen",     UI, "KEY_ALT+KEY_RETURN");
	uiSaveState        = Switch("UISaveState",        "Save State",            UI, "KEY_F5");
	uiChangeSlot       = Switch("UIChangeSlot",       "Change Save Slot",      UI, "KEY_F6");
	uiLoadState        = Switch("UILoadState",        "Load State",            UI, "KEY_F7");
	uiMusicVolUp       = Switch("UIMusicVolUp",       "Increase Music Volume", UI, "KEY_F10");
	uiMusicVolDown     = Switch("UIMusicVolDown",     "Decrease Music Volume", UI, "KEY_F9");
	uiSoundVolUp       = Switch("UISoundVolUp",       "Increase Sound Volume", UI, "KEY_F12");
	uiSoundVolDown     = Switch("UISoundVolDown",     "Decrease Sound Volume", UI, "KEY_F11");
	uiClearNVRAM       = Switch("UIClearNVRAM",       "Clear NVRAM",           UI, "KEY_ALT+KEY_N");
	uiSelectCrosshairs = Switch("UISelectCrosshairs", "Select Crosshairs",     UI, "KEY_ALT+KEY_I");
	uiToggleFrLimit    = Switch("UIToggleFrameLimit", "Toggle Frame Limiting", UI, "KEY_ALT+KEY_T");
	uiDumpInpState     = Switch("UIDumpInputState",   "Dump Input State",      UI, "KEY_ALT+KEY_U");
	uiDumpTimings      = Switch("UIDumpTimings",      "Dump Frame Timings",    UI, "KEY_ALT+KEY_O");
	uiScreenshot       = Switch("UIScreenShot",       "Screenshot",            UI, "KEY_ALT+KEY_S");

	const unsigned COMMON = Game::INPUT_COMMON;
	start[0]   = Switch("Start1",   "P1 Start",  COMMON);
	start[1]   = Switch("Start2",   "P2 Start",  COMMON);
	coin[0]    = Switch("Coin1",    "P1 Coin",   COMMON);
	coin[1]    = Switch("Coin2",    "P2 Coin",   COMMON);
	service[0] = Switch("ServiceA", "Service A", COMMON);
	service[1] = Switch("ServiceB", "Service B", COMMON);
	test[0]    = Switch("TestA",    "Test A",    COMMON);
	test[1]    = Switch("TestB",    "Test B",    COMMON);

	up[0]    = Switch("JoyUp",     "P1 Joystick Up",    Game::INPUT_JOYSTICK1);
	down[0]  = Switch("JoyDown",   "P1 Joystick Down",  Game::INPUT_JOYSTICK1);
	left[0]  = Switch("JoyLeft",   "P1 Joystick Left",  Game::INPUT_JOYSTICK1);
	right[0] = Switch("JoyRight",  "P1 Joystick Right", Game::INPUT_JOYSTICK1);
	up[1]    = Switch("JoyUp2",    "P2 Joystick Up",    Game::INPUT_JOYSTICK2);
	down[1]  = Switch("JoyDown2",  "P2 Joystick Down",  Game::INPUT_JOYSTICK2);
	left[1]  = Switch("JoyLeft2",  "P2 Joystick Left",  Game::INPUT_JOYSTICK2);
	right[1] = Switch("JoyRight2", "P2 Joystick Right", Game::INPUT_JOYSTICK2);

	const unsigned FIGHT = Game::INPUT_FIGHTING;
	punch[0]  = Switch("Punch",   "P1 Punch",  FIGHT);
	kick[0]   = Switch("Kick",    "P1 Kick",   FIGHT);
	guard[0]  = Switch("Guard",   "P1 Guard",  FIGHT);
	escape[0] = Switch("Escape",  "P1 Escape", FIGHT);
	punch[1]  = Switch("Punch2",  "P2 Punch",  FIGHT);
	kick[1]   = Switch("Kick2",   "P2 Kick",   FIGHT);
	guard[1]  = Switch("Guard2",  "P2 Guard",  FIGHT);
	escape[1] = Switch("Escape2", "P2 Escape", FIGHT);

	shift  = Switch("Shift",  "Shift",  Game::INPUT_SPIKEOUT);
	beat   = Switch("Beat",   "Beat",   Game::INPUT_SPIKEOUT);
	charge = Switch("Charge", "Charge", Game::INPUT_SPIKEOUT);
	jump   = Switch("Jump",   "Jump",   Game::INPUT_SPIKEOUT);

	const unsigned SOCCER = Game::INPUT_SOCCER;
	shortPass[0] = Switch("ShortPass",  "P1 Short Pass", SOCCER);
	longPass[0]  = Switch("LongPass",   "P1 Long Pass",  SOCCER);
	shoot[0]     = Switch("Shoot",      "P1 Shoot",      SOCCER);
	shortPass[1] = Switch("ShortPass2", "P2 Short Pass", SOCCER);
	longPass[1]  = Switch("LongPass2",  "P2 Long Pass",  SOCCER);
	shoot[1]     = Switch("Shoot2",     "P2 Shoot",      SOCCER);

	const unsigned CAR = Game::INPUT_VEHICLE;
	auto steerLeft  = Analog("SteeringLeft",  "Steer Left",  CAR);
	auto steerRight = Analog("SteeringRight", "Steer Right", CAR);
	steering      = Axis("Steering", "Full Steering", CAR, steerLeft, steerRight);
	accelerator   = Analog("Accelerator", "Accelerator Pedal",       CAR);
	brake         = Analog("Brake",       "Brake Pedal/Front Brake", CAR);
	gearShiftUp   = Switch("GearShiftUp",   "Shift Up",   CAR);
	gearShiftDown = Switch("GearShiftDown", "Shift Down", CAR);

	const unsigned SHIFT4 = Game::INPUT_SHIFT4;
	auto gear1 = Switch("GearShift1", "Shift 1",       SHIFT4);
	auto gear2 = Switch("GearShift2", "Shift 2",       SHIFT4);
	auto gear3 = Switch("GearShift3", "Shift 3",       SHIFT4);
	auto gear4 = Switch("GearShift4", "Shift 4",       SHIFT4);
	auto gearN = Switch("GearShiftN", "Shift Neutral", SHIFT4);
	gearShift4 = Keep(std::make_shared<CGearShift4Input>("GearShift", "Gear Shift", SHIFT4, gear1, gear2, gear3, gear4, gearN, gearShiftUp, gearShiftDown));

	vr[0] = Switch("VR1", "VR1", Game::INPUT_VR4);
	vr[1] = Switch("VR2", "VR2", Game::INPUT_VR4);
	vr[2] = Switch("VR3", "VR3", Game::INPUT_VR4);
	vr[3] = Switch("VR4", "VR4", Game::INPUT_VR4);

	viewChange = Switch("ViewChange", "View Change", Game::INPUT_VIEWCHANGE);
	handBrake  = Switch("HandBrake",  "Hand Brake",  Game::INPUT_HANDBRAKE);

	rearBrake   = Analog("RearBrake",   "Rear Brake",      Game::INPUT_HARLEY);
	musicSelect = Switch("MusicSelect", "Music Selection", Game::INPUT_HARLEY);

	const unsigned TWIN = Game::INPUT_TWIN_JOYSTICKS;
	twinJoyTurnLeft    = Switch("TwinJoyTurnLeft",    "Macro Turn Left",      TWIN);
	twinJoyTurnRight   = Switch("TwinJoyTurnRight",   "Macro Turn Right",     TWIN);
	twinJoyForward     = Switch("TwinJoyForward",     "Macro Forward",        TWIN);
	twinJoyReverse     = Switch("TwinJoyReverse",     "Macro Reverse",        TWIN);
	twinJoyStrafeLeft  = Switch("TwinJoyStrafeLeft",  "Macro Strafe Left",    TWIN);
	twinJoyStrafeRight = Switch("TwinJoyStrafeRight", "Macro Strafe Right",   TWIN);
	twinJoyJump        = Switch("TwinJoyJump",        "Macro Jump",           TWIN);
	twinJoyCrouch      = Switch("TwinJoyCrouch",      "Macro Crouch",         TWIN);
	twinJoyLeft1       = Switch("TwinJoyLeft1",       "Left Joystick Left",   TWIN);
	twinJoyLeft2       = Switch("TwinJoyLeft2",       "Right Joystick Left",  TWIN);
	twinJoyRight1      = Switch("TwinJoyRight1",      "Left Joystick Right",  TWIN);
	twinJoyRight2      = Switch("TwinJoyRight2",      "Right Joystick Right", TWIN);
	twinJoyUp1         = Switch("TwinJoyUp1",         "Left Joystick Up",     TWIN);
	twinJoyUp2         = Switch("TwinJoyUp2",         "Right Joystick Up",    TWIN);
	twinJoyDown1       = Switch("TwinJoyDown1",       "Left Joystick Down",   TWIN);
	twinJoyDown2       = Switch("TwinJoyDown2",       "Right Joystick Down",  TWIN);
	twinJoyShot1       = Switch("TwinJoyShot1",       "Left Shot Trigger",    TWIN);
	twinJoyShot2       = Switch("TwinJoyShot2",       "Right Shot Trigger",   TWIN);
	twinJoyTurbo1      = Switch("TwinJoyTurbo1",      "Left Turbo",           TWIN);
	twinJoyTurbo2      = Switch("TwinJoyTurbo2",      "Right Turbo",          TWIN);

	const unsigned AJOY = Game::INPUT_ANALOG_JOYSTICK;
	auto ajLeft  = Analog("AnalogJoyLeft",  "Analog Left",  AJOY);
	auto ajRight = Analog("AnalogJoyRight", "Analog Right", AJOY);
	auto ajUp    = Analog("AnalogJoyUp",    "Analog Up",    AJOY);
	auto ajDown  = Analog("AnalogJoyDown",  "Analog Down",  AJOY);
	analogJoyX        = Axis("AnalogJoyX", "Analog X-Axis", AJOY, ajLeft, ajRight);
	analogJoyY        = Axis("AnalogJoyY", "Analog Y-Axis", AJOY, ajUp, ajDown);
	analogJoyTrigger1 = Switch("AnalogJoyTrigger",  "Trigger Button 1", AJOY);
	analogJoyTrigger2 = Switch("AnalogJoyTrigger2", "Trigger Button 2", AJOY);
	analogJoyEvent1   = Switch("AnalogJoyEvent",    "Event Button 1",   AJOY);
	analogJoyEvent2   = Switch("AnalogJoyEvent2",   "Event Button 2",   AJOY);

	// Light guns: positions in the guns' own coordinates (X 150-651, Y 80-465).
	for (int p = 0; p < 2; p++)
	{
		const unsigned GUN = p == 0 ? Game::INPUT_GUN1 : Game::INPUT_GUN2;
		const bool p1 = p == 0;
		auto gl = Analog(p1 ? "GunLeft"  : "GunLeft2",  p1 ? "P1 Gun Left"  : "P2 Gun Left",  GUN);
		auto gr = Analog(p1 ? "GunRight" : "GunRight2", p1 ? "P1 Gun Right" : "P2 Gun Right", GUN);
		auto gu = Analog(p1 ? "GunUp"    : "GunUp2",    p1 ? "P1 Gun Up"    : "P2 Gun Up",    GUN);
		auto gd = Analog(p1 ? "GunDown"  : "GunDown2",  p1 ? "P1 Gun Down"  : "P2 Gun Down",  GUN);
		gunX[p] = Axis(p1 ? "GunX" : "GunX2", p1 ? "P1 Gun X-Axis" : "P2 Gun X-Axis", GUN, gl, gr, 150, 400, 651);
		gunY[p] = Axis(p1 ? "GunY" : "GunY2", p1 ? "P1 Gun Y-Axis" : "P2 Gun Y-Axis", GUN, gu, gd, 80, 272, 465);
		auto shot = Switch(p1 ? "Trigger"   : "Trigger2",   p1 ? "P1 Trigger"          : "P2 Trigger",          GUN);
		auto away = Switch(p1 ? "Offscreen" : "Offscreen2", p1 ? "P1 Point Off-screen" : "P2 Point Off-screen", GUN);
		trigger[p] = Keep(std::make_shared<CTriggerInput>(p1 ? "AutoTrigger" : "AutoTrigger2", p1 ? "P1 Auto Trigger" : "P2 Auto Trigger", GUN, shot, away));
	}

	// Analog guns: Y runs from 0xFF at the top down to 0.
	for (int p = 0; p < 2; p++)
	{
		const unsigned AGUN = p == 0 ? Game::INPUT_ANALOG_GUN1 : Game::INPUT_ANALOG_GUN2;
		const bool p1 = p == 0;
		auto gl = Analog(p1 ? "AnalogGunLeft"  : "AnalogGunLeft2",  p1 ? "P1 Analog Gun Left"  : "P2 Analog Gun Left",  AGUN);
		auto gr = Analog(p1 ? "AnalogGunRight" : "AnalogGunRight2", p1 ? "P1 Analog Gun Right" : "P2 Analog Gun Right", AGUN);
		auto gu = Analog(p1 ? "AnalogGunUp"    : "AnalogGunUp2",    p1 ? "P1 Analog Gun Up"    : "P2 Analog Gun Up",    AGUN);
		auto gd = Analog(p1 ? "AnalogGunDown"  : "AnalogGunDown2",  p1 ? "P1 Analog Gun Down"  : "P2 Analog Gun Down",  AGUN);
		analogGunX[p] = Axis(p1 ? "AnalogGunX" : "AnalogGunX2", p1 ? "P1 Analog Gun X-Axis" : "P2 Analog Gun X-Axis", AGUN, gl, gr, 0, 0x80, 0xFF);
		analogGunY[p] = Axis(p1 ? "AnalogGunY" : "AnalogGunY2", p1 ? "P1 Analog Gun Y-Axis" : "P2 Analog Gun Y-Axis", AGUN, gu, gd, 0xFF, 0x80, 0);
		analogTriggerLeft[p]  = Switch(p1 ? "AnalogTriggerLeft"  : "AnalogTriggerLeft2",  p1 ? "P1 Analog Gun Left Trigger"  : "P2 Analog Gun Left Trigger",  AGUN);
		analogTriggerRight[p] = Switch(p1 ? "AnalogTriggerRight" : "AnalogTriggerRight2", p1 ? "P1 Analog Gun Right Trigger" : "P2 Analog Gun Right Trigger", AGUN);
	}

	const unsigned SKI = Game::INPUT_SKI;
	auto skiLeft  = Analog("SkiLeft",  "Ski Champ Left",  SKI);
	auto skiRight = Analog("SkiRight", "Ski Champ Right", SKI);
	auto skiUp    = Analog("SkiUp",    "Ski Champ Up",    SKI);
	auto skiDown  = Analog("SkiDown",  "Ski Champ Down",  SKI);
	skiX         = Axis("SkiX", "Ski Champ X-Axis", SKI, skiLeft, skiRight, 0xFF, 0x80, 0);
	skiY         = Axis("SkiY", "Ski Champ Y-Axis", SKI, skiUp, skiDown);
	skiPollLeft  = Switch("SkiPollLeft",  "Ski Champ Poll Left",  SKI);
	skiPollRight = Switch("SkiPollRight", "Ski Champ Poll Right", SKI);
	skiSelect1   = Switch("SkiSelect1",   "Ski Champ Select 1",   SKI);
	skiSelect2   = Switch("SkiSelect2",   "Ski Champ Select 2",   SKI);
	skiSelect3   = Switch("SkiSelect3",   "Ski Champ Select 3",   SKI);

	const unsigned TRUCK = Game::INPUT_MAGTRUCK;
	auto lever1Up   = Analog("MagicalLeverUp1",   "P1 Magical Lever Up",   TRUCK);
	auto lever1Down = Analog("MagicalLeverDown1", "P1 Magical Lever Down", TRUCK);
	auto lever2Up   = Analog("MagicalLeverUp2",   "P2 Magical Lever Up",   TRUCK);
	auto lever2Down = Analog("MagicalLeverDown2", "P2 Magical Lever Down", TRUCK);
	magicalLever1 = Axis("MagicalLever1", "P1 Magical Lever", TRUCK, lever1Up, lever1Down, 0xFF, 0x80, 0);
	magicalLever2 = Axis("MagicalLever2", "P2 Magical Lever", TRUCK, lever2Up, lever2Down, 0xFF, 0x80, 0);
	magicalPedal1 = Switch("MagicalPedal1", "P1 Magical Pedal", TRUCK);
	magicalPedal2 = Switch("MagicalPedal2", "P2 Magical Pedal", TRUCK);

	const unsigned FISH = Game::INPUT_FISHING;
	auto rodLeft    = Analog("FishingRodLeft",    "Rod Left",    FISH);
	auto rodRight   = Analog("FishingRodRight",   "Rod Right",   FISH);
	auto rodUp      = Analog("FishingRodUp",      "Rod Up",      FISH);
	auto rodDown    = Analog("FishingRodDown",    "Rod Down",    FISH);
	auto stickLeft  = Analog("FishingStickLeft",  "Stick Left",  FISH);
	auto stickRight = Analog("FishingStickRight", "Stick Right", FISH);
	auto stickUp    = Analog("FishingStickUp",    "Stick Up",    FISH);
	auto stickDown  = Analog("FishingStickDown",  "Stick Down",  FISH);
	fishingRodX    = Axis("FishingRodX", "Rod X-Axis", FISH, rodLeft, rodRight);
	fishingRodY    = Axis("FishingRodY", "Rod Y-Axis", FISH, rodUp, rodDown);
	fishingReel    = Analog("FishingReel", "Reel Speed", FISH);
	fishingStickX  = Axis("FishingStickX", "Stick X-Axis", FISH, stickLeft, stickRight);
	fishingStickY  = Axis("FishingStickY", "Stick Y-Axis", FISH, stickUp, stickDown);
	fishingCast    = Switch("FishingCast",   "Cast",   FISH);
	fishingSelect  = Switch("FishingSelect", "Select", FISH);
	fishingTension = Analog("FishingTension", "Tension", FISH);  // Get Bass only
}

CInputs::~CInputs()
{
}

void CInputs::PrintHeader(const char *fmt, ...)
{
	char text[1024];
	va_list vl;
	va_start(vl, fmt);
	vsnprintf(text, sizeof(text), fmt, vl);
	va_end(vl);
	puts(text);
	puts(std::string(strlen(text), '-').c_str());
	putchar('\n');
}

void CInputs::PrintConfigureInputsHelp()
{
	static const char *const lines[] =
	{
		"For each control, use the following keys to map the inputs:",
		"",
		"  Return  Set current input mapping and move to next control",
		"  c       Clear current input mapping and remain there",
		"  s       Set the current input mapping and remain there",
		"  a       Append to current input mapping (for multiple assignments)",
		"          and remain there",
		"  r       Reset current input mapping to default and remain there",
		"  Down    Move onto next control",
		"  Up      Go back to previous control",
		"  b       Calibrate joystick axes",
		"  i       Display information about input system and attached devices",
		"  h       Display this help again",
		"  q       Finish and save all changes",
		"  Esc     Finish without saving any changes",
		"",
		"To assign input(s), simply press the appropriate key(s), mouse button(s),",
		"joystick button(s), or move the mouse along one or more axes, or move a",
		"joystick's axis or POV hat controller(s).  The input(s) will be accepted",
		"as soon as all pressed keys and buttons are released and all moved mouse",
		"and joystick controllers are returned to their rest positions.",
		"",
		"Notes:",
		" - In order to assign keys the configuration window must on top.",
		" - In order to assign mouse buttons the mouse must be clicked within the",
		"   window.",
		" - In order to assign mouse axes, the cursor must first be placed in the",
		"   center of the window and then moved in the corresponding direction to the",
		"   window's edge and then returned to the center.",
		""
	};
	for (const char *line : lines)
		puts(line);
}

unsigned CInputs::Count()
{
	return unsigned(m_inputs.size());
}

std::shared_ptr<CInput> CInputs::operator[](const unsigned index)
{
	return m_inputs[index];
}

std::shared_ptr<CInput> CInputs::operator[](const char *idOrLabel)
{
	for (auto &input : m_inputs)
		if (Util::Stricmp(input->id, idOrLabel) == 0 || Util::Stricmp(input->label, idOrLabel) == 0)
			return input;
	return nullptr;
}

std::shared_ptr<CInputSystem> CInputs::GetInputSystem()
{
	return m_system;
}

bool CInputs::Initialize()
{
	if (!m_system->Initialize())
		return false;
	for (auto &input : m_inputs)
		input->Initialize(m_system);
	return true;
}

void CInputs::LoadFromConfig(const Util::Config::Node &config)
{
	m_system->LoadFromConfig(config);
	for (auto &input : m_inputs)
		input->LoadFromConfig(config);
}

void CInputs::StoreToConfig(Util::Config::Node *config)
{
	m_system->StoreToConfig(config);
	for (auto &input : m_inputs)
		input->StoreToConfig(config);
}

std::vector<std::shared_ptr<CInput>> CInputs::InputsFor(uint32_t gameFlags)
{
	std::vector<std::shared_ptr<CInput>> list;
	for (auto &input : m_inputs)
		if (input->IsConfigurable() && (input->gameFlags & gameFlags))
			list.push_back(input);
	return list;
}

std::vector<std::shared_ptr<CInput>> CInputs::GetGameInputs(const Game &game)
{
	return InputsFor(game.name.empty() ? uint32_t(Game::INPUT_ALL) : game.inputs);
}

// Reads one key from the keyboard for the console menus; false on Esc (the
// UI exit mapping) or when the window closes.
static bool ReadMenuKey(CInputSystem &system, const char *escape, char (&key)[50])
{
	return system.ReadMapping(key, sizeof(key), false, READ_KEYBOARD | READ_MERGE, escape);
}

// The digit of a single "KEY_<digit>" press, or -1.
static int MenuDigit(const char *key)
{
	if (strlen(key) != 5 || strncmp(key, "KEY_", 4) != 0 || !isdigit((unsigned char)key[4]))
		return -1;
	return key[4] - '0';
}

bool CInputs::ConfigureInputs(const Game &game)
{
	m_system->UngrabMouse();

	uint32_t gameFlags = Game::INPUT_ALL;
	if (game.name.empty())
		PrintHeader("Configure Inputs");
	else
	{
		PrintHeader("Configure Inputs for '%s'", game.title.c_str());
		gameFlags = game.inputs;
	}
	PrintConfigureInputsHelp();

	std::vector<std::shared_ptr<CInput>> inputs = InputsFor(gameFlags);
	std::vector<std::string> before;
	for (auto &input : inputs)
		before.push_back(input->GetMapping());

	enum class Step { Stay, Move, Finish, Cancel };
	const char *shownGroup = nullptr;
	Step step = Step::Move;
	size_t at = 0;
	while (at < inputs.size() && step != Step::Finish && step != Step::Cancel)
	{
		CInput &input = *inputs[at];
		const char *group = input.GetInputGroup();
		if (!shownGroup || Util::Stricmp(shownGroup, group) != 0)
		{
			shownGroup = group;
			printf("%s:\n", group);
		}

		// Show the prompt, then act on keys until one moves on (Move), asks
		// for the prompt again (Stay) or ends the configuration.
		do
		{
			if (at > 0)
				printf(" %s [%s]: Ret/c/s/a/r/Up/Down/b/h/q/Esc? ", input.label, input.GetMapping());
			else
				printf(" %s [%s]: Ret/c/s/a/r/Down/h/b/q/Esc? ", input.label, input.GetMapping());
			fflush(stdout);

			step = Step::Stay;
			bool waiting = true;
			while (waiting)
			{
				char key[50];
				if (!ReadMenuKey(*m_system, uiExit->GetMapping(), key))
				{
					step = Step::Cancel;
					break;
				}
				waiting = false;
				auto is = [&](const char *name) { return Util::Stricmp(key, name) == 0; };
				if (is("KEY_RETURN") || is("KEY_S"))
				{
					printf("Setting... ");
					fflush(stdout);
					if (input.Configure(false, uiExit->GetMapping()))
					{
						puts(input.GetMapping());
						if (is("KEY_RETURN"))
							at++;
						step = Step::Move;
					}
					else
						puts("[Cancelled]");
				}
				else if (is("KEY_A"))
				{
					printf("Appending... ");
					fflush(stdout);
					puts(input.Configure(true, uiExit->GetMapping()) ? input.GetMapping() : "[Cancelled]");
				}
				else if (is("KEY_C"))
				{
					input.SetMapping("NONE");
					puts("Cleared");
				}
				else if (is("KEY_R"))
				{
					input.ResetToDefaultMapping();
					puts("Reset");
				}
				else if (is("KEY_DOWN"))
				{
					puts("");
					at++;
					step = Step::Move;
				}
				else if (is("KEY_UP") && at > 0)
				{
					puts("");
					at--;
					step = Step::Move;
				}
				else if (is("KEY_HOME"))
				{
					puts("");
					at = 0;
					step = Step::Move;
				}
				else if (is("KEY_B"))
				{
					printf("\n\n");
					CalibrateJoysticks();
					puts("");
				}
				else if (is("KEY_I"))
				{
					printf("\n\n");
					m_system->PrintSettings();
				}
				else if (is("KEY_H"))
				{
					printf("\n\n");
					PrintConfigureInputsHelp();
				}
				else if (is("KEY_Q"))
					step = Step::Finish;
				else
					waiting = true;  // not a menu key: keep waiting
			}
		} while (step == Step::Stay);
	}

	if (step == Step::Cancel)
	{
		for (size_t i = 0; i < inputs.size(); i++)
			inputs[i]->SetMapping(before[i].c_str());
	}

	printf("\n\n");
	m_system->GrabMouse();
	return step != Step::Cancel;
}

bool CInputs::ConfigureInputs(const Game &game, unsigned dispX, unsigned dispY, unsigned dispW, unsigned dispH)
{
	m_system->SetDisplayGeom(dispX, dispY, dispW, dispH);
	return ConfigureInputs(game);
}

void CInputs::CalibrateJoysticks()
{
	int count = m_system->GetNumJoysticks();
	if (count == 0 || count == ANY_JOYSTICK)
	{
		puts("No joysticks attached to calibrate!");
		return;
	}
	puts("Choose joystick to calibrate (or press Esc to cancel):");
	// Keys 1-9 and then 0 choose among the first ten.
	if (count > 10)
		count = 10;
	for (int i = 0; i < count; i++)
		printf(" %u: %s\n", unsigned((i + 1) % 10), m_system->GetJoyDetails(i)->name);

	char key[50];
	while (ReadMenuKey(*m_system, uiExit->GetMapping(), key))
	{
		int digit = MenuDigit(key);
		if (digit < 0)
			continue;
		int joyNum = digit == 0 ? 9 : digit - 1;
		if (joyNum >= count)
			continue;
		puts("");
		CalibrateJoystick(joyNum);
		return;
	}
}

void CInputs::CalibrateJoystick(int joyNum)
{
	const JoyDetails *joy = m_system->GetJoyDetails(joyNum);
	if (!joy || joy->numAxes == 0)
	{
		puts("No axes available to calibrate on joystick!");
		return;
	}
	printf("Calibrating joystick '%s'.\n\n", joy->name);
	puts("Choose axis to calibrate (or press Esc to cancel):");
	std::vector<unsigned> choices;
	for (unsigned axis = 0; axis < NUM_JOY_AXES; axis++)
	{
		if (joy->hasAxis[axis])
		{
			choices.push_back(axis);
			printf(" %u: %s\n", unsigned(choices.size()), joy->axisName[axis]);
		}
	}
	printf(" 0: Unsure - help me choose...\n");

	char key[50];
	while (ReadMenuKey(*m_system, uiExit->GetMapping(), key))
	{
		unsigned axis;
		if (Util::Stricmp(key, "KEY_0") == 0)
		{
			puts("");
			if (!m_system->DetectJoystickAxis(joyNum, axis))
				return;
		}
		else
		{
			int digit = MenuDigit(key);
			if (digit < 1 || unsigned(digit) > choices.size())
				continue;
			axis = choices[digit - 1];
		}
		puts("");
		if (m_system->CalibrateJoystickAxis(joyNum, axis))
		{
			for (auto &input : m_inputs)
				input->InputSystemChanged();
		}
		return;
	}
}

// Prints the group heading when it changes between consecutive inputs.
static void PrintGroupChange(const char *&shown, CInput &input)
{
	const char *group = input.GetInputGroup();
	if (!shown || Util::Stricmp(shown, group) != 0)
	{
		shown = group;
		printf("%s:\n", group);
	}
}

void CInputs::PrintInputs(const Game *game)
{
	uint32_t gameFlags = Game::INPUT_ALL;
	if (game)
	{
		PrintHeader("Input Assignments for %s", game->title.c_str());
		gameFlags = game->inputs;
	}
	else
		PrintHeader("Input Assignments");

	const char *shown = nullptr;
	for (auto &input : InputsFor(gameFlags))
	{
		PrintGroupChange(shown, *input);
		printf(" %s = %s\n", input->label, input->GetMapping());
	}
	puts("");
}

bool CInputs::Poll(const Game *game, unsigned dispX, unsigned dispY, unsigned dispW, unsigned dispH)
{
	m_system->SetDisplayGeom(dispX, dispY, dispW, dispH);
	if (!m_system->Poll())
		return false;
	uint32_t gameFlags = game ? game->inputs : uint32_t(Game::INPUT_ALL);
	for (auto &input : m_inputs)
		if (input->IsUIInput() || (input->gameFlags & gameFlags))
			input->Poll();
	return true;
}

void CInputs::DumpState(const Game *game)
{
	uint32_t gameFlags = Game::INPUT_ALL;
	if (game)
	{
		PrintHeader("Input States for %s", game->title.c_str());
		gameFlags = game->inputs;
	}
	else
		PrintHeader("Input States");

	for (auto &input : m_inputs)
	{
		if (!input->IsUIInput() && !(input->gameFlags & gameFlags))
			continue;
		if (input->IsVirtual())
			printf("%s = (%d)\n", input->id, input->value);
		else
			printf("%s [%s] = (%d)\n", input->id, input->GetMapping(), input->value);
	}
}
