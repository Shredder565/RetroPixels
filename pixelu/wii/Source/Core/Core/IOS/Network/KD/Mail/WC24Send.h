// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Core/IOS/Network/KD/Mail/MailCommon.h"
#include "Core/IOS/Network/KD/Mail/WC24FriendList.h"
#include "Core/IOS/Network/KD/NWC24Config.h"

namespace IOS::HLE
{
namespace FS
{
class FileSystem;
}
namespace NWC24::Mail
{
// VFF holding the bodies of outgoing messages.
constexpr const char SEND_BOX_PATH[] = "/" WII_WC24CONF_DIR "/mbox"
                                       "/wc24send.mbx";

// The outgoing mail list (wc24send.ctl). Values stay in file (big endian) order. If the list
// cannot be loaded it is deleted and the object stays disabled.
class WC24SendList final
{
public:
  explicit WC24SendList(std::shared_ptr<FS::FileSystem> fs);

  bool ReadSendList();
  bool CheckSendList() const;
  void WriteSendList() const;
  bool IsDisabled() const;

  std::string_view GetMailFlag() const;
  u32 GetNumberOfMail() const;
  // Indices of the (at most 16) entries to send next.
  std::vector<u32> GetMailToSend() const;
  u32 GetEntryId(u32 entry_index) const;
  u32 GetMailSize(u32 index) const;
  ErrorCode DeleteMessage(u32 index);
  // Name of the message body inside the send box VFF.
  std::string GetMailPath(u32 index) const;
  u32 GetNextEntryId() const;
  u32 GetNextEntryIndex() const;
  std::optional<u32> GetNextFreeEntryIndex() const;

  // Queues a friend registration message to every unconfirmed Wii friend.
  ErrorCode AddRegistrationMessages(const WC24FriendList& friend_list, u64 sender);

private:
  static constexpr u32 MAX_ENTRIES = 127;
  static constexpr u32 SEND_LIST_SIZE = 16384;

#pragma pack(push, 1)
  struct SendList final
  {
    MailListHeader header;
    std::array<MailListEntry, MAX_ENTRIES> entries;
  };
  static_assert(sizeof(SendList) == SEND_LIST_SIZE);
#pragma pack(pop)

  // Points next_entry_offset at the first free entry, or at the first entry if none is free.
  void PointAtFreeEntry();

  SendList m_data;
  std::shared_ptr<FS::FileSystem> m_fs;
  bool m_is_disabled = false;
};
}  // namespace NWC24::Mail
}  // namespace IOS::HLE
