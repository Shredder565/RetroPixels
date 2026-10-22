// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceAGP.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <utility>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Config/Config.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/HW/EXI/EXI.h"

namespace ExpansionInterface
{

namespace
{
// Command words (the first write of a transfer).
constexpr u32 CMD_ID = 0xAE000000;
constexpr u32 CMD_STATUS = 0xAE010000;
constexpr u32 CMD_ROM_READ_2 = 0xAE020000;
constexpr u32 CMD_ROM_READ_4 = 0xAE030000;
constexpr u32 CMD_EEPROM_READ = 0xAE040000;
constexpr u32 CMD_EEPROM_WRITE = 0xAE070000;
constexpr u32 CMD_RESET_09 = 0xAE090000;
constexpr u32 CMD_EEPROM_RESET = 0xAE0A0000;
constexpr u32 CMD_EEPROM_BIT_READ = 0xAE0B0000;
constexpr u32 CMD_EEPROM_BIT_WRITE = 0xAE0C0000;

constexpr u8 BIT_READ_ZERO = 0x0A;
constexpr u8 BIT_READ_ONE = 0x0B;

constexpr u32 ROM_32MIB = 0x02000000;
constexpr std::size_t EMPTY_ROM_SIZE = 0x2000;

// One step of the 1-Wire CRC-8 (reflected polynomial 0x8C), one table entry
// per bit of the byte.
constexpr std::array<u8, 8> CRC_BITS{0x5E, 0xBC, 0x61, 0xC2, 0x9D, 0x23, 0x46, 0x8C};

u8 Byte(u32 value, int index)
{
  return static_cast<u8>((value >> (index * 8)) & 0xFF);
}

// The save holds 64-bit EEPROM words most significant byte first; in memory
// each word is a host-order u64.
void SwapWords(std::vector<u8>& bytes)
{
  for (std::size_t i = 0; i + 8 <= bytes.size(); i += 8)
  {
    for (std::size_t j = 0; j != 4; ++j)
      std::swap(bytes[i + j], bytes[i + 7 - j]);
  }
}

bool IsStandardSaveSize(u32 size)
{
  return size == 512 || size == 8192;
}

// Only correct on a little-endian host, which every PixelU-Wii build is.
static_assert(std::endian::native == std::endian::little);
}  // namespace

CEXIAgp::CEXIAgp(Core::System& system, const Slot slot) : IEXIDevice(system), m_slot{slot}
{
  ASSERT(IsMemcardSlot(slot));

  std::string folder;
  std::string name;
  std::string extension;
  SplitPath(Config::Get(Config::GetInfoForAGPCartPath(m_slot)), &folder, &name, &extension);
  m_base_path = folder + name;

  LoadRom(m_base_path + extension);
  INFO_LOG_FMT(EXPANSIONINTERFACE, "Loaded GBA rom: {} card: {}", m_base_path, m_slot);
  LoadSave(m_base_path + ".sav");
  INFO_LOG_FMT(EXPANSIONINTERFACE, "Loaded GBA sav: {} card: {}", m_base_path, m_slot);
}

CEXIAgp::~CEXIAgp()
{
  if (!m_base_path.empty())
    WriteSave(m_base_path + ".sav");
}

void CEXIAgp::LoadRom(const std::string& path)
{
  File::IOFile file(path, "rb");
  if (!file.IsOpen())
  {
    m_rom.assign(EMPTY_ROM_SIZE, 0);
    return;
  }
  const u64 size = file.GetSize();
  m_rom_size = static_cast<u32>(size);
  m_rom_mask = m_rom_size - 1;
  m_rom.resize(static_cast<std::size_t>(size));
  file.ReadBytes(m_rom.data(), m_rom.size());
}

void CEXIAgp::LoadSave(const std::string& path)
{
  File::IOFile file(path, "rb");
  if (!file.IsOpen())
    return;

  const u64 size = file.GetSize();
  m_eeprom_size = static_cast<u32>(size);
  m_eeprom_mask = m_eeprom_size - 1;
  m_eeprom.resize(static_cast<std::size_t>(size));
  file.ReadBytes(m_eeprom.data(), m_eeprom.size());

  if (!IsStandardSaveSize(m_eeprom_size))
  {
    m_eeprom_status_mask = 0;
    return;
  }

  SwapWords(m_eeprom);
  const bool small = m_eeprom_size == 512;
  m_eeprom_add_end = small ? 8 : 16;
  m_eeprom_add_mask = small ? 0x3F : 0x3FF;
  m_eeprom_read_mask = small ? 0x80 : 0x8000;
  m_eeprom_status_mask = m_rom_size == ROM_32MIB ? 0x1FFFF00 : 0x1000000;
}

void CEXIAgp::WriteSave(const std::string& path) const
{
  File::IOFile file(path, "wb");
  std::vector<u8> bytes = m_eeprom;
  if (IsStandardSaveSize(m_eeprom_size))
    SwapWords(bytes);
  file.WriteBytes(bytes.data(), bytes.size());
}

void CEXIAgp::Hash(u8 byte)
{
  const u8 mixed = static_cast<u8>(m_hash ^ byte);
  u8 result = 0;
  for (std::size_t bit = 0; bit != CRC_BITS.size(); ++bit)
  {
    if ((mixed >> bit) & 1)
      result ^= CRC_BITS[bit];
  }
  m_hash = result;
}

u8 CEXIAgp::RomByte()
{
  const u32 index = m_rw_offset++ & m_rom_mask;
  return index < m_rom.size() ? m_rom[index] : u8{0};
}

bool CEXIAgp::ReadEEPROMWord(u32 index, u64* word) const
{
  const std::size_t at = std::size_t{index} * 8;
  if (m_eeprom_size == 0 || at + 8 > m_eeprom.size())
    return false;
  std::memcpy(word, &m_eeprom[at], sizeof(*word));
  return true;
}

void CEXIAgp::WriteEEPROMBit(u32 data)
{
  const u32 pos = m_eeprom_pos;
  const u32 end = m_eeprom_add_end;
  const bool set = ((data >> 16) & 1) != 0;
  const u32 special = (m_eeprom_cmd & m_eeprom_read_mask) != 0 ? end : end + 64;

  if (pos < end || pos == special)
  {
    // Address (and read/write flag) bits, most significant first.
    const u32 mask = 1u << (end - std::min(pos, end));
    if (set)
      m_eeprom_cmd |= mask;
    else
      m_eeprom_cmd &= ~mask;

    if (pos == end + 64)
    {
      const std::size_t at = std::size_t{(m_eeprom_cmd >> 1) & m_eeprom_add_mask} * 8;
      if (m_eeprom_size != 0 && at + 8 <= m_eeprom.size())
        std::memcpy(&m_eeprom[at], &m_eeprom_data, sizeof(m_eeprom_data));
      m_eeprom_write_status = true;
    }
    return;
  }

  // Data bits, most significant first; bits past the word are ignored.
  if (pos > end + 63)
    return;
  const u64 mask = u64{1} << (end + 63 - pos);
  if (set)
    m_eeprom_data |= mask;
  else
    m_eeprom_data &= ~mask;
}

void CEXIAgp::ImmWrite(u32 _uData, u32 _uSize)
{
  const u32 data = _uData;
  if (_uSize == 1 && Byte(data, 3) == 0)
    return;

  switch (m_current_cmd)
  {
  case CMD_ROM_READ_2:
  case CMD_ROM_READ_4:
    m_rw_offset = (data & 0xFFFFFF00) >> 7;
    m_return_pos = 0;
    Hash(Byte(data, 3));
    Hash(Byte(data, 2));
    Hash(Byte(data, 1));
    return;

  case CMD_EEPROM_READ:
    m_eeprom_pos = static_cast<u16>((data >> 16) & m_eeprom_mask);
    Hash(Byte(data, 3));
    Hash(Byte(data, 2));
    return;

  case CMD_EEPROM_WRITE:
    m_eeprom_pos = static_cast<u16>((data >> 16) & m_eeprom_mask);
    if (m_eeprom_size != 0 && m_eeprom_pos < m_eeprom.size())
      m_eeprom[m_eeprom_pos] = Byte(data, 1);
    Hash(Byte(data, 3));
    Hash(Byte(data, 2));
    Hash(Byte(data, 1));
    return;

  case CMD_EEPROM_BIT_WRITE:
    WriteEEPROMBit(data);
    ++m_eeprom_pos;
    m_return_pos = 0;
    Hash(Byte(data, 3));
    Hash(Byte(data, 2));
    return;

  case CMD_EEPROM_BIT_READ:
    m_eeprom_write_status = false;
    return;

  case CMD_ID:
  case CMD_STATUS:
  case CMD_RESET_09:
    m_eeprom_write_status = false;
    m_eeprom_pos = 0;
    break;

  case CMD_EEPROM_RESET:
    m_eeprom_pos = 0;
    break;

  default:
    break;
  }

  // A new command.
  DEBUG_LOG_FMT(EXPANSIONINTERFACE, "AGP command {:x}", data);
  m_current_cmd = data;
  m_return_pos = 0;
  m_hash = 0xFF;
  Hash(Byte(data, 2));
}

u32 CEXIAgp::ImmRead(u32 _uSize)
{
  u32 result = 0;
  bool keep_command = false;

  switch (m_current_cmd)
  {
  case CMD_ID:
    result = 0x5AAA5517;
    break;

  case CMD_STATUS:
    result = m_return_pos == 0 ? 0x01020304 : 0xF0020304;
    if (m_return_pos != 1)
    {
      m_return_pos = 1;
      keep_command = true;
    }
    break;

  case CMD_ROM_READ_2:
  {
    u8 first;
    u8 second;
    if (m_eeprom_write_status && m_eeprom_status_mask != 0 &&
        (m_rw_offset & m_eeprom_status_mask) == m_eeprom_status_mask)
    {
      first = 0x01;
      second = 0x00;
    }
    else
    {
      first = RomByte();
      second = RomByte();
    }
    Hash(second);
    Hash(first);
    result = (u32{second} << 24) | (u32{first} << 16) | (u32{m_hash} << 8);
    break;
  }

  case CMD_ROM_READ_4:
  {
    if (_uSize == 1)
    {
      result = 0xFF000000;
      break;
    }
    const u8 v1 = RomByte();
    const u8 v2 = RomByte();
    const u8 v3 = RomByte();
    const u8 v4 = RomByte();
    Hash(v2);
    Hash(v1);
    Hash(v4);
    Hash(v3);
    result = (u32{v2} << 24) | (u32{v1} << 16) | (u32{v4} << 8) | v3;
    keep_command = true;
    break;
  }

  case CMD_EEPROM_READ:
  {
    const u8 value =
        (m_eeprom_size == 0 || m_eeprom_pos >= m_eeprom.size()) ? u8{0xFF} : m_eeprom[m_eeprom_pos];
    Hash(value);
    result = (u32{value} << 24) | (u32{m_hash} << 16);
    break;
  }

  case CMD_EEPROM_BIT_READ:
  {
    u8 value = BIT_READ_ZERO;
    u64 word = 0;
    if (m_eeprom_pos >= 4 && m_eeprom_pos <= 67 &&
        ReadEEPROMWord((m_eeprom_cmd >> 1) & m_eeprom_add_mask, &word))
    {
      if ((word >> (63 - (m_eeprom_pos - 4))) & 1)
        value = BIT_READ_ONE;
    }
    Hash(0x00);
    Hash(value);
    result = (u32{value} << 16) | (u32{m_hash} << 8);
    ++m_eeprom_pos;
    break;
  }

  case CMD_EEPROM_WRITE:
  case CMD_EEPROM_BIT_WRITE:
    result = u32{m_hash} << 24;
    break;

  default:
    result = 0;
    break;
  }

  DEBUG_LOG_FMT(EXPANSIONINTERFACE, "AGP read {:x}", result);
  if (!keep_command)
    m_current_cmd = 0;
  return result;
}

void CEXIAgp::DoState(PointerWrap& p)
{
  // Savestate order is fixed (spec): slot, ROM cursor and command, the EEPROM block, then the
  // cartridge image and its bookkeeping.
  const auto save = [&p](auto&... fields) { (p.Do(fields), ...); };
  save(m_slot, m_address, m_current_cmd, m_eeprom, m_eeprom_cmd, m_eeprom_data, m_eeprom_mask,
       m_eeprom_pos, m_eeprom_size, m_eeprom_add_end, m_eeprom_add_mask, m_eeprom_read_mask,
       m_eeprom_status_mask, m_eeprom_write_status, m_hash, m_position, m_return_pos, m_rom,
       m_rom_mask, m_rom_size, m_rw_offset);
}

}  // namespace ExpansionInterface
