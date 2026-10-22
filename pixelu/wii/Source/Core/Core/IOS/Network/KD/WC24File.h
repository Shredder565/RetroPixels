// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"

namespace IOS::HLE::NWC24
{
#pragma pack(push, 1)
// Header in front of a downloaded WiiConnect24 file (320 bytes). The payload after it is
// AES-encrypted when crypt_type says so, using `iv` and the title's key from WC24PubkMod.
struct WC24File final
{
  char magic[4];
  u32 version;
  u32 filler;
  u8 crypt_type;
  u8 padding[3];
  u8 reserved[32];
  u8 iv[16];
  u8 rsa_signature[256];
};
static_assert(sizeof(WC24File) == 320);

// Layout of a title's wc24pubk.mod file (544 bytes). Only the layout is defined here; the
// contents come from the title's own data.
struct WC24PubkMod final
{
  u8 rsa_public[256];
  u8 rsa_reserved[256];
  u8 aes_key[16];
  u8 aes_reserved[16];
};
static_assert(sizeof(WC24PubkMod) == 544);
#pragma pack(pop)
}  // namespace IOS::HLE::NWC24
