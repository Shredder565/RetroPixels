// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/SettingsHandler.h"

#include <algorithm>
#include <cstddef>
#include <ctime>

#include <fmt/format.h>

namespace Common
{
namespace
{
// Starting value of the setting.txt obfuscation key.
constexpr u32 KEY_SEED = 0x73B5DBFA;

constexpr u32 NextKey(u32 key)
{
  return (key << 1) | (key >> 31);
}
}  // namespace

SettingsWriter::SettingsWriter() : m_buffer{}, m_position{0}, m_key{KEY_SEED}
{
}

const SettingsBuffer& SettingsWriter::GetBytes() const
{
  return m_buffer;
}

void SettingsWriter::AddSetting(std::string_view key, std::string_view value)
{
  WriteLine(fmt::format("{}={}\r\n", key, value));
}

void SettingsWriter::WriteLine(std::string_view str)
{
  // The console's reader stops at the first encoded 0x00 byte. If this line produced one, rewind
  // and emit an extra LF first, which shifts the key so the line encodes differently (this is
  // what Nintendo's own writer does; LF never encodes to 0x00 with this key schedule). Readers
  // ignore the resulting blank line.
  for (;;)
  {
    const u32 start = m_position;
    const u32 start_key = m_key;
    for (const char c : str)
      WriteByte(static_cast<u8>(c));

    const auto written_begin = m_buffer.begin() + start;
    const auto written_end = m_buffer.begin() + m_position;
    if (std::find(written_begin, written_end, u8{0}) == written_end)
      return;

    m_position = start;
    m_key = start_key;
    WriteByte('\n');
  }
}

void SettingsWriter::WriteByte(u8 b)
{
  if (m_position >= m_buffer.size())
    return;
  m_buffer[m_position++] = static_cast<u8>(b ^ m_key);
  m_key = NextKey(m_key);
}

std::string SettingsWriter::GenerateSerialNumber()
{
  // The SDK rejects serial numbers of 10 or more characters.
  const std::time_t now = std::time(nullptr);
  return fmt::format("{:09}", now % 1000000000);
}

SettingsReader::SettingsReader(const SettingsBuffer& buffer)
{
  m_decoded.reserve(buffer.size());
  u32 key = KEY_SEED;
  for (const u8 byte : buffer)
  {
    m_decoded.push_back(static_cast<char>(byte ^ key));
    key = NextKey(key);
  }

  // Lines end in CRLF, or CRLFLF after the null-avoidance trick above. Dropping every CR leaves
  // LF-separated lines (some of them empty).
  std::erase(m_decoded, '\r');
}

std::string SettingsReader::GetValue(std::string_view key) const
{
  const std::string pattern = fmt::format("{}=", key);

  // A line other than the first, else the first line.
  std::size_t value_start;
  if (const std::size_t pos = m_decoded.find("\n" + pattern); pos != std::string::npos)
    value_start = pos + 1 + pattern.size();
  else if (m_decoded.starts_with(pattern))
    value_start = pattern.size();
  else
    return "";

  // The value runs to the end of its line. Without a terminating LF it stops one character short
  // of the end of the data (setting.txt always has a terminator; this keeps the behaviour that
  // existing callers have seen).
  std::size_t value_end = m_decoded.find('\n', value_start);
  if (value_end == std::string::npos)
    value_end = m_decoded.size() - 1;
  if (value_end < value_start)
    return "";
  return m_decoded.substr(value_start, value_end - value_start);
}
}  // namespace Common
