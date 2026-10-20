// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/KD/Mail/WC24Send.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Network/KD/VFF/VFFUtil.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE::NWC24::Mail
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
constexpr const char SEND_LIST_PATH[] = "/" WII_WC24CONF_DIR "/mbox"
                                        "/wc24send.ctl";

constexpr u32 SEND_LIST_VERSION = 4;
constexpr u32 ENTRY_SIZE = sizeof(MailListEntry);
// At most this many messages go out per send.
constexpr u32 MAX_MAILS_PER_SEND = 16;

// The friend registration request the Wii Message Board sends: {0} is the sender's ID, {1}
// the friend's, {2} the current UTC time. The attachment (friend profile data) is always 128
// zero bytes. The text, including the missing '@' in RCPT TO, is what the console sends.
constexpr char REGISTRATION_MAIL[] =
    "MAIL FROM: {0:016d}@wii.com\r\n"
    "RCPT TO: {1:016d}wii.com\r\n"
    "DATA\r\n"
    "Date: {2:%a, %d %b %Y %X} GMT\r\n"
    "From: {0:016d}@wii.com\r\n"
    "To: {1:016d}@wii.com\r\n"
    "Message-Id: <00002000B0DF6BB47FE0303E0DB0D@wii.com>\r\n"
    "Subject: WC24 Cmd Message\r\n"
    "X-Wii-AppId: 0-00000001-0001\r\n"
    "X-Wii-Cmd: 80010001\r\n"
    "MIME-Version: 1.0\r\n"
    "Content-Type: multipart/mixed;\r\n "
    "boundary=\"Boundary-NWC24-041B6CE500012\"\r\n"
    "--Boundary-NWC24-041B6CE500012\r\n"
    "Content-Type: text/plain; charset=us-ascii\r\n"
    "Content-Transfer-Encoding: 7bit\r\n"
    "WC24 Cmd Message\r\n"
    "--Boundary-NWC24-041B6CE500012\r\n"
    "Content-Type: application/octet-stream;\r\n "
    "name=a0000018.dat\r\n"
    "Content-Transfer-Encoding: base64\r\n"
    "Content-Disposition: attachment;\r\n "
    "filename=a0000018.dat\r\n\r\n "
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="
    "\r\n\r\n"
    "--Boundary-NWC24-041B6CE500012--";
}  // namespace

WC24SendList::WC24SendList(std::shared_ptr<FS::FileSystem> fs) : m_fs{std::move(fs)}
{
  if (ReadSendList())
    return;

  ERROR_LOG_FMT(IOS_WC24, "There is an error in the Send List for WC24 mail. Mail will be "
                          "unavailable for this IOS session.");
  m_is_disabled = true;

  // Remove the broken list.
  const FS::ResultCode deleted = m_fs->Delete(PID_KD, PID_KD, SEND_LIST_PATH);
  if (deleted != FS::ResultCode::Success && deleted != FS::ResultCode::NotFound)
    ERROR_LOG_FMT(IOS_WC24, "Failed to delete the Send list.");
}

void WC24SendList::PointAtFreeEntry()
{
  const std::optional<u32> free_index = GetNextFreeEntryIndex();
  // With no free entry, the oldest one (the first) gets overwritten.
  m_data.header.next_entry_offset = CalculateFileOffset(free_index.value_or(0));
}

bool WC24SendList::ReadSendList()
{
  const auto file = m_fs->OpenFile(PID_KD, PID_KD, SEND_LIST_PATH, FS::Mode::Read);
  if (!file || !file->Read(reinterpret_cast<u8*>(&m_data), sizeof(m_data)))
  {
    ERROR_LOG_FMT(IOS_WC24, "Failed to read the Send list");
    return false;
  }

  if (file->GetStatus()->size != SEND_LIST_SIZE)
  {
    ERROR_LOG_FMT(IOS_WC24, "The WC24 Send list file is not the correct size.");
    return false;
  }

  // Repair a next-entry offset that is not on an entry boundary or lies past the last entry.
  // The value is checked as stored, without a byte swap, which is how it has always been
  // checked; any non-trivial big-endian value therefore gets repaired.
  const u32 raw_offset = m_data.header.next_entry_offset;
  if (raw_offset % ENTRY_SIZE != 0 || raw_offset > ENTRY_SIZE * MAX_ENTRIES)
    PointAtFreeEntry();

  // A list with the wrong magic or version is used anyway.
  if (!CheckSendList())
    ERROR_LOG_FMT(IOS_WC24, "There is an error in the Send List for WC24 mail");
  return true;
}

bool WC24SendList::IsDisabled() const
{
  return m_is_disabled;
}

void WC24SendList::WriteSendList() const
{
  ASSERT(!IsDisabled());
  constexpr FS::Modes everyone_rw{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};
  m_fs->CreateFullPath(PID_KD, PID_KD, SEND_LIST_PATH, 0, everyone_rw);
  const auto file = m_fs->CreateAndOpenFile(PID_KD, PID_KD, SEND_LIST_PATH, everyone_rw);
  const bool written = file && file->Write(reinterpret_cast<const u8*>(&m_data), sizeof(m_data));
  if (!written)
    ERROR_LOG_FMT(IOS_WC24, "Failed to open or write WC24 Send list file");
}

bool WC24SendList::CheckSendList() const
{
  const u32 magic = ByteSwapped(m_data.header.magic);
  if (magic != MAIL_LIST_MAGIC)
  {
    ERROR_LOG_FMT(IOS_WC24, "Send List magic mismatch ({} != {})", magic, MAIL_LIST_MAGIC);
    return false;
  }
  if (ByteSwapped(m_data.header.version) != SEND_LIST_VERSION)
  {
    ERROR_LOG_FMT(IOS_WC24, "Send List version mismatch");
    return false;
  }
  return true;
}

u32 WC24SendList::GetNumberOfMail() const
{
  ASSERT(!IsDisabled());
  return ByteSwapped(m_data.header.number_of_mail);
}

u32 WC24SendList::GetEntryId(u32 entry_index) const
{
  ASSERT(!IsDisabled());
  return ByteSwapped(m_data.entries[entry_index].id);
}

u32 WC24SendList::GetMailSize(u32 index) const
{
  ASSERT(!IsDisabled());
  return ByteSwapped(m_data.entries[index].msg_size);
}

ErrorCode WC24SendList::DeleteMessage(u32 index)
{
  ASSERT(!IsDisabled());
  const ErrorCode deleted = DeleteFileFromVFF(SEND_BOX_PATH, GetMailPath(index), m_fs);
  if (deleted != WC24_OK)
    return deleted;

  // The freed entry is reused next, with the same ID.
  MailListHeader& header = m_data.header;
  header.number_of_mail = ByteSwapped(GetNumberOfMail() - 1);
  header.next_entry_id = ByteSwapped(GetEntryId(index));
  header.next_entry_offset = CalculateFileOffset(index);
  // Stored without converting back to big endian (long-standing behaviour); nothing reads it.
  header.total_size_of_messages =
      ByteSwapped(header.total_size_of_messages) - GetMailSize(index);

  m_data.entries[index] = {};
  return WC24_OK;
}

std::string WC24SendList::GetMailPath(u32 index) const
{
  return fmt::format("mb/s{:07d}.msg", GetEntryId(index));
}

u32 WC24SendList::GetNextEntryId() const
{
  ASSERT(!IsDisabled());
  return ByteSwapped(m_data.header.next_entry_id);
}

u32 WC24SendList::GetNextEntryIndex() const
{
  ASSERT(!IsDisabled());
  // Offsets below the first entry wrap around (u32), as in IOS.
  return (ByteSwapped(m_data.header.next_entry_offset) - ENTRY_SIZE) / ENTRY_SIZE;
}

std::vector<u32> WC24SendList::GetMailToSend() const
{
  ASSERT(!IsDisabled());
  // Used entries need not be next to each other.
  const u32 wanted = std::min(GetNumberOfMail(), MAX_MAILS_PER_SEND);
  std::vector<u32> indices;
  for (u32 index = 0; index < MAX_ENTRIES && indices.size() < wanted; ++index)
  {
    if (GetEntryId(index) != 0)
      indices.push_back(index);
  }
  return indices;
}

std::optional<u32> WC24SendList::GetNextFreeEntryIndex() const
{
  for (u32 index = 0; index < MAX_ENTRIES; ++index)
  {
    if (GetEntryId(index) == 0)
      return index;
  }
  return std::nullopt;
}

ErrorCode WC24SendList::AddRegistrationMessages(const WC24FriendList& friend_list, u64 sender)
{
  ASSERT(!IsDisabled());
  // Pick up anything the user queued since the list was loaded.
  ReadSendList();

  for (const u64 friend_code : friend_list.GetUnconfirmedFriends())
  {
    const u32 index = GetNextEntryIndex();
    const u32 id = GetNextEntryId();
    MailListEntry& entry = m_data.entries[index];
    entry.id = ByteSwapped(id);

    const std::string mail =
        fmt::format(REGISTRATION_MAIL, sender, friend_code, fmt::gmtime(std::time(nullptr)));
    const u32 mail_size = static_cast<u32>(mail.size());
    const ErrorCode written = WriteToVFF(
        SEND_BOX_PATH, GetMailPath(index), m_fs,
        std::span<const u8>{reinterpret_cast<const u8*>(mail.data()), mail.size()});
    if (written != WC24_OK)
    {
      // The list is not saved, so the half-made entry is forgotten.
      ERROR_LOG_FMT(IOS_WC24, "Error writing registration message to VFF");
      return written;
    }
    NOTICE_LOG_FMT(IOS_WC24, "Issued registration message for Wii Friend: {}", friend_code);

    entry.msg_size = ByteSwapped(mail_size);
    MailListHeader& header = m_data.header;
    header.number_of_mail = ByteSwapped(GetNumberOfMail() + 1);
    header.next_entry_id = ByteSwapped(id + 1);
    // Host order, like in DeleteMessage.
    header.total_size_of_messages = ByteSwapped(header.total_size_of_messages) + mail_size;
    PointAtFreeEntry();
  }

  WriteSendList();
  return WC24_OK;
}

std::string_view WC24SendList::GetMailFlag() const
{
  ASSERT(!IsDisabled());
  return {m_data.header.mail_flag.data(), m_data.header.mail_flag.size()};
}
}  // namespace IOS::HLE::NWC24::Mail
