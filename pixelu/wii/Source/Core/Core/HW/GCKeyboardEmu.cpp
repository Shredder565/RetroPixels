// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GCKeyboardEmu.h"

#include <array>

#include "Common/Common.h"
#include "Common/CommonTypes.h"

#include "Core/HW/GCKeyboard.h"

#include "InputCommon/ControllerEmu/ControlGroup/Buttons.h"
#include "InputCommon/ControllerEmu/ControlGroup/ControlGroup.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/KeyboardStatus.h"

namespace
{
constexpr std::size_t WORD_COUNT = 6;
constexpr std::size_t KEY_COUNT = 85;  // 5 * 16 + 5

// Control names (stored in the INI) for every key, in scancode order; key
// n is bit n % 16 of status word n / 16.
constexpr std::array<const char*, KEY_COUNT> KEY_NAMES = {
    "HOME", "END", "PGUP", "PGDN", "SCR LK", "A", "B", "C",  //
    "D", "E", "F", "G", "H", "I", "J", "K",                  //
    "L", "M", "N", "O", "P", "Q", "R", "S",                  //
    "T", "U", "V", "W", "X", "Y", "Z", "1",                  //
    "2", "3", "4", "5", "6", "7", "8", "9",                  //
    "0", "-", "`", "PRT SC", "'", "[", "EQUALS", "*",        //
    "]", ",", ".", "/", "\\", "F1", "F2", "F3",              //
    "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11",        //
    "F12", "ESC", "INSERT", "DELETE", ";", "BACKSPACE", "TAB", "CAPS LOCK",
    "L SHIFT", "R SHIFT", "L CTRL", "R ALT", "L WIN", "SPACE", "R WIN", "MENU",
    "LEFT", "DOWN", "UP", "RIGHT", "ENTER",
};

// Default mapping to the Windows keyboard backend's key names, same order.
constexpr std::array<const char*, KEY_COUNT> KEY_DEFAULTS = {
    "HOME", "END", "PRIOR", "NEXT", "SCROLL", "A", "B", "C",  //
    "D", "E", "F", "G", "H", "I", "J", "K",                   //
    "L", "M", "N", "O", "P", "Q", "R", "S",                   //
    "T", "U", "V", "W", "X", "Y", "Z", "`1`",                 //
    "`2`", "`3`", "`4`", "`5`", "`6`", "`7`", "`8`", "`9`",   //
    "`0`", "MINUS", "GRAVE", "SYSRQ", "APOSTROPHE", "LBRACKET", "EQUALS", "MULTIPLY",
    "RBRACKET", "COMMA", "PERIOD", "SLASH", "BACKSLASH", "F1", "F2", "F3",  //
    "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11",                        //
    "F12", "ESCAPE", "INSERT", "DELETE", "SEMICOLON", "BACK", "TAB", "CAPITAL",
    "LSHIFT", "RSHIFT", "LCONTROL", "RMENU", "LWIN", "SPACE", "RWIN", "MENU",
    "LEFT", "DOWN", "UP", "RIGHT", "RETURN",
};

constexpr std::size_t KeysInWord(std::size_t word)
{
  return word + 1 < WORD_COUNT ? 16 : KEY_COUNT - 16 * (WORD_COUNT - 1);
}

// One bit per control, lowest first.
constexpr std::array<u16, 16> BIT_MASKS = {
    0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
    0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000, 0x8000,
};
}  // namespace

GCKeyboard::GCKeyboard(const unsigned int index) : m_index(index)
{
  ControllerEmu::Buttons** const words[WORD_COUNT] = {&m_keys0x, &m_keys1x, &m_keys2x,
                                                      &m_keys3x, &m_keys4x, &m_keys5x};
  for (std::size_t word = 0; word < WORD_COUNT; ++word)
  {
    auto* group = new ControllerEmu::Buttons(_trans("Keys"));
    groups.emplace_back(group);
    *words[word] = group;
    for (std::size_t key = 0; key < KeysInWord(word); ++key)
      group->AddInput(ControllerEmu::Translatability::DoNotTranslate, KEY_NAMES[16 * word + key]);
  }
}

std::string GCKeyboard::GetName() const
{
  std::string name = "GCKeyboard";
  name += static_cast<char>('1' + m_index);
  return name;
}

InputConfig* GCKeyboard::GetConfig() const
{
  return Keyboard::GetConfig();
}

ControllerEmu::ControlGroup* GCKeyboard::GetGroup(KeyboardGroup group)
{
  switch (group)
  {
  case KeyboardGroup::Kb0x:
    return m_keys0x;
  case KeyboardGroup::Kb1x:
    return m_keys1x;
  case KeyboardGroup::Kb2x:
    return m_keys2x;
  case KeyboardGroup::Kb3x:
    return m_keys3x;
  case KeyboardGroup::Kb4x:
    return m_keys4x;
  case KeyboardGroup::Kb5x:
    return m_keys5x;
  }
  return nullptr;
}

KeyboardStatus GCKeyboard::GetInput() const
{
  const auto state_lock = GetStateLock();

  KeyboardStatus status = {};
  const ControllerEmu::Buttons* const sources[WORD_COUNT] = {m_keys0x, m_keys1x, m_keys2x,
                                                             m_keys3x, m_keys4x, m_keys5x};
  u16* const targets[WORD_COUNT] = {&status.key0x, &status.key1x, &status.key2x,
                                    &status.key3x, &status.key4x, &status.key5x};
  for (std::size_t word = 0; word < WORD_COUNT; ++word)
    sources[word]->GetState(targets[word], BIT_MASKS.data());

  return status;
}

void GCKeyboard::LoadDefaults(const ControllerInterface& ciface)
{
  EmulatedController::LoadDefaults(ciface);

  ControllerEmu::Buttons* const words[WORD_COUNT] = {m_keys0x, m_keys1x, m_keys2x,
                                                     m_keys3x, m_keys4x, m_keys5x};
  for (std::size_t word = 0; word < WORD_COUNT; ++word)
  {
    for (std::size_t key = 0; key < KeysInWord(word); ++key)
      words[word]->SetControlExpression(static_cast<int>(key), KEY_DEFAULTS[16 * word + key]);
  }
}
