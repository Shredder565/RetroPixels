// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/KD/NWC24DL.h"

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE::NWC24
{
namespace
{
// Every multi-byte field of these files is stored big endian.
u32 ByteSwapped(u32 value)
{
  return Common::swap32(value);
}
}  // namespace

namespace
{
constexpr const char DL_LIST_PATH[] = "/" WII_WC24CONF_DIR "/nwc24dl.bin";

constexpr bool Bit(u32 value, u32 bit)
{
  return ((value >> bit) & 1) != 0;
}

// "<name>.NN" when the entry asks for the subtask number to be added.
std::string WithSubtask(std::string name, std::optional<u8> subtask_id, bool add_number)
{
  if (subtask_id && add_number)
    name += fmt::format(".{:02d}", *subtask_id);
  return name;
}
}  // namespace

NWC24Dl::NWC24Dl(std::shared_ptr<FS::FileSystem> fs) : m_fs{std::move(fs)}
{
  if (ReadDlList())
    return;

  ERROR_LOG_FMT(IOS_WC24, "There is an error in the DL list for WC24. WiiConnect24 downloading "
                          "will be unavailable for this current IOS session.");
  m_is_disabled = true;

  // Remove the broken list so that a new one can be made. The system menu reloads IOS, so the
  // previous load may already have removed it.
  const FS::ResultCode deleted = m_fs->Delete(PID_KD, PID_KD, DL_LIST_PATH);
  if (deleted != FS::ResultCode::Success && deleted != FS::ResultCode::NotFound)
    ERROR_LOG_FMT(IOS_WC24, "Failed to delete the DL list.");
}

bool NWC24Dl::ReadDlList()
{
  const auto file = m_fs->OpenFile(PID_KD, PID_KD, DL_LIST_PATH, FS::Mode::Read);
  const bool loaded = file && file->Read(reinterpret_cast<u8*>(&m_data), sizeof(m_data));
  if (!loaded)
  {
    ERROR_LOG_FMT(IOS_WC24, "Failed to read the DL list");
    return false;
  }
  return CheckNwc24DlList();
}

bool NWC24Dl::CheckNwc24DlList() const
{
  if (Magic() != DL_LIST_MAGIC)
  {
    ERROR_LOG_FMT(IOS_WC24, "DL list magic mismatch");
    return false;
  }
  if (Version() != 1)
  {
    ERROR_LOG_FMT(IOS_WC24, "DL list version mismatch");
    return false;
  }
  return true;
}

bool NWC24Dl::IsDisabled() const
{
  return m_is_disabled;
}

const NWC24Dl::DLListEntry& NWC24Dl::Entry(u16 entry_index) const
{
  ASSERT(!IsDisabled());
  return m_data.entries[entry_index];
}

void NWC24Dl::WriteDlList() const
{
  ASSERT(!IsDisabled());
  constexpr FS::Modes everyone_rw{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};
  m_fs->CreateFullPath(PID_KD, PID_KD, DL_LIST_PATH, 0, everyone_rw);
  const auto file = m_fs->CreateAndOpenFile(PID_KD, PID_KD, DL_LIST_PATH, everyone_rw);
  const bool written = file && file->Write(reinterpret_cast<const u8*>(&m_data), sizeof(m_data));
  if (!written)
    ERROR_LOG_FMT(IOS_WC24, "Failed to open or write WC24 DL list file");
}

bool NWC24Dl::DoesEntryExist(u16 entry_index) const
{
  return Entry(entry_index).low_title_id != 0;
}

std::string NWC24Dl::GetDownloadURL(u16 entry_index, std::optional<u8> subtask_id) const
{
  const DLListEntry& entry = Entry(entry_index);
  return WithSubtask(entry.dl_url, subtask_id, Bit(ByteSwapped(entry.subtask_bitmask), 1));
}

std::string NWC24Dl::GetVFFContentName(u16 entry_index, std::optional<u8> subtask_id) const
{
  const DLListEntry& entry = Entry(entry_index);
  return WithSubtask(entry.filename, subtask_id, Bit(ByteSwapped(entry.subtask_bitmask), 0));
}

std::string NWC24Dl::GetVFFPath(u16 entry_index) const
{
  const DLListEntry& entry = Entry(entry_index);
  // Note the order: the low half of the title ID comes first in this path.
  return fmt::format("/title/{:08x}/{:08x}/data/wc24dl.vff", ByteSwapped(entry.low_title_id),
                     ByteSwapped(entry.high_title_id));
}

std::optional<WC24PubkMod> NWC24Dl::GetWC24PubkMod(u16 entry_index) const
{
  const DLListEntry& entry = Entry(entry_index);
  const std::string path =
      fmt::format("/title/{:08x}/{:08x}/data/wc24pubk.mod", ByteSwapped(entry.low_title_id),
                  ByteSwapped(entry.high_title_id));

  const auto file = m_fs->OpenFile(PID_KD, PID_KD, path, FS::Mode::Read);
  WC24PubkMod contents;
  if (!file || !file->Read(reinterpret_cast<u8*>(&contents), sizeof(contents)))
    return std::nullopt;
  return contents;
}

bool NWC24Dl::IsEncrypted(u16 entry_index) const
{
  return Bit(ByteSwapped(Entry(entry_index).flags), 3);
}

bool NWC24Dl::IsRSASigned(u16 entry_index) const
{
  return !Bit(ByteSwapped(Entry(entry_index).flags), 2);
}

bool NWC24Dl::SkipSchedulerDownload(u16 entry_index) const
{
  return Bit(ByteSwapped(Entry(entry_index).flags), 5);
}

bool NWC24Dl::HasSubtask(u16 entry_index) const
{
  // Subtask types 1 to 4 exist.
  const u8 type = Entry(entry_index).subtask_type;
  return type >= 1 && type <= 4;
}

bool NWC24Dl::IsSubtaskDownloadDisabled(u16 entry_index) const
{
  return Bit(Common::swap16(Entry(entry_index).subtask_flags), 9);
}

bool NWC24Dl::IsValidSubtask(u16 entry_index, u8 subtask_id) const
{
  // The mask is tested as stored, without a byte swap.
  return Bit(Entry(entry_index).subtask_bitmask, subtask_id);
}

u64 NWC24Dl::GetNextDownloadTime(u16 record_index) const
{
  ASSERT(!IsDisabled());
  return ByteSwapped(m_data.records[record_index].next_dl_timestamp) * SECONDS_PER_MINUTE;
}

u64 NWC24Dl::GetRetryTime(u16 entry_index) const
{
  const u64 minutes = Common::swap16(Entry(entry_index).retry_frequency);
  return (minutes == 0 ? MINUTES_PER_DAY : minutes) * SECONDS_PER_MINUTE;
}

u64 NWC24Dl::GetDownloadMargin(u16 entry_index) const
{
  return Common::swap16(Entry(entry_index).dl_margin) * SECONDS_PER_MINUTE;
}

void NWC24Dl::SetNextDownloadTime(u16 record_index, u64 value, std::optional<u8> subtask_id)
{
  ASSERT(!IsDisabled());
  const u32 minutes_be = ByteSwapped(static_cast<u32>(value / SECONDS_PER_MINUTE));
  if (subtask_id)
    m_data.entries[record_index].subtask_timestamps[*subtask_id] = minutes_be;
  m_data.records[record_index].next_dl_timestamp = minutes_be;
}

u64 NWC24Dl::GetLastSubtaskDownloadTime(u16 entry_index, u8 subtask_id) const
{
  const DLListEntry& entry = Entry(entry_index);
  const u64 last = ByteSwapped(entry.subtask_timestamps[subtask_id]);
  const u64 interval = ByteSwapped(entry.server_dl_interval);
  return (last + interval) * SECONDS_PER_MINUTE;
}

u32 NWC24Dl::Magic() const
{
  ASSERT(!IsDisabled());
  return ByteSwapped(m_data.header.magic);
}

void NWC24Dl::SetMagic(u32 magic)
{
  ASSERT(!IsDisabled());
  m_data.header.magic = ByteSwapped(magic);
}

u32 NWC24Dl::Version() const
{
  ASSERT(!IsDisabled());
  return ByteSwapped(m_data.header.version);
}

void NWC24Dl::SetVersion(u32 version)
{
  ASSERT(!IsDisabled());
  m_data.header.version = ByteSwapped(version);
}
}  // namespace IOS::HLE::NWC24
