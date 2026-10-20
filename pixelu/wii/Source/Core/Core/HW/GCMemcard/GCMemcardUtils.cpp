// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GCMemcard/GCMemcardUtils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/NandPaths.h"

#include "Core/HW/GCMemcard/GCMemcard.h"

namespace Memcard
{
namespace
{
// Every export container is "some header, then whole 8 KiB blocks". The
// header sizes are all different modulo BLOCK_SIZE, which is how the
// container is recognised.
struct ContainerLayout
{
  u32 header_size;
  std::string_view magic;  // At offset 0; empty for GCI.
  u32 entry_offset;        // Where the directory entry sits in the header.
};

// GCI: the header is just the directory entry.
constexpr ContainerLayout GCI_LAYOUT{DENTRY_SIZE, {}, 0};
// GameShark / GameSaves.
constexpr ContainerLayout GCS_LAYOUT{0x150, "GCSAVE", 0x110};
// MaxDrive / Datel.
constexpr ContainerLayout SAV_LAYOUT{0xC0, "DATELGC_SAVE", 0x80};

constexpr size_t MAX_HEADER_SIZE = 0x150;

using HeaderBytes = std::array<u8, MAX_HEADER_SIZE>;

// The Datel tool stores a handful of 16-bit entry fields with their bytes the
// other way round (the field at 0x06 and every halfword from 0x2C to 0x3E).
void SwapSavEntryFields(u8* entry)
{
  std::swap(entry[0x06], entry[0x07]);
  for (size_t pos = 0x2C; pos < DENTRY_SIZE; pos += 2)
    std::swap(entry[pos], entry[pos + 1]);
}

std::string_view CStringPrefix(const u8* data, size_t max_length)
{
  const char* text = reinterpret_cast<const char*>(data);
  return std::string_view(text, strnlen(text, max_length));
}

bool ReadBlocks(File::IOFile& file, size_t count, std::vector<GCMBlock>& out)
{
  out.reserve(count);
  for (size_t n = 0; n < count; ++n)
  {
    if (!file.ReadBytes(out.emplace_back().m_block.data(), BLOCK_SIZE))
      return false;
  }
  return true;
}

std::variant<ReadSavefileErrorCode, Savefile> ReadContainer(File::IOFile& file, u64 file_size,
                                                            const ContainerLayout& layout,
                                                            SavefileFormat format)
{
  HeaderBytes header{};
  if (!file.ReadBytes(header.data(), layout.header_size))
    return ReadSavefileErrorCode::IOError;

  if (!layout.magic.empty() &&
      std::memcmp(header.data(), layout.magic.data(), layout.magic.size()) != 0)
  {
    return ReadSavefileErrorCode::DataCorrupted;
  }

  u8* entry_bytes = header.data() + layout.entry_offset;
  if (format == SavefileFormat::SAV)
    SwapSavEntryFields(entry_bytes);

  Savefile save;
  std::memcpy(&save.dir_entry, entry_bytes, DENTRY_SIZE);

  const u64 payload = file_size - layout.header_size;
  size_t count;
  if (format == SavefileFormat::GCS)
  {
    // GCS files don't keep a usable block count in the entry (the real one
    // lives in a separate .gsv file), so derive it from the payload size.
    if (payload % BLOCK_SIZE != 0)
      return ReadSavefileErrorCode::DataCorrupted;
    count = static_cast<size_t>(payload / BLOCK_SIZE);
    save.dir_entry.m_block_count = static_cast<u16>(count);
  }
  else
  {
    count = save.dir_entry.m_block_count;
    if (static_cast<u64>(count) * BLOCK_SIZE != payload)
      return ReadSavefileErrorCode::DataCorrupted;
  }

  if (!ReadBlocks(file, count, save.blocks))
    return ReadSavefileErrorCode::IOError;
  return save;
}
}  // namespace

bool HasSameIdentity(const DEntry& lhs, const DEntry& rhs)
{
  // The IPL itself compares the whole 32-byte name, but games only look at
  // it up to the first NUL, and the IPL misbehaves when copying two saves that
  // differ only after it. Treating the name as a C string is the less
  // surprising rule for users, so that is what is used here.
  if (lhs.m_gamecode != rhs.m_gamecode || lhs.m_makercode != rhs.m_makercode)
    return false;
  return CStringPrefix(lhs.m_filename.data(), lhs.m_filename.size()) ==
         CStringPrefix(rhs.m_filename.data(), rhs.m_filename.size());
}

bool HasDuplicateIdentity(std::span<const Savefile> savefiles)
{
  for (auto first = savefiles.begin(); first != savefiles.end(); ++first)
  {
    const bool clash = std::any_of(first + 1, savefiles.end(), [&](const Savefile& other) {
      return HasSameIdentity(first->dir_entry, other.dir_entry);
    });
    if (clash)
      return true;
  }
  return false;
}

std::variant<ReadSavefileErrorCode, Savefile> ReadSavefile(const std::string& filename)
{
  File::IOFile file(filename, "rb");
  if (!file)
    return ReadSavefileErrorCode::OpenFileFail;

  const u64 file_size = file.GetSize();
  const u64 header_size = file_size % BLOCK_SIZE;
  if (header_size == GCI_LAYOUT.header_size)
    return ReadContainer(file, file_size, GCI_LAYOUT, SavefileFormat::GCI);
  if (header_size == GCS_LAYOUT.header_size)
    return ReadContainer(file, file_size, GCS_LAYOUT, SavefileFormat::GCS);
  if (header_size == SAV_LAYOUT.header_size)
    return ReadContainer(file, file_size, SAV_LAYOUT, SavefileFormat::SAV);
  return ReadSavefileErrorCode::DataCorrupted;
}

bool WriteSavefile(const std::string& filename, const Savefile& savefile, SavefileFormat format)
{
  File::IOFile file(filename, "wb");
  if (!file)
    return false;

  const ContainerLayout* layout;
  switch (format)
  {
  case SavefileFormat::GCI:
    layout = &GCI_LAYOUT;
    break;
  case SavefileFormat::GCS:
    layout = &GCS_LAYOUT;
    break;
  case SavefileFormat::SAV:
    layout = &SAV_LAYOUT;
    break;
  default:
    return false;
  }

  // Zero-filled header with the magic up front and the entry at its slot.
  HeaderBytes header{};
  std::copy(layout->magic.begin(), layout->magic.end(), header.begin());
  DEntry entry = savefile.dir_entry;
  if (format == SavefileFormat::GCS)
    entry.m_block_count = 1;  // What GameSaves itself writes there.
  u8* entry_bytes = header.data() + layout->entry_offset;
  std::memcpy(entry_bytes, &entry, DENTRY_SIZE);
  if (format == SavefileFormat::SAV)
    SwapSavEntryFields(entry_bytes);

  if (!file.WriteBytes(header.data(), layout->header_size))
    return false;
  for (const GCMBlock& block : savefile.blocks)
  {
    if (!file.WriteBytes(block.m_block.data(), block.m_block.size()))
      return false;
  }
  return file.IsGood();
}

std::string GenerateFilename(const DEntry& entry)
{
  std::string name(reinterpret_cast<const char*>(entry.m_makercode.data()),
                   entry.m_makercode.size());
  name += '-';
  name.append(reinterpret_cast<const char*>(entry.m_gamecode.data()), entry.m_gamecode.size());
  name += '-';
  name += CStringPrefix(entry.m_filename.data(), entry.m_filename.size());
  return Common::EscapeFileName(name);
}

std::string GetDefaultExtension(SavefileFormat format)
{
  switch (format)
  {
  case SavefileFormat::GCI:
    return ".gci";
  case SavefileFormat::GCS:
    return ".gcs";
  case SavefileFormat::SAV:
    return ".sav";
  }
  ASSERT(false);
  return ".gci";
}

std::vector<Savefile> GetSavefiles(const GCMemcard& card, std::span<const u8> file_indices)
{
  std::vector<Savefile> result;
  result.reserve(file_indices.size());
  for (const u8 index : file_indices)
  {
    std::optional<Savefile> exported = card.ExportFile(index);
    if (!exported)
      return {};
    result.push_back(std::move(*exported));
  }
  return result;
}

size_t GetBlockCount(std::span<const Savefile> savefiles)
{
  size_t total = 0;
  for (const Savefile& save : savefiles)
    total += save.blocks.size();
  return total;
}
}  // namespace Memcard
