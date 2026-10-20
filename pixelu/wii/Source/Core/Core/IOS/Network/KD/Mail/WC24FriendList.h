// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Core/IOS/Network/KD/NWC24Config.h"

namespace IOS::HLE
{
namespace FS
{
class FileSystem;
}
namespace NWC24::Mail
{
constexpr const char FRIEND_LIST_PATH[] = "/" WII_WC24CONF_DIR "/nwc24fl.bin";

// The WiiConnect24 friend list (nwc24fl.bin): who mail may be exchanged with.
class WC24FriendList final
{
public:
  explicit WC24FriendList(std::shared_ptr<FS::FileSystem> fs);
  // The 64-bit ID that WiiConnect24 derives from an e-mail address.
  static u64 ConvertEmailToFriendCode(std::string_view email);

  void ReadFriendList();
  bool CheckFriendList() const;
  void WriteFriendList() const;

  // `friend_id` is compared with the codes as they are stored (big endian).
  bool DoesFriendExist(u64 friend_id) const;
  // Codes of the Wii friends whose registration is still pending.
  std::vector<u64> GetUnconfirmedFriends() const;

private:
  static constexpr u32 FRIEND_LIST_MAGIC = 0x5763466C;  // "WcFl"
  static constexpr u32 MAX_ENTRIES = 100;

#pragma pack(push, 1)
  struct FriendListHeader final
  {
    u32 magic;
    u32 version;
    u32 max_friend_entries;
    u32 number_of_friends;
    char padding[48];
  };
  static_assert(sizeof(FriendListHeader) == 64);
  static_assert(std::is_trivially_copyable_v<FriendListHeader>);

  enum class FriendType : u32
  {
    None,
    Wii,
    Email
  };

  enum class FriendStatus : u32
  {
    None,
    Unconfirmed,
    Confirmed,
    Declined
  };

  // Big endian, like the rest of the file.
  struct FriendListEntry final
  {
    u32 friend_type;
    u32 status;
    char nickname[24];
    u32 mii_id;
    u32 system_id;
    char reserved[24];
    char email_or_code[96];
    char padding[160];
  };
  static_assert(sizeof(FriendListEntry) == 320);
  static_assert(std::is_trivially_copyable_v<FriendListEntry>);

  struct FriendList final
  {
    FriendListHeader header;
    std::array<u64, MAX_ENTRIES> friend_codes;
    std::array<FriendListEntry, MAX_ENTRIES> entries;
  };
  static_assert(sizeof(FriendList) == 32864);
  static_assert(std::is_trivially_copyable_v<FriendList>);
#pragma pack(pop)

  FriendList m_data{};
  std::shared_ptr<FS::FileSystem> m_fs;
};
}  // namespace NWC24::Mail
}  // namespace IOS::HLE
