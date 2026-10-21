// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"

namespace IOS::HLE::USB
{
enum class Type : u8;

struct SkylanderDateTime final
{
  u8 minute;
  u8 hour;
  u8 day;
  u8 month;
  u16 year;
};

// The editable values of a character figure.
struct SkylanderData final
{
  std::array<u8, 11> toy_code;
  u16 money;
  u16 hero_level;
  u32 playtime;
  // Null-terminated UTF-16 string
  std::array<u16, 0x10> nickname;
  SkylanderDateTime last_reset;
  SkylanderDateTime last_placed;
};

struct Trophydata final
{
  u8 unlocked_villains;
};

struct FigureData final
{
  Type normalized_type;
  u16 figure_id;
  u16 variant_id;
  union
  {
    SkylanderData skylander_data;
    Trophydata trophy_data;
  };
};

// A figure is a MIFARE Classic 1K tag: 16 sectors of four 16-byte blocks.
constexpr u32 BLOCK_COUNT = 0x40;
constexpr u32 BLOCK_SIZE = 0x10;
constexpr u32 FIGURE_SIZE = BLOCK_COUNT * BLOCK_SIZE;

// A figure dump, kept in memory and written back to its file.
class SkylanderFigure
{
public:
  // Creates (or truncates) the file; call Create() to fill it.
  SkylanderFigure(const std::string& file_path);
  // Reads a dump from an open file.
  SkylanderFigure(File::IOFile file);

  // Makes a blank figure with this ID and variant and writes it to the file.
  bool Create(u16 sky_id, u16 sky_var,
              std::optional<std::array<u8, 4>> requested_nuid = std::nullopt);
  void Save();
  void Close();
  bool FileIsOpen() const;

  // Raw (encrypted) block access, as the portal sees the tag.
  void GetBlock(u8 index, u8* dest) const;
  void SetBlock(u8 block, const u8* buf);

  void DecryptFigure(std::array<u8, FIGURE_SIZE>* dest) const;
  FigureData GetData() const;
  // Writes the values back (updating checksums and area counters) and saves.
  void SetData(FigureData* data);

private:
  using Dump = std::array<u8, FIGURE_SIZE>;

  void WriteSectorTrailers();
  void WriteSectorKeys();
  std::array<u8, 0x56> BlockKeySeed() const;
  void StoreEncrypted(std::span<const u8, FIGURE_SIZE> plain);

  File::IOFile m_sky_file;
  Dump m_data;
};
}  // namespace IOS::HLE::USB
