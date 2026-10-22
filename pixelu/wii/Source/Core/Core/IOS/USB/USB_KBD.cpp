// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/USB_KBD.h"

#include <array>
#include <cstddef>
#include <queue>
#include <utility>

#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/System.h"
#include "InputCommon/ControlReference/ControlReference.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace IOS::HLE
{
namespace
{
constexpr int LAYOUT_QWERTY = 0;
constexpr int LAYOUT_AZERTY = 1;

using KeyTable = std::array<u8, 256>;

// Windows virtual key code -> USB HID keyboard usage, for a US (QWERTY) keyboard.
// Keys that are not listed are ignored.
constexpr std::pair<u8, u8> QWERTY_KEYS[] = {
    {0x08, 0x2A},  // backspace
    {0x09, 0x2B},  // tab
    {0x0D, 0x28},  // enter
    {0x13, 0x48},  // pause
    {0x14, 0x39},  // caps lock
    {0x1B, 0x29},  // escape
    {0x20, 0x2C},  // space
    {0x21, 0x4B},  // page up
    {0x22, 0x4E},  // page down
    {0x23, 0x4D},  // end
    {0x24, 0x4A},  // home
    {0x25, 0x50},  // left
    {0x26, 0x52},  // up
    {0x27, 0x4F},  // right
    {0x28, 0x51},  // down
    {0x2C, 0x46},  // print screen
    {0x2D, 0x49},  // insert
    {0x2E, 0x4C},  // delete
    {0x6A, 0x55},  // keypad *
    {0x6B, 0x57},  // keypad +
    {0x6D, 0x56},  // keypad -
    {0x6E, 0x63},  // keypad .
    {0x6F, 0x54},  // keypad /
    {0x90, 0x53},  // num lock
    {0x91, 0x47},  // scroll lock
    {0xBA, 0x33},  // ;
    {0xBB, 0x2E},  // =
    {0xBC, 0x36},  // ,
    {0xBD, 0x2D},  // -
    {0xBE, 0x37},  // .
    {0xBF, 0x38},  // /
    {0xC0, 0x35},  // `
    {0xDB, 0x2F},  // [
    {0xDC, 0x32},  // backslash
    {0xDD, 0x30},  // ]
    {0xDE, 0x34},  // '
};

// Where a French (AZERTY) keyboard differs from the table above. A usage of 0 removes the key.
constexpr std::pair<u8, u8> AZERTY_CHANGES[] = {
    {'A', 0x14}, {'M', 0x33}, {'Q', 0x04}, {'W', 0x1D}, {'Z', 0x1A},
    {0xBA, 0x30},  // $
    {0xBC, 0x10},  // ,
    {0xBD, 0x00},
    {0xBE, 0x36},  // ;
    {0xBF, 0x37},  // :
    {0xC0, 0x34},  // ù
    {0xDB, 0x2D},  // )
    {0xDD, 0x2F},  // ^
    {0xDE, 0x00},
    {0xDF, 0x38},  // !
};

constexpr KeyTable BuildQwerty()
{
  KeyTable table{};
  // Top row digits: HID puts 1..9 at 0x1E..0x26 and 0 after them.
  for (u8 digit = 1; digit <= 9; ++digit)
    table['0' + digit] = static_cast<u8>(0x1E + digit - 1);
  table['0'] = 0x27;
  // Letters are in alphabetical order in both.
  for (u8 letter = 0; letter < 26; ++letter)
    table['A' + letter] = static_cast<u8>(0x04 + letter);
  // Keypad digits (VK_NUMPAD0..9): HID has 1..9 at 0x59..0x61 and 0 at 0x62.
  for (u8 digit = 1; digit <= 9; ++digit)
    table[0x60 + digit] = static_cast<u8>(0x59 + digit - 1);
  table[0x60] = 0x62;
  // F1..F12 (VK 0x70..) map to 0x3A.. in order.
  for (u8 key = 0; key < 12; ++key)
    table[0x70 + key] = static_cast<u8>(0x3A + key);

  for (const auto& [virtual_key, usage] : QWERTY_KEYS)
    table[virtual_key] = usage;
  return table;
}

constexpr KeyTable BuildAzerty()
{
  KeyTable table = BuildQwerty();
  for (const auto& [virtual_key, usage] : AZERTY_CHANGES)
    table[virtual_key] = usage;
  return table;
}

constexpr KeyTable KEYS_QWERTY = BuildQwerty();
constexpr KeyTable KEYS_AZERTY = BuildAzerty();

#ifdef _WIN32
// Host keys behind each modifier bit, lowest bit first. Alt is used for both Alt bits.
constexpr int MODIFIER_KEYS[8] = {VK_LCONTROL, VK_LSHIFT, VK_MENU,  VK_LWIN,
                                  VK_RCONTROL, VK_RSHIFT, VK_MENU, VK_RWIN};
#endif
}  // namespace

USB_KBD::USB_KBD(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
}

USB_KBD::Report USB_KBD::MakeReport(ReportKind kind, u8 modifiers, const NewKeys& keys)
{
  Report report;
  report.kind_be = Common::swap32(static_cast<u32>(kind));
  report.modifiers = modifiers;
  report.keys = keys;
  return report;
}

std::optional<IPCReply> USB_KBD::Open(const OpenRequest& request)
{
  INFO_LOG_FMT(IOS, "{}: opened", GetDeviceName());

  Common::IniFile ini;
  ini.Load(File::GetUserPath(F_DOLPHINCONFIG_IDX));
  ini.GetOrCreateSection("USB Keyboard")->Get("Layout", &m_layout, LAYOUT_QWERTY);

  // Start from "nothing pressed". (No connect report is queued.)
  m_reports = {};
  m_was_down.fill(false);
  m_last_modifiers = 0;
  return Device::Open(request);
}

std::optional<IPCReply> USB_KBD::Write(const ReadWriteRequest&)
{
  // Output reports (LEDs) are accepted and dropped.
  return IPCReply(IPC_SUCCESS);
}

std::optional<IPCReply> USB_KBD::IOCtl(const IOCtlRequest& request)
{
  const bool can_deliver = Config::Get(Config::MAIN_WII_KEYBOARD) && !Core::WantsDeterminism() &&
                           ControlReference::GetInputGate() && !m_reports.empty();
  if (can_deliver)
  {
    GetSystem().GetMemory().CopyToEmu(request.buffer_out, &m_reports.front(), sizeof(Report));
    m_reports.pop();
  }
  return IPCReply(IPC_SUCCESS);
}

bool USB_KBD::IsHostKeyDown(int virtual_key)
{
#ifdef _WIN32
  return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
#else
  // Host keyboard input is only read on Windows so far.
  static_cast<void>(virtual_key);
  return false;
#endif
}

u8 USB_KBD::ReadHostModifiers()
{
  u8 modifiers = 0;
#ifdef _WIN32
  for (size_t bit = 0; bit < 8; ++bit)
  {
    if (IsHostKeyDown(MODIFIER_KEYS[bit]))
      modifiers = static_cast<u8>(modifiers | (1u << bit));
  }
#endif
  return modifiers;
}

void USB_KBD::Update()
{
  if (!Config::Get(Config::MAIN_WII_KEYBOARD) || Core::WantsDeterminism() || !m_is_active)
    return;

  const KeyTable* table = nullptr;
  if (m_layout == LAYOUT_QWERTY)
    table = &KEYS_QWERTY;
  else if (m_layout == LAYOUT_AZERTY)
    table = &KEYS_AZERTY;

  NewKeys new_keys{};
  size_t new_key_count = 0;
  bool changed = false;

  for (size_t key = 0; key < m_was_down.size(); ++key)
  {
    const bool down = IsHostKeyDown(static_cast<int>(key));
    if (down == m_was_down[key])
      continue;

    if (down)
    {
      const u8 usage = table ? (*table)[key] : 0;
      // A key with no usage is left marked as up, so it is looked at again next time.
      if (usage == 0)
        continue;
      new_keys[new_key_count++] = usage;
      // The report is full: this key and the ones after it are seen as new again next time.
      if (new_key_count == new_keys.size())
        break;
    }

    changed = true;
    m_was_down[key] = down;
  }

  const u8 modifiers = ReadHostModifiers();
  if (modifiers != m_last_modifiers)
  {
    changed = true;
    m_last_modifiers = modifiers;
  }

  if (changed)
    m_reports.push(MakeReport(ReportKind::KeyChange, modifiers, new_keys));
}
}  // namespace IOS::HLE
