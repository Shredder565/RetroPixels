// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI_DeviceKeyboard.h"

#include <array>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/GCKeyboard.h"
#include "InputCommon/KeyboardStatus.h"

namespace SerialInterface
{
namespace
{
constexpr std::size_t MAX_REPORTED_KEYS = 3;

// The keyboard status packs 85 keys into six 16-bit words, one bit per key
// in scancode order (only the low 5 bits of the last word are used). The
// scancodes are consecutive except for three unused gaps: 0x0B-0x0F after
// Scroll Lock, 0x52 after Tab and 0x60 before Enter.
constexpr u8 ScancodeForKeyIndex(u32 index)
{
  if (index < 5)
    return static_cast<u8>(KEY_HOME + index);
  if (index < 71)
    return static_cast<u8>(index + 0x0B);
  if (index < 84)
    return static_cast<u8>(index + 0x0C);
  return static_cast<u8>(index + 0x0D);
}
static_assert(ScancodeForKeyIndex(5) == KEY_A);
static_assert(ScancodeForKeyIndex(70) == KEY_TAB);
static_assert(ScancodeForKeyIndex(71) == KEY_CAPSLOCK);
static_assert(ScancodeForKeyIndex(83) == KEY_RIGHTARROW);
static_assert(ScancodeForKeyIndex(84) == KEY_ENTER);

void StoreWordBE(u8* dst, u32 value)
{
  for (int i = 0; i < 4; ++i)
    dst[i] = static_cast<u8>(value >> (24 - 8 * i));
}
}  // namespace

CSIDevice_Keyboard::CSIDevice_Keyboard(Core::System& system, SIDevices device, int device_number)
    : ISIDevice(system, device, device_number)
{
}

int CSIDevice_Keyboard::RunBuffer(u8* buffer, int request_length)
{
  ISIDevice::RunBuffer(buffer, request_length);

  const u8 command = buffer[0];
  switch (static_cast<EBufferCommands>(command))
  {
  case EBufferCommands::CMD_STATUS:
  case EBufferCommands::CMD_RESET:
    return CreateStatusResponse(SI_GC_KEYBOARD, buffer);

  case EBufferCommands::CMD_DIRECT_KB:
  {
    INFO_LOG_FMT(SERIALINTERFACE, "Keyboard {}: direct poll ({} byte request)", m_device_number,
                 request_length);
    u32 hi = 0;
    u32 lo = 0;
    GetData(hi, lo);
    StoreWordBE(buffer, hi);
    StoreWordBE(buffer + 4, lo);
    return 8;
  }

  default:
    ERROR_LOG_FMT(SERIALINTERFACE, "Keyboard {}: unhandled SI command {:#x}", m_device_number,
                  command);
    return 0;
  }
}

KeyboardStatus CSIDevice_Keyboard::GetKeyboardStatus() const
{
  return Keyboard::GetStatus(m_device_number);
}

DataResponse CSIDevice_Keyboard::GetData(u32& hi, u32& low)
{
  const KeyArray keys = MapKeys(GetKeyboardStatus());
  const u8 check = static_cast<u8>(keys[0] ^ keys[1] ^ keys[2] ^ m_counter);

  hi = u32{m_counter} << 24;
  low = (u32{keys[0]} << 24) | (u32{keys[1]} << 16) | (u32{keys[2]} << 8) | check;
  return DataResponse::Success;
}

void CSIDevice_Keyboard::SendCommand(u32 command, u8)
{
  const UCommand word(command);
  if (static_cast<EDirectCommands>(word.command) == EDirectCommands::CMD_POLL)
  {
    m_counter = static_cast<u8>((m_counter + 1) & 0xF);
    return;
  }
  if (word.command != 0)
  {
    ERROR_LOG_FMT(SERIALINTERFACE, "Keyboard {}: unhandled direct command {:#x}", m_device_number,
                  command);
  }
}

void CSIDevice_Keyboard::DoState(PointerWrap& p)
{
  p.Do(m_counter);
}

CSIDevice_Keyboard::KeyArray CSIDevice_Keyboard::MapKeys(const KeyboardStatus& key_status) const
{
  const std::array<u16, 6> words = {key_status.key0x, key_status.key1x, key_status.key2x,
                                    key_status.key3x, key_status.key4x, key_status.key5x};
  constexpr u32 KEY_COUNT = 85;

  // Report the first three held keys in scancode order.
  KeyArray reported{};
  std::size_t found = 0;
  for (u32 index = 0; index < KEY_COUNT && found < MAX_REPORTED_KEYS; ++index)
  {
    if ((words[index / 16] >> (index % 16)) & 1)
      reported[found++] = ScancodeForKeyIndex(index);
  }
  return reported;
}
}  // namespace SerialInterface
