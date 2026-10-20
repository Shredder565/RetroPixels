// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GCMemcard/GCIFile.h"

#include <algorithm>
#include <cstring>
#include <string_view>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"

namespace Memcard
{
namespace
{
// The directory entry's file name as a C string (it may fill all 32 bytes).
std::string_view SaveName(const DEntry& entry)
{
  const char* text = reinterpret_cast<const char*>(entry.m_filename.data());
  return std::string_view(text, strnlen(text, entry.m_filename.size()));
}
}  // namespace

bool GCIFile::LoadHeader()
{
  if (m_filename.empty())
    return false;

  File::IOFile gci(m_filename, "rb");
  if (!gci)
    return false;

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Reading header from disk for {}", m_filename);
  if (gci.ReadBytes(&m_gci_header, sizeof(m_gci_header)))
    return true;

  ERROR_LOG_FMT(EXPANSIONINTERFACE, "Failed to read header for {}", m_filename);
  return false;
}

bool GCIFile::LoadSaveBlocks()
{
  // Already resident (either loaded earlier or written by the game).
  if (!m_save_data.empty())
    return true;

  if (m_filename.empty())
    return false;

  File::IOFile gci(m_filename, "rb");
  if (!gci)
    return false;

  INFO_LOG_FMT(EXPANSIONINTERFACE, "Reading savedata from disk for {}", m_filename);

  // A GCI is exactly one directory entry followed by the blocks it declares.
  const u16 blocks = m_gci_header.m_block_count;
  const u32 data_bytes = blocks * BLOCK_SIZE;
  const u32 expected_size = data_bytes + DENTRY_SIZE;
  const u64 actual_size = gci.GetSize();
  if (actual_size != expected_size)
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE,
                  "{}\nwas not loaded because it is an invalid GCI.\n File size ({:#x}) does not "
                  "match the size recorded in the header ({:#x})",
                  m_filename, actual_size, expected_size);
    return false;
  }

  m_save_data.resize(blocks);
  gci.Seek(DENTRY_SIZE, File::SeekOrigin::Begin);
  if (gci.ReadBytes(m_save_data.data(), data_bytes))
    return true;

  ERROR_LOG_FMT(EXPANSIONINTERFACE, "Failed to read data from GCI file {}", m_filename);
  m_save_data.clear();
  return false;
}

bool GCIFile::HasCopyProtection() const
{
  const std::string_view name = SaveName(m_gci_header);
  return name == "PSO_SYSTEM" || name == "PSO3_SYSTEM" || name == "f_zero.dat";
}

int GCIFile::UsesBlock(u16 blocknum)
{
  const auto it = std::find(m_used_blocks.begin(), m_used_blocks.end(), blocknum);
  if (it == m_used_blocks.end())
    return -1;
  return static_cast<int>(it - m_used_blocks.begin());
}

void GCIFile::DoState(PointerWrap& p)
{
  // Savestate order: entry, dirty flag, path, blocks, block list.
  p.Do(m_gci_header);
  p.Do(m_dirty);
  p.Do(m_filename);
  p.Do(m_save_data);
  p.Do(m_used_blocks);
}
}  // namespace Memcard
