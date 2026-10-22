// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/KD/Mail/WC24FriendList.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/Uids.h"

namespace IOS::HLE::NWC24::Mail
{
WC24FriendList::WC24FriendList(std::shared_ptr<FS::FileSystem> fs) : m_fs{std::move(fs)}
{
  ReadFriendList();
}

void WC24FriendList::ReadFriendList()
{
  // A missing or short file leaves the list as it was.
  const auto file = m_fs->OpenFile(PID_KD, PID_KD, FRIEND_LIST_PATH, FS::Mode::Read);
  if (!file || !file->Read(reinterpret_cast<u8*>(&m_data), sizeof(m_data)))
    return;

  if (!CheckFriendList())
    ERROR_LOG_FMT(IOS_WC24, "There is an error in the Receive List for WC24 mail");
}

void WC24FriendList::WriteFriendList() const
{
  constexpr FS::Modes everyone_rw{FS::Mode::ReadWrite, FS::Mode::ReadWrite, FS::Mode::ReadWrite};
  m_fs->CreateFullPath(PID_KD, PID_KD, FRIEND_LIST_PATH, 0, everyone_rw);
  const auto file = m_fs->CreateAndOpenFile(PID_KD, PID_KD, FRIEND_LIST_PATH, everyone_rw);
  const bool written = file && file->Write(reinterpret_cast<const u8*>(&m_data), sizeof(m_data));
  if (!written)
    ERROR_LOG_FMT(IOS_WC24, "Failed to open or write WC24 Receive list file");
}

bool WC24FriendList::CheckFriendList() const
{
  const u32 magic = Common::swap32(m_data.header.magic);
  if (magic == FRIEND_LIST_MAGIC)
    return true;
  ERROR_LOG_FMT(IOS_WC24, "Receive List magic mismatch ({} != {})", magic, FRIEND_LIST_MAGIC);
  return false;
}

bool WC24FriendList::DoesFriendExist(u64 friend_id) const
{
  return std::ranges::find(m_data.friend_codes, friend_id) != m_data.friend_codes.end();
}

std::vector<u64> WC24FriendList::GetUnconfirmedFriends() const
{
  std::vector<u64> pending;
  for (size_t i = 0; i < MAX_ENTRIES; ++i)
  {
    const FriendListEntry& entry = m_data.entries[i];
    const auto type = static_cast<FriendType>(Common::swap32(entry.friend_type));
    const auto status = static_cast<FriendStatus>(Common::swap32(entry.status));
    if (type == FriendType::Wii && status == FriendStatus::Unconfirmed)
      pending.push_back(Common::swap64(m_data.friend_codes[i]));
  }
  return pending;
}

u64 WC24FriendList::ConvertEmailToFriendCode(std::string_view email)
{
  // A 64-bit shift register into which up to seven characters are pushed a byte at a time.
  // Each character is ORed in (sign-extended where char is signed) rather than shifted in, and
  // bit 31 of the high word sticks. When there is an eighth character, the address length
  // (mod 32) goes into the lowest bits instead.
  u32 low_word = 0x80;
  u32 high_word = 0;

  for (size_t position = 0; position < email.size(); ++position)
  {
    if (position == 7)
    {
      low_word |= static_cast<u32>(email.size() & 0x1f);
      break;
    }

    const u32 mixed = low_word | static_cast<u32>(email[position]);
    const u32 sticky = high_word | (high_word >> 31);
    high_word = (sticky << 8) | (mixed >> 24);
    low_word = mixed << 8;
  }

  return (u64{high_word} << 32) | low_word;
}
}  // namespace IOS::HLE::NWC24::Mail
