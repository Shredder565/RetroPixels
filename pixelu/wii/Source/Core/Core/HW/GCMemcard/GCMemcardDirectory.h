// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Common/Event.h"
#include "Core/HW/GCMemcard/GCIFile.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardBase.h"
#include "DiscIO/Enums.h"

// Exports every save of the configured raw card image for this slot/region
// into a GCI folder (used when switching a slot from raw card to folder).
void MigrateFromMemcardFile(const std::string& directory_name, ExpansionInterface::Slot card_slot,
                            DiscIO::Region region);

// A memory card synthesised from a folder of .gci files. The system area
// (header, directories, BATs) is built in memory when the card is created;
// each loaded save keeps its own blocks, and the game's writes to the
// directory are mirrored back onto the individual GCI files.
class GCMemcardDirectory : public MemoryCardBase
{
public:
  GCMemcardDirectory(std::string directory, ExpansionInterface::Slot slot,
                     const Memcard::HeaderData& header_data, u32 game_id);
  ~GCMemcardDirectory() override;

  GCMemcardDirectory(const GCMemcardDirectory&) = delete;
  GCMemcardDirectory& operator=(const GCMemcardDirectory&) = delete;
  GCMemcardDirectory(GCMemcardDirectory&&) = delete;
  GCMemcardDirectory& operator=(GCMemcardDirectory&&) = delete;

  // The .gci files in a folder that belong to one game (for NetPlay sync).
  static std::vector<std::string> GetFileNamesForGameID(const std::string& directory,
                                                        const std::string& game_id);
  void FlushToFile();
  void FlushThread();
  s32 Read(u32 src_address, s32 length, u8* dest_address) override;
  s32 Write(u32 dest_address, s32 length, const u8* src_address) override;
  void ClearBlock(u32 address) override;
  void ClearAll() override {}
  void DoState(PointerWrap& p) override;

private:
  bool LoadGCI(Memcard::GCIFile gci);
  inline s32 SaveAreaRW(u32 block, bool writing = false);
  s32 DirectoryWrite(u32 dest_address, u32 length, const u8* src_address);
  inline void SyncSaves();
  bool SetUsedBlocks(int save_index);

  // Points m_last_block_address at the storage behind a card block. Returns
  // false for a data block that no save owns.
  bool MapBlock(s32 block, bool writing);

  u32 m_game_id;
  s32 m_last_block;
  u8* m_last_block_address;

  Memcard::Header m_hdr;
  Memcard::Directory m_dir1;
  Memcard::Directory m_dir2;
  Memcard::BlockAlloc m_bat1;
  Memcard::BlockAlloc m_bat2;
  std::vector<Memcard::GCIFile> m_saves;

  std::string m_save_directory;
  Common::Event m_flush_trigger;
  std::mutex m_write_mutex;
  Common::Flag m_exiting;
  std::thread m_flush_thread;
};
