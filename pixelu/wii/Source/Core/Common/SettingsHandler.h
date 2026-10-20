// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace Common
{
// The Wii's setting.txt: 256 bytes of "KEY=VALUE\r\n" lines, XORed with a rotating 32-bit key
// (only its low byte is used per character; the key rotates left by one bit per byte).
using SettingsBuffer = std::array<u8, 0x100>;

class SettingsWriter
{
public:
  SettingsWriter();

  // Appends "key=value\r\n". Output past the end of the buffer is dropped.
  void AddSetting(std::string_view key, std::string_view value);

  const SettingsBuffer& GetBytes() const;

  // A 9-digit serial number derived from the current time.
  static std::string GenerateSerialNumber();

private:
  void WriteLine(std::string_view str);
  void WriteByte(u8 b);

  SettingsBuffer m_buffer;
  u32 m_position, m_key;
};

class SettingsReader
{
public:
  explicit SettingsReader(const SettingsBuffer& buffer);

  // The value stored for `key`, or "" if there is none.
  std::string GetValue(std::string_view key) const;

private:
  std::string m_decoded;
};
}  // namespace Common
