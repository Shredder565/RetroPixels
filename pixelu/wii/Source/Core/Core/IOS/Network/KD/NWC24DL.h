// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "Common/CommonTypes.h"
#include "Core/IOS/Network/KD/WC24File.h"

namespace IOS::HLE
{
namespace FS
{
class FileSystem;
}
namespace NWC24
{
// The WiiConnect24 download task list (nwc24dl.bin). Values stay in file (big endian) order and
// are converted by the accessors; times are handed out in seconds although the file holds
// minutes. If the list cannot be loaded it is deleted and the object stays disabled; nothing
// but IsDisabled may then be called.
class NWC24Dl final
{
public:
  explicit NWC24Dl(std::shared_ptr<FS::FileSystem> fs);

  bool ReadDlList();
  void WriteDlList() const;

  bool CheckNwc24DlList() const;
  bool IsDisabled() const;

  bool DoesEntryExist(u16 entry_index) const;
  bool IsEncrypted(u16 entry_index) const;
  bool IsRSASigned(u16 entry_index) const;
  bool SkipSchedulerDownload(u16 entry_index) const;
  bool HasSubtask(u16 entry_index) const;
  bool IsSubtaskDownloadDisabled(u16 entry_index) const;
  bool IsValidSubtask(u16 entry_index, u8 subtask_id) const;
  std::string GetVFFContentName(u16 entry_index, std::optional<u8> subtask_id) const;
  std::string GetDownloadURL(u16 entry_index, std::optional<u8> subtask_id) const;
  std::string GetVFFPath(u16 entry_index) const;
  std::optional<WC24PubkMod> GetWC24PubkMod(u16 entry_index) const;

  u64 GetNextDownloadTime(u16 record_index) const;
  u64 GetDownloadMargin(u16 entry_index) const;
  void SetNextDownloadTime(u16 record_index, u64 value, std::optional<u8> subtask_id);
  u64 GetRetryTime(u16 entry_index) const;
  u64 GetLastSubtaskDownloadTime(u16 entry_index, u8 subtask_id) const;

  u32 Magic() const;
  void SetMagic(u32 magic);

  u32 Version() const;
  void SetVersion(u32 version);

  static constexpr u32 MAX_ENTRIES = 120;

private:
  static constexpr u32 DL_LIST_MAGIC = 0x5763446C;  // "WcDl"
  static constexpr u32 MAX_SUBENTRIES = 32;
  static constexpr u64 SECONDS_PER_MINUTE = 60;
  static constexpr u64 MINUTES_PER_DAY = 1440;

  enum EntryType : u8
  {
    SUBTASK = 1,
    MAIL,
    CHANNEL_CONTENT,
    UNUSED = 0xff
  };

  // File layout (see the /dev/net/kd/request page on WiiBrew).
#pragma pack(push, 1)
  struct DLListHeader final
  {
    u32 magic;
    u32 version;  // 1
    u32 unk1;
    u32 unk2;
    u16 max_subentries;
    u16 reserved_mailnum;
    u16 max_entries;
    u8 reserved[106];
  };

  // Scheduling record of one entry.
  struct DLListRecord final
  {
    u32 low_title_id;
    u32 next_dl_timestamp;  // minutes since 1970
    u32 last_modified_timestamp;
    u8 flags;
    u8 padding[3];
  };

  // One download task.
  struct DLListEntry final
  {
    u16 index;
    EntryType type;
    u8 record_flags;
    u32 flags;  // bit 2: not RSA signed, bit 3: encrypted, bit 5: not for the scheduler
    u32 high_title_id;
    u32 low_title_id;
    u32 unknown1;
    u16 group_id;
    u16 padding1;
    u16 remaining_downloads;
    u16 error_count;
    u16 dl_margin;        // minutes
    u16 retry_frequency;  // minutes; 0 means once a day
    s32 error_code;
    u8 subtask_id;
    u8 subtask_type;
    u16 subtask_flags;     // bit 9: subtask downloads disabled
    u32 subtask_bitmask;   // bit 0/1: add the subtask number to the file name/URL
    u32 server_dl_interval;
    u32 dl_timestamp;
    u32 subtask_timestamps[32];
    char dl_url[236];
    char filename[64];
    u8 unk6[29];
    u8 should_use_rootca;
    u16 unknown3;
  };

  struct DLList final
  {
    DLListHeader header;
    DLListRecord records[MAX_ENTRIES];
    DLListEntry entries[MAX_ENTRIES];
  };
#pragma pack(pop)
  static_assert(sizeof(DLListHeader) == 128);
  static_assert(sizeof(DLListRecord) == 16);
  static_assert(sizeof(DLListEntry) == 512);

  const DLListEntry& Entry(u16 entry_index) const;

  std::shared_ptr<FS::FileSystem> m_fs;
  DLList m_data;
  bool m_is_disabled = false;
};
}  // namespace NWC24
}  // namespace IOS::HLE
