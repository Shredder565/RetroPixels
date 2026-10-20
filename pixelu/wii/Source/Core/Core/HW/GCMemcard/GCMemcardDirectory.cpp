// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/GCMemcard/GCMemcardDirectory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Thread.h"

#include "Core/Config/MainSettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardUtils.h"
#include "Core/HW/Sram.h"
#include "Core/NetPlayProto.h"

using Memcard::BLOCK_SIZE;
using Memcard::DENTRY_SIZE;

namespace
{
// Optional dump of the card's system area kept inside the folder.
constexpr std::string_view SYSTEM_AREA_FILE = "MC_SYSTEM_AREA";

// Card block numbers of the system area.
enum : s32
{
  HEADER_BLOCK = 0,
  DIRECTORY_BLOCK = 1,
  DIRECTORY_BACKUP_BLOCK = 2,
  BAT_BLOCK = 3,
  BAT_BACKUP_BLOCK = 4,
};

// Largest save that can fit on the biggest card.
constexpr u16 MAX_SAVE_BLOCKS = 2043;

// Saves of other games stop loading once this many entries are in use, so a
// game always has some directory slots left for new saves.
constexpr size_t OTHER_GAME_ENTRY_LIMIT = 112;

constexpr u32 GAMECODE_UNUSED = 0xFFFFFFFF;

u32 GameCodeOf(const Memcard::DEntry& entry)
{
  return Common::swap32(entry.m_gamecode.data());
}

bool IsUnused(const Memcard::DEntry& entry)
{
  return entry.m_gamecode == Memcard::DEntry::UNINITIALIZED_GAMECODE;
}

std::string_view UpToNul(std::string_view text)
{
  return text.substr(0, std::min(text.find('\0'), text.size()));
}

// "<maker>-<game>-<name>.gci", with each field decoded from the card's text
// encoding (decoded whole, then cut at the first NUL).
std::string DefaultGciName(const Memcard::DEntry& entry, bool shift_jis)
{
  const auto decode = [shift_jis](const auto& field) {
    const std::string_view raw(reinterpret_cast<const char*>(field.data()), field.size());
    return shift_jis ? SHIFTJISToUTF8(raw) : CP1252ToUTF8(raw);
  };
  const std::string maker = decode(entry.m_makercode);
  const std::string game = decode(entry.m_gamecode);
  const std::string name = decode(entry.m_filename);
  return Common::EscapeFileName(
      fmt::format("{}-{}-{}.gci", UpToNul(maker), UpToNul(game), UpToNul(name)));
}

// The directory or BAT copy the game wrote last (the higher update counter;
// ties go to the second argument).
template <typename T>
T& Newest(T& first, T& second)
{
  return first.m_update_counter > second.m_update_counter ? first : second;
}
}  // namespace

bool GCMemcardDirectory::LoadGCI(Memcard::GCIFile gci)
{
  // The card can only hold one save per identity.
  const auto clash = std::ranges::find_if(m_saves, [&gci](const Memcard::GCIFile& loaded) {
    return Memcard::HasSameIdentity(gci.m_gci_header, loaded.m_gci_header);
  });
  if (clash != m_saves.end())
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE,
                  "{}\nwas not loaded because it has the same internal filename as previously "
                  "loaded save\n{}",
                  gci.m_filename, clash->m_filename);
    return false;
  }

  const u16 blocks = gci.m_gci_header.m_block_count;
  if (blocks > MAX_SAVE_BLOCKS)
  {
    ERROR_LOG_FMT(
        EXPANSIONINTERFACE,
        "{}\nwas not loaded because it is an invalid GCI.\nNumber of blocks claimed to be {}",
        gci.m_filename, blocks);
    return false;
  }

  if (!gci.LoadSaveBlocks())
  {
    ERROR_LOG_FMT(EXPANSIONINTERFACE, "Failed to load data of {}", gci.m_filename);
    return false;
  }

  // Lay the save out as one contiguous run in the primary BAT.
  const u16 start = m_bat1.AssignBlocksContiguous(blocks);
  if (start == 0xFFFF)
  {
    ERROR_LOG_FMT(
        EXPANSIONINTERFACE,
        "{}\nwas not loaded because there are not enough free blocks on the virtual memory card",
        gci.m_filename);
    return false;
  }
  gci.m_gci_header.m_first_block = start;

  // These games check that the save matches where it sits on this card.
  if (gci.HasCopyProtection())
  {
    Memcard::GCMemcard::PSO_MakeSaveGameValid(m_hdr, gci.m_gci_header, gci.m_save_data);
    Memcard::GCMemcard::FZEROGX_MakeSaveGameValid(m_hdr, gci.m_gci_header, gci.m_save_data);
  }

  const int slot = static_cast<int>(m_saves.size());
  m_dir1.Replace(gci.m_gci_header, slot);
  m_saves.push_back(std::move(gci));
  SetUsedBlocks(slot);
  return true;
}

std::vector<std::string> GCMemcardDirectory::GetFileNamesForGameID(const std::string& directory,
                                                                   const std::string& game_id)
{
  u32 wanted = 0;
  if (game_id.length() >= 4 && game_id != "00000000")
    wanted = Common::swap32(reinterpret_cast<const u8*>(game_id.c_str()));

  std::vector<std::string> matches;
  std::vector<Memcard::DEntry> taken;
  for (const std::string& path : Common::DoFileSearch(directory, ".gci"))
  {
    File::IOFile file(path, "rb");
    if (!file)
      continue;

    Memcard::DEntry entry;
    if (!file.ReadBytes(&entry, DENTRY_SIZE))
      continue;

    // Same filters LoadGCI applies, minus the free-space check: one game's
    // saves always fit.
    const bool duplicate = std::ranges::any_of(
        taken, [&entry](const Memcard::DEntry& other) { return HasSameIdentity(entry, other); });
    if (duplicate)
      continue;
    const u16 blocks = entry.m_block_count;
    if (blocks > MAX_SAVE_BLOCKS)
      continue;
    if (file.GetSize() != blocks * BLOCK_SIZE + DENTRY_SIZE)
      continue;

    if (GameCodeOf(entry) == wanted)
    {
      taken.push_back(entry);
      matches.push_back(path);
    }
  }
  return matches;
}

GCMemcardDirectory::GCMemcardDirectory(std::string directory, ExpansionInterface::Slot slot,
                                       const Memcard::HeaderData& header_data, u32 game_id)
    : MemoryCardBase(slot, header_data.m_size_mb), m_game_id(game_id), m_last_block(-1),
      m_last_block_address(nullptr), m_hdr(header_data), m_bat1(header_data.m_size_mb),
      m_save_directory(std::move(directory)), m_exiting(false)
{
  // A saved system area in the folder overrides the generated header.
  File::IOFile(m_save_directory + std::string(SYSTEM_AREA_FILE), "rb")
      .ReadBytes(&m_hdr, BLOCK_SIZE);

  // The running game's saves are loaded first and unconditionally; other
  // games' saves only while there is room to spare.
  const bool only_this_game = Config::Get(Config::SESSION_GCI_FOLDER_CURRENT_GAME_ONLY);
  std::vector<Memcard::GCIFile> mine;
  std::vector<Memcard::GCIFile> others;
  for (const std::string& path : Common::DoFileSearch(m_save_directory, ".gci"))
  {
    Memcard::GCIFile gci;
    gci.m_filename = path;
    gci.m_dirty = false;
    if (!gci.LoadHeader())
    {
      ERROR_LOG_FMT(EXPANSIONINTERFACE, "Failed to load header of {}", path);
      continue;
    }

    if (GameCodeOf(gci.m_gci_header) == m_game_id)
      mine.push_back(std::move(gci));
    else if (!only_this_game)
      others.push_back(std::move(gci));
  }

  m_saves.reserve(Memcard::DIRLEN);

  size_t mine_failed = 0;
  for (Memcard::GCIFile& gci : mine)
  {
    if (!LoadGCI(std::move(gci)))
      ++mine_failed;
  }

  // Keep roughly a tenth of the card free for saves the game may create.
  const int reserve = static_cast<int>(Memcard::MbitToFreeBlocks(m_hdr.m_data.m_size_mb)) / 10;
  for (Memcard::GCIFile& gci : others)
  {
    if (m_saves.size() > OTHER_GAME_ENTRY_LIMIT)
      break;
    const int free_after = int{m_bat1.m_free_blocks} - int{gci.m_gci_header.m_block_count};
    if (free_after < reserve)
      continue;
    LoadGCI(std::move(gci));
  }

  if (mine_failed != 0)
    Core::DisplayMessage("Warning: Save file(s) of the current game failed to load.", 10000);

  // Both copies of the system tables start out identical.
  m_dir1.FixChecksums();
  m_dir2 = m_dir1;
  m_bat2 = m_bat1;

  m_flush_thread = std::thread(&GCMemcardDirectory::FlushThread, this);
}

void GCMemcardDirectory::FlushThread()
{
  if (!Config::Get(Config::SESSION_SAVE_DATA_WRITABLE))
    return;

  Common::SetCurrentThreadName(fmt::format("Memcard {} flushing thread", m_card_slot).c_str());

  // Writes come in bursts; wait for a write, then for a full second without
  // one, before touching the disk.
  constexpr std::chrono::seconds quiet_period{1};
  for (;;)
  {
    m_flush_trigger.Wait();
    if (m_exiting.TestAndClear())
      return;

    bool still_busy;
    do
    {
      still_busy = m_flush_trigger.WaitFor(quiet_period);
      if (still_busy && m_exiting.TestAndClear())
        return;
    } while (still_busy);

    FlushToFile();
  }
}

GCMemcardDirectory::~GCMemcardDirectory()
{
  m_exiting.Set();
  m_flush_trigger.Set();
  m_flush_thread.join();

  FlushToFile();
}

bool GCMemcardDirectory::MapBlock(s32 block, bool writing)
{
  // The directories are never cached: accesses to them may need to be
  // intercepted (see Write), so m_last_block is invalidated for them.
  switch (block)
  {
  case HEADER_BLOCK:
    m_last_block = block;
    m_last_block_address = reinterpret_cast<u8*>(&m_hdr);
    return true;
  case DIRECTORY_BLOCK:
    m_last_block = -1;
    m_last_block_address = reinterpret_cast<u8*>(&m_dir1);
    return true;
  case DIRECTORY_BACKUP_BLOCK:
    m_last_block = -1;
    m_last_block_address = reinterpret_cast<u8*>(&m_dir2);
    return true;
  case BAT_BLOCK:
    m_last_block = block;
    m_last_block_address = reinterpret_cast<u8*>(&m_bat1);
    return true;
  case BAT_BACKUP_BLOCK:
    m_last_block = block;
    m_last_block_address = reinterpret_cast<u8*>(&m_bat2);
    return true;
  default:
    m_last_block = SaveAreaRW(static_cast<u32>(block), writing);
    return m_last_block != -1;
  }
}

s32 GCMemcardDirectory::Read(u32 src_address, s32 length, u8* dest_address)
{
  const s32 block = src_address / BLOCK_SIZE;
  const u32 offset = src_address % BLOCK_SIZE;

  // Anything past the end of this block is read by a recursive call.
  s32 overflow = 0;
  if (offset + length > BLOCK_SIZE)
  {
    overflow = static_cast<s32>(offset + length - BLOCK_SIZE);
    length -= overflow;
    DEBUG_ASSERT_MSG(EXPANSIONINTERFACE, (src_address + length) % BLOCK_SIZE == 0,
                     "Memcard directory Read Logic Error");
  }

  if (m_last_block != block && !MapBlock(block, false))
  {
    // Free space reads as erased flash.
    std::memset(dest_address, 0xFF, length);
    return 0;
  }

  std::memcpy(dest_address, m_last_block_address + offset, length);
  if (overflow != 0)
    overflow = Read(src_address + length, overflow, dest_address + length);
  return length + overflow;
}

s32 GCMemcardDirectory::Write(u32 dest_address, s32 length, const u8* src_address)
{
  std::unique_lock lock(m_write_mutex);
  const s32 block = dest_address / BLOCK_SIZE;
  const u32 offset = dest_address % BLOCK_SIZE;

  s32 overflow = 0;
  if (offset + length > BLOCK_SIZE)
  {
    overflow = static_cast<s32>(offset + length - BLOCK_SIZE);
    length -= overflow;
    DEBUG_ASSERT_MSG(EXPANSIONINTERFACE, (dest_address + length) % BLOCK_SIZE == 0,
                     "Memcard directory Write Logic Error");
  }

  if (m_last_block != block)
  {
    if (block == DIRECTORY_BLOCK || block == DIRECTORY_BACKUP_BLOCK)
    {
      // Directory writes go entry by entry so the save list can be kept in
      // step. Only the part inside this block is written.
      m_last_block = -1;
      s32 done = 0;
      for (s32 left = length; left > 0;)
      {
        const s32 piece = std::min<s32>(DENTRY_SIZE, left);
        done += DirectoryWrite(dest_address + done, piece, src_address + done);
        left -= piece;
      }
      return done;
    }

    if (!MapBlock(block, true))
    {
      PanicAlertFmtT("Report: GCIFolder Writing to unallocated block {0:#x}", block);
      std::exit(0);
    }
  }

  std::memcpy(m_last_block_address + offset, src_address, length);
  lock.unlock();

  if (overflow != 0)
    overflow = Write(dest_address + length, overflow, src_address + length);
  // A block written to its end is likely complete: schedule a flush.
  if (offset + length == BLOCK_SIZE)
    m_flush_trigger.Set();
  return length + overflow;
}

void GCMemcardDirectory::ClearBlock(u32 address)
{
  if (address % BLOCK_SIZE != 0)
  {
    PanicAlertFmtT("GCMemcardDirectory: ClearBlock called with invalid block address");
    return;
  }

  const u32 block = address / BLOCK_SIZE;
  INFO_LOG_FMT(EXPANSIONINTERFACE, "Erasing card block {}", block);
  if (MapBlock(static_cast<s32>(block), true))
    std::memset(m_last_block_address, 0xFF, BLOCK_SIZE);
}

inline void GCMemcardDirectory::SyncSaves()
{
  // Bring m_saves in line with the directory the game wrote last.
  const Memcard::Directory& dir = Newest(m_dir1, m_dir2);

  for (u32 index = 0; index < Memcard::DIRLEN; ++index)
  {
    const Memcard::DEntry& entry = dir.m_dir_entries[index];

    if (IsUnused(entry))
    {
      // Entry removed: forget the save (FlushToFile retires its file).
      if (index < m_saves.size() && !IsUnused(m_saves[index].m_gci_header))
      {
        Memcard::GCIFile& save = m_saves[index];
        INFO_LOG_FMT(EXPANSIONINTERFACE, "Save {:#x} was deleted by the game",
                     GameCodeOf(save.m_gci_header));
        save.m_gci_header.m_gamecode = Memcard::DEntry::UNINITIALIZED_GAMECODE;
        save.m_save_data.clear();
        save.m_used_blocks.clear();
        save.m_dirty = true;
      }
      continue;
    }

    bool fresh = false;
    if (index >= m_saves.size())
    {
      m_saves.resize(index + 1);
      fresh = true;
    }

    Memcard::GCIFile& save = m_saves[index];
    if (!fresh && std::memcmp(&save.m_gci_header, &entry, DENTRY_SIZE) == 0)
      continue;

    save.m_dirty = true;
    const u32 old_code = GameCodeOf(save.m_gci_header);
    const u32 new_code = GameCodeOf(entry);
    const u32 old_first = save.m_gci_header.m_first_block;
    const u32 new_first = entry.m_first_block;
    if (old_code != GAMECODE_UNUSED && old_code != new_code)
    {
      PanicAlertFmtT(
          "Game overwrote with another games save. Data corruption ahead {0:#x}, {1:#x}",
          old_code, new_code);
    }

    std::memcpy(&save.m_gci_header, &entry, DENTRY_SIZE);
    if (old_first != new_first)
    {
      // Relocated: the cached block list and data no longer apply.
      INFO_LOG_FMT(EXPANSIONINTERFACE, "Save now starts at block {:#x} (was {:#x})", new_first,
                   old_first);
      save.m_used_blocks.clear();
      save.m_save_data.clear();
    }
    if (save.m_used_blocks.empty())
      SetUsedBlocks(static_cast<int>(index));
  }
}

inline s32 GCMemcardDirectory::SaveAreaRW(u32 block, bool writing)
{
  for (u16 n = 0; n < m_saves.size(); ++n)
  {
    Memcard::GCIFile& save = m_saves[n];
    if (IsUnused(save.m_gci_header))
      continue;

    if (save.m_used_blocks.empty())
      SetUsedBlocks(n);

    const int position = save.UsesBlock(static_cast<u16>(block));
    if (position < 0)
      continue;

    // Data that can't be read from disk is presented as erased blocks.
    if (!save.LoadSaveBlocks())
      save.m_save_data.resize(save.m_save_data.size() + save.m_gci_header.m_block_count);

    if (writing)
      save.m_dirty = true;

    if (static_cast<size_t>(position) >= save.m_save_data.size())
    {
      PanicAlertFmt("Block index ({0}) is larger than the number of available blocks ({1})",
                    position, save.m_save_data.size());
      return -1;
    }

    m_last_block = static_cast<s32>(block);
    m_last_block_address = save.m_save_data[position].m_block.data();
    return m_last_block;
  }
  return -1;
}

s32 GCMemcardDirectory::DirectoryWrite(u32 dest_address, u32 length, const u8* src_address)
{
  const u32 offset = dest_address % BLOCK_SIZE;
  Memcard::Directory& dir = (dest_address / BLOCK_SIZE == DIRECTORY_BLOCK) ? m_dir1 : m_dir2;
  std::memcpy(reinterpret_cast<u8*>(&dir) + offset, src_address, length);

  // The last 0x40 bytes hold the update counter and checksums; the game
  // writes them after the entries, so that is when the saves are re-synced.
  if (offset / DENTRY_SIZE == Memcard::DIRLEN)
    SyncSaves();
  return static_cast<s32>(length);
}

bool GCMemcardDirectory::SetUsedBlocks(int save_index)
{
  const Memcard::BlockAlloc& bat = Newest(m_bat2, m_bat1);
  Memcard::GCIFile& save = m_saves[save_index];

  for (u16 block = save.m_gci_header.m_first_block; block != 0xFFFF;)
  {
    save.m_used_blocks.push_back(block);
    block = bat.GetNextBlock(block);
    if (block == 0)
    {
      PanicAlertFmtT("BAT incorrect. Dolphin will now exit");
      std::exit(0);
    }
  }

  const u16 from_bat = static_cast<u16>(save.m_used_blocks.size());
  const u16 from_entry = save.m_gci_header.m_block_count;
  if (from_bat == from_entry)
    return true;

  PanicAlertFmtT("Warning: Number of blocks indicated by the BAT ({0}) does not match that of the "
                 "loaded file header ({1})",
                 from_bat, from_entry);
  return false;
}

void GCMemcardDirectory::FlushToFile()
{
  std::unique_lock lock(m_write_mutex);

  for (Memcard::GCIFile& save : m_saves)
  {
    if (save.m_dirty && !IsUnused(save.m_gci_header))
    {
      save.m_dirty = false;
      if (save.m_save_data.empty())
      {
        // Only the entry changed so far; write the file once data arrives.
        ERROR_LOG_FMT(EXPANSIONINTERFACE,
                      "GCI header modified without corresponding save data changes");
        continue;
      }

      if (save.m_filename.empty())
      {
        // New save: pick "<maker>-<game>-<name>.gci", padding with zeros
        // before the extension if that name is already taken.
        std::string path =
            m_save_directory + DefaultGciName(save.m_gci_header, m_hdr.IsShiftJIS());
        for (int attempt = 0; attempt < 10 && File::Exists(path); ++attempt)
          path.insert(path.size() - 4, 1, '0');
        if (File::Exists(path))
          PanicAlertFmtT("Failed to find new filename.\n{0}\n will be overwritten", path);
        save.m_filename = std::move(path);
      }

      File::IOFile out(save.m_filename, "wb");
      if (!out)
      {
        Core::DisplayMessage(fmt::format("Failed to open file at {} for writing", save.m_filename),
                             10000);
        ERROR_LOG_FMT(EXPANSIONINTERFACE, "Could not open {} for writing", save.m_filename);
      }
      else
      {
        out.WriteBytes(&save.m_gci_header, DENTRY_SIZE);
        for (const Memcard::GCMBlock& data : save.m_save_data)
          out.WriteBytes(data.m_block.data(), BLOCK_SIZE);

        if (out.IsGood())
        {
          Core::DisplayMessage("Wrote save contents to GCI Folder", 4000);
        }
        else
        {
          Core::DisplayMessage(fmt::format("Failed to write save contents to {}", save.m_filename),
                               10000);
          ERROR_LOG_FMT(EXPANSIONINTERFACE, "Writing {} failed", save.m_filename);
        }
      }
    }
    else if (save.m_dirty && !save.m_filename.empty())
    {
      // Deleted by the game: keep the old file around as "<name>.deleted".
      save.m_dirty = false;
      const std::string retired = save.m_filename + ".deleted";
      if (File::Exists(retired))
        File::Delete(retired);
      File::Rename(save.m_filename, retired);
      save.m_filename.clear();
      save.m_save_data.clear();
      save.m_used_blocks.clear();
    }

    // Other games' data is dropped from memory after each flush (it is
    // reloaded on demand); the running game's stays so savestates have it.
    const u32 code = GameCodeOf(save.m_gci_header);
    if (code != m_game_id && code != GAMECODE_UNUSED && !save.m_save_data.empty())
    {
      INFO_LOG_FMT(EXPANSIONINTERFACE, "Unloading save data of {}", save.m_filename);
      save.m_save_data.clear();
    }
  }
}

void GCMemcardDirectory::DoState(PointerWrap& p)
{
  std::unique_lock lock(m_write_mutex);

  // Loading replaces the saves, so the cached block pointer can't survive.
  m_last_block = -1;
  m_last_block_address = nullptr;

  // Folder path, then the system area, then every save (savestate order).
  const auto save_all = [&p](auto&... parts) { (p.Do(parts), ...); };
  save_all(m_save_directory, m_hdr, m_dir1, m_dir2, m_bat1, m_bat2);
  p.DoEachElement(m_saves, [](PointerWrap& pw, Memcard::GCIFile& gci) { gci.DoState(pw); });
}

void MigrateFromMemcardFile(const std::string& directory_name, ExpansionInterface::Slot card_slot,
                            DiscIO::Region region)
{
  File::CreateFullPath(directory_name);
  const std::string card_path = Config::GetMemcardPath(card_slot, region);
  if (!File::Exists(card_path))
    return;

  auto [status, card] = Memcard::GCMemcard::Open(card_path.c_str());
  if (status.HasCriticalErrors() || !card || !card->IsValid())
    return;

  for (u8 index = 0; index < Memcard::DIRLEN; ++index)
  {
    const auto save = card->ExportFile(index);
    if (!save)
      continue;
    const std::string target =
        directory_name + DIR_SEP + Memcard::GenerateFilename(save->dir_entry) + ".gci";
    Memcard::WriteSavefile(target, *save, Memcard::SavefileFormat::GCI);
  }
}
