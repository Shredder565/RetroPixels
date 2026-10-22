// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"

// Layout of "ancast" images, the signed and encrypted boot images used by
// the Wii U. A vWii PPC image is embedded in a DOL and must be decrypted
// before it can run. All multi-byte fields are big-endian.

// First word of every ancast image.
constexpr u32 ANCAST_MAGIC = 0xEFA282D9;

// Fixed load address of PPC ancast images, physical and cached virtual.
constexpr u32 ESPRESSO_ANCAST_LOCATION_PHYS = 0x01330000;
constexpr u32 ESPRESSO_ANCAST_LOCATION_VIRT = 0x81330000;

// Which processor and boot path the image is for.
enum AncastImageType
{
  ANCAST_IMAGE_TYPE_ESPRESSO_WIIU = 0x11,
  ANCAST_IMAGE_TYPE_ESPRESSO_WII = 0x13,
  ANCAST_IMAGE_TYPE_STARBUCK_NAND = 0x21,
  ANCAST_IMAGE_TYPE_STARBUCK_SD = 0x22,
};

// Which key set the body was encrypted with.
enum AncastConsoleType
{
  ANCAST_CONSOLE_TYPE_DEV = 0x01,
  ANCAST_CONSOLE_TYPE_RETAIL = 0x02,
};

// Leading block shared by both header variants.
struct AncastHeaderBlock
{
  u32 magic;
  u32 unknown;
  u32 header_block_size;
  u8 padding[0x14];
};
static_assert(sizeof(AncastHeaderBlock) == 0x20);

// Signature block of PPC images (signature type 1).
struct AncastSignatureBlockv1
{
  u32 signature_type;
  u8 signature[0x38];
  u8 padding[0x44];
};

// Signature block of ARM images (signature type 2).
struct AncastSignatureBlockv2
{
  u32 signature_type;
  u8 signature[0x100];
  u8 padding[0x7c];
};

// Describes the body that follows the header.
struct AncastInfoBlock
{
  u32 unknown;
  u32 image_type;
  u32 console_type;
  u32 body_size;
  Common::SHA1::Digest body_hash;
  u32 version;
  u8 padding[0x38];
};

// Full header of a PPC (Espresso) image.
struct EspressoAncastHeader
{
  AncastHeaderBlock header_block;
  AncastSignatureBlockv1 signature_block;
  AncastInfoBlock info_block;
};
static_assert(sizeof(EspressoAncastHeader) == 0x100);

// Full header of an ARM (Starbuck) image.
struct StarbuckAncastHeader
{
  AncastHeaderBlock header_block;
  AncastSignatureBlockv2 signature_block;
  AncastInfoBlock info_block;
};
static_assert(sizeof(StarbuckAncastHeader) == 0x200);
