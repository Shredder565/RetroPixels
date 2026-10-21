// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HotkeyManager.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"

namespace
{
// --- What each hotkey is called --------------------------------------------------

struct HotkeyName
{
  Hotkey id;
  const char* label;
};

// One entry per hotkey, in enum order (checked below). Labels are shown in the
// mapping UI and are also the control names in Hotkeys.ini.
constexpr HotkeyName HOTKEY_NAMES[] = {
    {HK_OPEN, _trans("Open")},
    {HK_CHANGE_DISC, _trans("Change Disc")},
    {HK_EJECT_DISC, _trans("Eject Disc")},
    {HK_REFRESH_LIST, _trans("Refresh Game List")},
    {HK_PLAY_PAUSE, _trans("Toggle Pause")},
    {HK_STOP, _trans("Stop")},
    {HK_RESET, _trans("Reset")},
    {HK_FULLSCREEN, _trans("Toggle Fullscreen")},
    {HK_SCREENSHOT, _trans("Take Screenshot")},
    {HK_EXIT, _trans("Exit")},
    {HK_UNLOCK_CURSOR, _trans("Unlock Cursor")},
    {HK_CENTER_MOUSE, _trans("Center Mouse")},
    {HK_ACTIVATE_CHAT, _trans("Activate NetPlay Chat")},
    {HK_REQUEST_GOLF_CONTROL, _trans("Control NetPlay Golf Mode")},
#ifdef USE_RETRO_ACHIEVEMENTS
    {HK_OPEN_ACHIEVEMENTS, _trans("Open Achievements")},
#endif

    {HK_VOLUME_DOWN, _trans("Volume Down")},
    {HK_VOLUME_UP, _trans("Volume Up")},
    {HK_VOLUME_TOGGLE_MUTE, _trans("Volume Toggle Mute")},

    {HK_DECREASE_EMULATION_SPEED, _trans("Decrease Emulation Speed")},
    {HK_INCREASE_EMULATION_SPEED, _trans("Increase Emulation Speed")},
    {HK_TOGGLE_THROTTLE, _trans("Disable Emulation Speed Limit")},

    {HK_FRAME_ADVANCE, _trans("Frame Advance")},
    {HK_FRAME_ADVANCE_DECREASE_SPEED, _trans("Frame Advance Decrease Speed")},
    {HK_FRAME_ADVANCE_INCREASE_SPEED, _trans("Frame Advance Increase Speed")},
    {HK_FRAME_ADVANCE_RESET_SPEED, _trans("Frame Advance Reset Speed")},

    {HK_START_RECORDING, _trans("Start Recording")},
    {HK_PLAY_RECORDING, _trans("Play Recording")},
    {HK_EXPORT_RECORDING, _trans("Export Recording")},
    {HK_READ_ONLY_MODE, _trans("Read-Only Mode")},

    // i18n: Here, "Step" is a verb. This feature is used for
    // going through code step by step.
    {HK_STEP, _trans("Step Into")},
    // i18n: Here, "Step" is a verb. This feature is used for
    // going through code step by step.
    {HK_STEP_OVER, _trans("Step Over")},
    // i18n: Here, "Step" is a verb. This feature is used for
    // going through code step by step.
    {HK_STEP_OUT, _trans("Step Out")},
    {HK_SKIP, _trans("Skip")},

    // i18n: Here, PC is an acronym for program counter, not personal computer.
    {HK_SHOW_PC, _trans("Show PC")},
    // i18n: Here, PC is an acronym for program counter, not personal computer.
    {HK_SET_PC, _trans("Set PC")},

    {HK_BP_TOGGLE, _trans("Toggle Breakpoint")},
    {HK_BP_ADD, _trans("Add a Breakpoint")},
    {HK_MBP_ADD, _trans("Add a Memory Breakpoint")},

    {HK_TRIGGER_SYNC_BUTTON, _trans("Press Sync Button")},
    {HK_WIIMOTE1_CONNECT, _trans("Connect Wii Remote 1")},
    {HK_WIIMOTE2_CONNECT, _trans("Connect Wii Remote 2")},
    {HK_WIIMOTE3_CONNECT, _trans("Connect Wii Remote 3")},
    {HK_WIIMOTE4_CONNECT, _trans("Connect Wii Remote 4")},
    {HK_BALANCEBOARD_CONNECT, _trans("Connect Balance Board")},
    {HK_TOGGLE_SD_CARD, _trans("Toggle SD Card")},
    {HK_TOGGLE_USB_KEYBOARD, _trans("Toggle USB Keyboard")},
    {HK_TOGGLE_WII_SPEAK_MUTE, _trans("Toggle Wii Speak Mute")},

    {HK_NEXT_WIIMOTE_PROFILE_1, _trans("Next Profile")},
    {HK_PREV_WIIMOTE_PROFILE_1, _trans("Previous Profile")},
    {HK_NEXT_GAME_WIIMOTE_PROFILE_1, _trans("Next Game Profile")},
    {HK_PREV_GAME_WIIMOTE_PROFILE_1, _trans("Previous Game Profile")},
    {HK_NEXT_WIIMOTE_PROFILE_2, _trans("Next Profile")},
    {HK_PREV_WIIMOTE_PROFILE_2, _trans("Previous Profile")},
    {HK_NEXT_GAME_WIIMOTE_PROFILE_2, _trans("Next Game Profile")},
    {HK_PREV_GAME_WIIMOTE_PROFILE_2, _trans("Previous Game Profile")},
    {HK_NEXT_WIIMOTE_PROFILE_3, _trans("Next Profile")},
    {HK_PREV_WIIMOTE_PROFILE_3, _trans("Previous Profile")},
    {HK_NEXT_GAME_WIIMOTE_PROFILE_3, _trans("Next Game Profile")},
    {HK_PREV_GAME_WIIMOTE_PROFILE_3, _trans("Previous Game Profile")},
    {HK_NEXT_WIIMOTE_PROFILE_4, _trans("Next Profile")},
    {HK_PREV_WIIMOTE_PROFILE_4, _trans("Previous Profile")},
    {HK_NEXT_GAME_WIIMOTE_PROFILE_4, _trans("Next Game Profile")},
    {HK_PREV_GAME_WIIMOTE_PROFILE_4, _trans("Previous Game Profile")},

    {HK_TOGGLE_CROP_TO_ASPECT_RATIO, _trans("Toggle Aspect Ratio Crop")},
    {HK_TOGGLE_CROP_CUSTOM, _trans("Toggle Custom Crop")},
    {HK_TOGGLE_AR, _trans("Toggle Aspect Ratio")},
    {HK_TOGGLE_SKIP_EFB_ACCESS, _trans("Toggle Skip EFB Access")},
    {HK_TOGGLE_EFBCOPIES, _trans("Toggle EFB Copies")},
    {HK_TOGGLE_XFBCOPIES, _trans("Toggle XFB Copies")},
    {HK_TOGGLE_IMMEDIATE_XFB, _trans("Toggle XFB Immediate Mode")},
    {HK_TOGGLE_FOG, _trans("Toggle Fog")},
    {HK_TOGGLE_DUMPTEXTURES, _trans("Toggle Texture Dumping")},
    {HK_TOGGLE_TEXTURES, _trans("Toggle Custom Textures")},

    // i18n: IR stands for internal resolution
    {HK_INCREASE_IR, _trans("Increase IR")},
    // i18n: IR stands for internal resolution
    {HK_DECREASE_IR, _trans("Decrease IR")},

    {HK_FREELOOK_TOGGLE, _trans("Freelook Toggle")},

    {HK_TOGGLE_STEREO_SIDE_BY_SIDE, _trans("Toggle 3D Side-by-Side")},
    {HK_TOGGLE_STEREO_TOP_AND_BOTTOM, _trans("Toggle 3D Top-Bottom")},
    {HK_TOGGLE_STEREO_ANAGLYPH, _trans("Toggle 3D Anaglyph")},
    {HK_DECREASE_DEPTH, _trans("Decrease Depth")},
    {HK_INCREASE_DEPTH, _trans("Increase Depth")},
    {HK_DECREASE_CONVERGENCE, _trans("Decrease Convergence")},
    {HK_INCREASE_CONVERGENCE, _trans("Increase Convergence")},

    {HK_LOAD_STATE_SLOT_1, _trans("Load State Slot 1")},
    {HK_LOAD_STATE_SLOT_2, _trans("Load State Slot 2")},
    {HK_LOAD_STATE_SLOT_3, _trans("Load State Slot 3")},
    {HK_LOAD_STATE_SLOT_4, _trans("Load State Slot 4")},
    {HK_LOAD_STATE_SLOT_5, _trans("Load State Slot 5")},
    {HK_LOAD_STATE_SLOT_6, _trans("Load State Slot 6")},
    {HK_LOAD_STATE_SLOT_7, _trans("Load State Slot 7")},
    {HK_LOAD_STATE_SLOT_8, _trans("Load State Slot 8")},
    {HK_LOAD_STATE_SLOT_9, _trans("Load State Slot 9")},
    {HK_LOAD_STATE_SLOT_10, _trans("Load State Slot 10")},
    {HK_LOAD_STATE_SLOT_SELECTED, _trans("Load from Selected Slot")},

    {HK_SAVE_STATE_SLOT_1, _trans("Save State Slot 1")},
    {HK_SAVE_STATE_SLOT_2, _trans("Save State Slot 2")},
    {HK_SAVE_STATE_SLOT_3, _trans("Save State Slot 3")},
    {HK_SAVE_STATE_SLOT_4, _trans("Save State Slot 4")},
    {HK_SAVE_STATE_SLOT_5, _trans("Save State Slot 5")},
    {HK_SAVE_STATE_SLOT_6, _trans("Save State Slot 6")},
    {HK_SAVE_STATE_SLOT_7, _trans("Save State Slot 7")},
    {HK_SAVE_STATE_SLOT_8, _trans("Save State Slot 8")},
    {HK_SAVE_STATE_SLOT_9, _trans("Save State Slot 9")},
    {HK_SAVE_STATE_SLOT_10, _trans("Save State Slot 10")},
    {HK_SAVE_STATE_SLOT_SELECTED, _trans("Save to Selected Slot")},

    {HK_SELECT_STATE_SLOT_1, _trans("Select State Slot 1")},
    {HK_SELECT_STATE_SLOT_2, _trans("Select State Slot 2")},
    {HK_SELECT_STATE_SLOT_3, _trans("Select State Slot 3")},
    {HK_SELECT_STATE_SLOT_4, _trans("Select State Slot 4")},
    {HK_SELECT_STATE_SLOT_5, _trans("Select State Slot 5")},
    {HK_SELECT_STATE_SLOT_6, _trans("Select State Slot 6")},
    {HK_SELECT_STATE_SLOT_7, _trans("Select State Slot 7")},
    {HK_SELECT_STATE_SLOT_8, _trans("Select State Slot 8")},
    {HK_SELECT_STATE_SLOT_9, _trans("Select State Slot 9")},
    {HK_SELECT_STATE_SLOT_10, _trans("Select State Slot 10")},

    {HK_LOAD_LAST_STATE_1, _trans("Load State Last 1")},
    {HK_LOAD_LAST_STATE_2, _trans("Load State Last 2")},
    {HK_LOAD_LAST_STATE_3, _trans("Load State Last 3")},
    {HK_LOAD_LAST_STATE_4, _trans("Load State Last 4")},
    {HK_LOAD_LAST_STATE_5, _trans("Load State Last 5")},
    {HK_LOAD_LAST_STATE_6, _trans("Load State Last 6")},
    {HK_LOAD_LAST_STATE_7, _trans("Load State Last 7")},
    {HK_LOAD_LAST_STATE_8, _trans("Load State Last 8")},
    {HK_LOAD_LAST_STATE_9, _trans("Load State Last 9")},
    {HK_LOAD_LAST_STATE_10, _trans("Load State Last 10")},

    {HK_SAVE_FIRST_STATE, _trans("Save Oldest State")},
    {HK_UNDO_LOAD_STATE, _trans("Undo Load State")},
    {HK_UNDO_SAVE_STATE, _trans("Undo Save State")},
    {HK_SAVE_STATE_FILE, _trans("Save State")},
    {HK_LOAD_STATE_FILE, _trans("Load State")},
    {HK_INCREMENT_SELECTED_STATE_SLOT, _trans("Increase Selected State Slot")},
    {HK_DECREMENT_SELECTED_STATE_SLOT, _trans("Decrease Selected State Slot")},

    {HK_GBA_LOAD, _trans("Load ROM")},
    {HK_GBA_UNLOAD, _trans("Unload ROM")},
    {HK_GBA_RESET, _trans("Reset")},

    {HK_GBA_VOLUME_DOWN, _trans("Volume Down")},
    {HK_GBA_VOLUME_UP, _trans("Volume Up")},
    {HK_GBA_TOGGLE_MUTE, _trans("Volume Toggle Mute")},
    {HK_GBA_1X, _trans("1x")},
    {HK_GBA_2X, _trans("2x")},
    {HK_GBA_3X, _trans("3x")},
    {HK_GBA_4X, _trans("4x")},

    {HK_SKYLANDERS_PORTAL, _trans("Show Skylanders Portal")},
    {HK_INFINITY_BASE, _trans("Show Infinity Base")},
};

constexpr bool NamesCoverEveryHotkeyInOrder()
{
  if (std::size(HOTKEY_NAMES) != NUM_HOTKEYS)
    return false;
  for (size_t i = 0; i < std::size(HOTKEY_NAMES); ++i)
  {
    if (static_cast<size_t>(HOTKEY_NAMES[i].id) != i)
      return false;
  }
  return true;
}
static_assert(NamesCoverEveryHotkeyInOrder(), "HOTKEY_NAMES must list every hotkey in enum order");

// --- How hotkeys are grouped -------------------------------------------------------

struct GroupRange
{
  const char* title;
  Hotkey first;
  Hotkey last;
  // GBA window hotkeys are sampled separately, without the render-window focus rule.
  bool sampled_without_focus = false;
};

constexpr std::array<GroupRange, NUM_HOTKEY_GROUPS> GROUP_RANGES{{
#ifdef USE_RETRO_ACHIEVEMENTS
    {_trans("General"), HK_OPEN, HK_OPEN_ACHIEVEMENTS},
#else
    {_trans("General"), HK_OPEN, HK_REQUEST_GOLF_CONTROL},
#endif
    {_trans("Volume"), HK_VOLUME_DOWN, HK_VOLUME_TOGGLE_MUTE},
    {_trans("Emulation Speed"), HK_DECREASE_EMULATION_SPEED, HK_TOGGLE_THROTTLE},
    {_trans("Frame Advance"), HK_FRAME_ADVANCE, HK_FRAME_ADVANCE_RESET_SPEED},
    {_trans("Movie"), HK_START_RECORDING, HK_READ_ONLY_MODE},
    {_trans("Stepping"), HK_STEP, HK_SKIP},
    {_trans("Program Counter"), HK_SHOW_PC, HK_SET_PC},
    {_trans("Breakpoint"), HK_BP_TOGGLE, HK_MBP_ADD},
    {_trans("Wii"), HK_TRIGGER_SYNC_BUTTON, HK_TOGGLE_WII_SPEAK_MUTE},
    {_trans("Controller Profile 1"), HK_NEXT_WIIMOTE_PROFILE_1, HK_PREV_GAME_WIIMOTE_PROFILE_1},
    {_trans("Controller Profile 2"), HK_NEXT_WIIMOTE_PROFILE_2, HK_PREV_GAME_WIIMOTE_PROFILE_2},
    {_trans("Controller Profile 3"), HK_NEXT_WIIMOTE_PROFILE_3, HK_PREV_GAME_WIIMOTE_PROFILE_3},
    {_trans("Controller Profile 4"), HK_NEXT_WIIMOTE_PROFILE_4, HK_PREV_GAME_WIIMOTE_PROFILE_4},
    {_trans("Graphics Toggles"), HK_TOGGLE_CROP_TO_ASPECT_RATIO, HK_TOGGLE_TEXTURES},
    {_trans("Internal Resolution"), HK_INCREASE_IR, HK_DECREASE_IR},
    {_trans("Freelook"), HK_FREELOOK_TOGGLE, HK_FREELOOK_TOGGLE},
    // i18n: Stereoscopic 3D
    {_trans("3D"), HK_TOGGLE_STEREO_SIDE_BY_SIDE, HK_TOGGLE_STEREO_ANAGLYPH},
    // i18n: Stereoscopic 3D
    {_trans("3D Depth"), HK_DECREASE_DEPTH, HK_INCREASE_CONVERGENCE},
    {_trans("Load State"), HK_LOAD_STATE_SLOT_1, HK_LOAD_STATE_SLOT_SELECTED},
    {_trans("Save State"), HK_SAVE_STATE_SLOT_1, HK_SAVE_STATE_SLOT_SELECTED},
    {_trans("Select State"), HK_SELECT_STATE_SLOT_1, HK_SELECT_STATE_SLOT_10},
    {_trans("Load Last State"), HK_LOAD_LAST_STATE_1, HK_LOAD_LAST_STATE_10},
    {_trans("Other State Hotkeys"), HK_SAVE_FIRST_STATE, HK_DECREMENT_SELECTED_STATE_SLOT},
    {_trans("GBA Core"), HK_GBA_LOAD, HK_GBA_RESET, true},
    {_trans("GBA Volume"), HK_GBA_VOLUME_DOWN, HK_GBA_TOGGLE_MUTE, true},
    {_trans("GBA Window Size"), HK_GBA_1X, HK_GBA_4X, true},
    {_trans("USB Emulation Devices"), HK_SKYLANDERS_PORTAL, HK_INFINITY_BASE},
}};

// A group's hotkeys are one bit each in a u32.
constexpr size_t MAX_KEYS_PER_GROUP = 32;

size_t GroupSize(const GroupRange& range)
{
  return static_cast<size_t>(range.last - range.first) + 1;
}

// "@(Ctrl+O)": keys held together.
std::string Chord(const std::vector<std::string>& keys)
{
  return fmt::format("@({})", fmt::join(keys, "+"));
}

// --- Shared state ---------------------------------------------------------------

InputConfig s_input_config("Hotkeys", _trans("Hotkeys"), "Hotkeys", "Hotkeys");

// The last sample, and which keys were already down at the previous IsPressed check.
HotkeyStatus s_sample;
std::array<u32, NUM_HOTKEY_GROUPS> s_seen_down;
bool s_enabled;

HotkeyManager& TheManager()
{
  return *static_cast<HotkeyManager*>(s_input_config.GetController(0));
}
}  // namespace

// --- HotkeyManager -----------------------------------------------------------------

HotkeyManager::HotkeyManager()
{
  for (size_t group = 0; group < GROUP_RANGES.size(); ++group)
  {
    const GroupRange& range = GROUP_RANGES[group];
    auto* const buttons = new ControllerEmu::Buttons(range.title);
    m_group_buttons[group] = buttons;
    groups.emplace_back(buttons);

    for (int hotkey = range.first; hotkey <= range.last; ++hotkey)
    {
      buttons->AddInput(ControllerEmu::Translatability::Translate,
                        HOTKEY_NAMES[static_cast<size_t>(hotkey)].label);
    }
  }
}

HotkeyManager::~HotkeyManager() = default;

std::string HotkeyManager::GetName() const
{
  return "Hotkeys";
}

InputConfig* HotkeyManager::GetConfig() const
{
  return HotkeyManagerEmu::GetConfig();
}

void HotkeyManager::GetInput(HotkeyStatus* kb, bool ignore_focus)
{
  std::array<u32, MAX_KEYS_PER_GROUP> bit_for_index;
  for (size_t index = 0; index < bit_for_index.size(); ++index)
    bit_for_index[index] = u32{1} << index;

  const auto lock = GetStateLock();
  for (size_t group = 0; group < GROUP_RANGES.size(); ++group)
  {
    const GroupRange& range = GROUP_RANGES[group];
    if (range.sampled_without_focus != ignore_focus)
      continue;

    ASSERT(GroupSize(range) <= MAX_KEYS_PER_GROUP);
    u32& bits = kb->button[group];
    bits = 0;
    m_group_buttons[group]->GetState(&bits, bit_for_index.data());
  }
}

ControllerEmu::ControlGroup* HotkeyManager::GetHotkeyGroup(HotkeyGroup group) const
{
  return m_group_buttons[static_cast<size_t>(group)];
}

int HotkeyManager::FindGroupByID(int id) const
{
  // Groups are in enum order, so the first one ending at or after `id` holds it.
  const auto match = std::ranges::find_if(
      GROUP_RANGES, [id](const GroupRange& range) { return static_cast<int>(range.last) >= id; });
  return static_cast<int>(std::distance(GROUP_RANGES.begin(), match));
}

int HotkeyManager::GetIndexForGroup(int group, int id) const
{
  return id - GROUP_RANGES[static_cast<size_t>(group)].first;
}

void HotkeyManager::LoadDefaults(const ControllerInterface& ciface)
{
  EmulatedController::LoadDefaults(ciface);

  const auto bind = [this](int hotkey, const std::string& expression) {
    const int group = FindGroupByID(hotkey);
    const int index = GetIndexForGroup(group, hotkey);
    m_group_buttons[static_cast<size_t>(group)]
        ->controls[static_cast<size_t>(index)]
        ->control_ref->SetExpression(expression);
  };

  // Key names differ between the Windows (DInput) and other keyboard backends.
#ifdef _WIN32
  constexpr const char* KEY_ESCAPE = "ESCAPE";
  constexpr const char* KEY_ENTER = "RETURN";
  constexpr const char* KEY_TAB = "TAB";
  constexpr const char* KEYPAD_MINUS = "`SUBTRACT`";
  constexpr const char* KEYPAD_PLUS = "`ADD`";
  constexpr const char* KEYPAD_DIGIT = "`NUMPAD{}`";
#else
  constexpr const char* KEY_ESCAPE = "Escape";
  constexpr const char* KEY_ENTER = "Return";
  constexpr const char* KEY_TAB = "Tab";
  constexpr const char* KEYPAD_MINUS = "`KP_Subtract`";
  constexpr const char* KEYPAD_PLUS = "`KP_Add`";
  constexpr const char* KEYPAD_DIGIT = "`KP_{}`";
#endif

  // General, debugging and Wii Remote connection keys.
  bind(HK_OPEN, Chord({"Ctrl", "O"}));
  bind(HK_PLAY_PAUSE, "F10");
  bind(HK_STOP, KEY_ESCAPE);
  bind(HK_FULLSCREEN, Chord({"Alt", KEY_ENTER}));
#ifdef USE_RETRO_ACHIEVEMENTS
  bind(HK_OPEN_ACHIEVEMENTS, Chord({"Alt", "A"}));
#endif
  bind(HK_STEP, "F11");
  bind(HK_STEP_OVER, Chord({"Shift", "F10"}));
  bind(HK_STEP_OUT, Chord({"Shift", "F11"}));
  bind(HK_BP_TOGGLE, Chord({"Shift", "F9"}));
  bind(HK_SCREENSHOT, "F9");
  for (int remote = 0; remote < 4; ++remote)
    bind(HK_WIIMOTE1_CONNECT + remote, Chord({"Alt", fmt::format("F{}", 5 + remote)}));
  bind(HK_BALANCEBOARD_CONNECT, Chord({"Alt", "F9"}));
  bind(HK_TOGGLE_THROTTLE, KEY_TAB);

  // F1-F8 load state slots 1-8, Shift+F1-F8 save them.
  for (int slot = 0; slot < 8; ++slot)
  {
    const std::string key = fmt::format("F{}", slot + 1);
    bind(HK_LOAD_STATE_SLOT_1 + slot, key);
    bind(HK_SAVE_STATE_SLOT_1 + slot, Chord({"Shift", key}));
  }
  bind(HK_UNDO_LOAD_STATE, "F12");
  bind(HK_UNDO_SAVE_STATE, Chord({"Shift", "F12"}));

  // Game Boy Advance windows.
  bind(HK_GBA_LOAD, Chord({"`Ctrl`", "`Shift`", "`O`"}));
  bind(HK_GBA_UNLOAD, Chord({"`Ctrl`", "`Shift`", "`W`"}));
  bind(HK_GBA_RESET, Chord({"`Ctrl`", "`Shift`", "`R`"}));
  bind(HK_GBA_VOLUME_DOWN, KEYPAD_MINUS);
  bind(HK_GBA_VOLUME_UP, KEYPAD_PLUS);
  bind(HK_GBA_TOGGLE_MUTE, "`M`");
  for (int scale = 1; scale <= 4; ++scale)
    bind(HK_GBA_1X + scale - 1, fmt::format(fmt::runtime(KEYPAD_DIGIT), scale));

  bind(HK_SKYLANDERS_PORTAL, Chord({"Ctrl", "P"}));
  bind(HK_INFINITY_BASE, Chord({"Ctrl", "I"}));
}

// --- HotkeyManagerEmu ------------------------------------------------------------

namespace HotkeyManagerEmu
{
InputConfig* GetConfig()
{
  return &s_input_config;
}

void Initialize()
{
  if (s_input_config.ControllersNeedToBeCreated())
    s_input_config.CreateController<HotkeyManager>();

  s_input_config.RegisterHotplugCallback();
  LoadConfig();

  s_seen_down = {};
  s_enabled = true;
}

void Shutdown()
{
  s_input_config.UnregisterHotplugCallback();
  s_input_config.ClearControllers();
}

void LoadConfig()
{
  s_input_config.LoadConfig();
}

ControllerEmu::ControlGroup* GetHotkeyGroup(HotkeyGroup group)
{
  return TheManager().GetHotkeyGroup(group);
}

void GetStatus(bool ignore_focus)
{
  TheManager().GetInput(&s_sample, ignore_focus);
}

bool IsEnabled()
{
  return s_enabled;
}

void Enable(bool enable_toggle)
{
  s_enabled = enable_toggle;
}

bool IsPressed(int id, bool held)
{
  const HotkeyManager& manager = TheManager();
  const auto group = static_cast<unsigned int>(manager.FindGroupByID(id));
  const auto bit = u32{1} << static_cast<unsigned int>(manager.GetIndexForGroup(static_cast<int>(group), id));

  u32& seen = s_seen_down[group];
  if ((s_sample.button[group] & bit) == 0)
  {
    seen &= ~bit;
    return false;
  }

  const bool was_down = (seen & bit) != 0;
  seen |= bit;
  return held || !was_down;
}
}  // namespace HotkeyManagerEmu
