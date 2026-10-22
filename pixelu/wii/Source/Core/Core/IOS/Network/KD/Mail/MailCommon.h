// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace IOS::HLE::NWC24::Mail
{
// "WcTf": first word of the outgoing (wc24send.ctl) and incoming mail list files.
constexpr u32 MAIL_LIST_MAGIC = 0x57635466;

// Where entry `index` starts in a mail list file (after the 128-byte header), big endian.
inline u32 CalculateFileOffset(u32 index)
{
  return Common::swap32((index + 1) * 128);
}

#pragma pack(push, 1)
// All fields are stored big endian.
struct MailListHeader final
{
  u32 magic;
  u32 version;  // 4 on system menu 4.x
  u32 number_of_mail;
  u32 total_entries;
  u32 total_size_of_messages;
  u32 filesize;
  u32 next_entry_id;
  u32 next_entry_offset;
  u32 unk1;
  u32 vff_free_space;
  std::array<u8, 48> unk2;
  std::array<char, 40> mail_flag;
};
static_assert(sizeof(MailListHeader) == 128);

struct MultipartEntry final
{
  u32 offset;
  u32 size;
};

// One message of a mail list (128 bytes).
struct MailListEntry final
{
  u32 id;
  u32 flag;
  u32 msg_size;
  u32 app_id;
  u32 header_length;
  u32 tag;
  u32 wii_cmd;
  u32 crc32;  // not checked by anything
  u64 from_friend_code;
  u32 minutes_since_1900;
  u32 padding;
  u8 always_1;
  u8 number_of_multipart_entries;
  u16 app_group;
  u32 packed_from;
  u32 packed_to;
  u32 packed_subject;
  u32 packed_charset;
  u32 packed_transfer_encoding;
  u32 message_offset;
  u32 encoded_length;  // equals message_length unless the body is base64
  std::array<MultipartEntry, 2> multipart_entries;
  std::array<u32, 2> multipart_sizes;
  std::array<u32, 2> multipart_content_types;
  u32 message_length;
  u32 dwc_id;
  u32 always_0x80000000;
  u32 padding3;
};
static_assert(sizeof(MailListEntry) == 128);
#pragma pack(pop)
}  // namespace IOS::HLE::NWC24::Mail
