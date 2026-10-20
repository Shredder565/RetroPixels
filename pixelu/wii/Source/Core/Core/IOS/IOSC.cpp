// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/IOSC.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <mbedtls/md.h>
#include <mbedtls/rsa.h>

#include "Common/Assert.h"
#include "Common/ChunkFile.h"
#include "Common/Crypto/AES.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Crypto/ec.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/ScopeGuard.h"
#include "Common/Swap.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/ES/Formats.h"

namespace
{
// Layout of keys.bin, the 1 KiB OTP/SEEPROM dump written by BootMii (see WiiBrew, "keys.bin").
// Offsets are given for each field; multi-byte values are big endian.
#pragma pack(push, 1)
struct KeysBin
{
  std::array<char, 0x100> creator;     // 0x000
  std::array<u8, 0x14> boot1_hash;     // 0x100
  std::array<u8, 0x10> common_key;     // 0x114
  u32 ng_id;                           // 0x124
  std::array<u8, 0x1e> ng_priv;        // 0x128
  std::array<u8, 0x02> unused_146;     // 0x146 (overlaps the NAND HMAC key below)
  std::array<u8, 0x10> unused_148;     // 0x148
  std::array<u8, 0x10> nand_key;       // 0x158
  std::array<u8, 0x10> backup_key;     // 0x168
  u32 unknown_178;                     // 0x178
  u32 unknown_17c;                     // 0x17c
  std::array<u8, 0x80> unused_180;     // 0x180
  u32 ms_id;                           // 0x200
  u32 ca_id;                           // 0x204
  u32 ng_key_id;                       // 0x208
  Common::ec::Signature ng_sig;        // 0x20c
  struct Counter
  {
    u8 boot2version;
    u8 unknown1;
    u8 unknown2;
    u8 pad;
    u32 update_tag;
    u16 checksum;
  };
  std::array<Counter, 2> counters;     // 0x248
  std::array<u8, 0x18> unused_25c;     // 0x25c
  std::array<u8, 0x10> korean_key;     // 0x274
  std::array<u8, 0x74> unused_284;     // 0x284
  std::array<u16, 2> prng_seed;        // 0x2f8
  std::array<u8, 0x04> unused_2fc;     // 0x2fc
  std::array<u8, 0x100> unused_300;    // 0x300
};
static_assert(sizeof(KeysBin) == 0x400, "keys.bin is 1 KiB");
#pragma pack(pop)

// The NAND HMAC key shares bytes with the end of the NG private key region.
constexpr size_t KEYS_BIN_NAND_HMAC_OFFSET = 0x144;
constexpr size_t KEYS_BIN_NAND_HMAC_SIZE = 0x14;
}  // namespace

namespace IOS::HLE
{
// =============================================================================================
// Key material. These tables are carried over unchanged.

constexpr std::array<u8, 512> ROOT_PUBLIC_KEY = {
    {0xF8, 0x24, 0x6C, 0x58, 0xBA, 0xE7, 0x50, 0x03, 0x01, 0xFB, 0xB7, 0xC2, 0xEB, 0xE0, 0x01,
     0x05, 0x71, 0xDA, 0x92, 0x23, 0x78, 0xF0, 0x51, 0x4E, 0xC0, 0x03, 0x1D, 0xD0, 0xD2, 0x1E,
     0xD3, 0xD0, 0x7E, 0xFC, 0x85, 0x20, 0x69, 0xB5, 0xDE, 0x9B, 0xB9, 0x51, 0xA8, 0xBC, 0x90,
     0xA2, 0x44, 0x92, 0x6D, 0x37, 0x92, 0x95, 0xAE, 0x94, 0x36, 0xAA, 0xA6, 0xA3, 0x02, 0x51,
     0x0C, 0x7B, 0x1D, 0xED, 0xD5, 0xFB, 0x20, 0x86, 0x9D, 0x7F, 0x30, 0x16, 0xF6, 0xBE, 0x65,
     0xD3, 0x83, 0xA1, 0x6D, 0xB3, 0x32, 0x1B, 0x95, 0x35, 0x18, 0x90, 0xB1, 0x70, 0x02, 0x93,
     0x7E, 0xE1, 0x93, 0xF5, 0x7E, 0x99, 0xA2, 0x47, 0x4E, 0x9D, 0x38, 0x24, 0xC7, 0xAE, 0xE3,
     0x85, 0x41, 0xF5, 0x67, 0xE7, 0x51, 0x8C, 0x7A, 0x0E, 0x38, 0xE7, 0xEB, 0xAF, 0x41, 0x19,
     0x1B, 0xCF, 0xF1, 0x7B, 0x42, 0xA6, 0xB4, 0xED, 0xE6, 0xCE, 0x8D, 0xE7, 0x31, 0x8F, 0x7F,
     0x52, 0x04, 0xB3, 0x99, 0x0E, 0x22, 0x67, 0x45, 0xAF, 0xD4, 0x85, 0xB2, 0x44, 0x93, 0x00,
     0x8B, 0x08, 0xC7, 0xF6, 0xB7, 0xE5, 0x6B, 0x02, 0xB3, 0xE8, 0xFE, 0x0C, 0x9D, 0x85, 0x9C,
     0xB8, 0xB6, 0x82, 0x23, 0xB8, 0xAB, 0x27, 0xEE, 0x5F, 0x65, 0x38, 0x07, 0x8B, 0x2D, 0xB9,
     0x1E, 0x2A, 0x15, 0x3E, 0x85, 0x81, 0x80, 0x72, 0xA2, 0x3B, 0x6D, 0xD9, 0x32, 0x81, 0x05,
     0x4F, 0x6F, 0xB0, 0xF6, 0xF5, 0xAD, 0x28, 0x3E, 0xCA, 0x0B, 0x7A, 0xF3, 0x54, 0x55, 0xE0,
     0x3D, 0xA7, 0xB6, 0x83, 0x26, 0xF3, 0xEC, 0x83, 0x4A, 0xF3, 0x14, 0x04, 0x8A, 0xC6, 0xDF,
     0x20, 0xD2, 0x85, 0x08, 0x67, 0x3C, 0xAB, 0x62, 0xA2, 0xC7, 0xBC, 0x13, 0x1A, 0x53, 0x3E,
     0x0B, 0x66, 0x80, 0x6B, 0x1C, 0x30, 0x66, 0x4B, 0x37, 0x23, 0x31, 0xBD, 0xC4, 0xB0, 0xCA,
     0xD8, 0xD1, 0x1E, 0xE7, 0xBB, 0xD9, 0x28, 0x55, 0x48, 0xAA, 0xEC, 0x1F, 0x66, 0xE8, 0x21,
     0xB3, 0xC8, 0xA0, 0x47, 0x69, 0x00, 0xC5, 0xE6, 0x88, 0xE8, 0x0C, 0xCE, 0x3C, 0x61, 0xD6,
     0x9C, 0xBB, 0xA1, 0x37, 0xC6, 0x60, 0x4F, 0x7A, 0x72, 0xDD, 0x8C, 0x7B, 0x3E, 0x3D, 0x51,
     0x29, 0x0D, 0xAA, 0x6A, 0x59, 0x7B, 0x08, 0x1F, 0x9D, 0x36, 0x33, 0xA3, 0x46, 0x7A, 0x35,
     0x61, 0x09, 0xAC, 0xA7, 0xDD, 0x7D, 0x2E, 0x2F, 0xB2, 0xC1, 0xAE, 0xB8, 0xE2, 0x0F, 0x48,
     0x92, 0xD8, 0xB9, 0xF8, 0xB4, 0x6F, 0x4E, 0x3C, 0x11, 0xF4, 0xF4, 0x7D, 0x8B, 0x75, 0x7D,
     0xFE, 0xFE, 0xA3, 0x89, 0x9C, 0x33, 0x59, 0x5C, 0x5E, 0xFD, 0xEB, 0xCB, 0xAB, 0xE8, 0x41,
     0x3E, 0x3A, 0x9A, 0x80, 0x3C, 0x69, 0x35, 0x6E, 0xB2, 0xB2, 0xAD, 0x5C, 0xC4, 0xC8, 0x58,
     0x45, 0x5E, 0xF5, 0xF7, 0xB3, 0x06, 0x44, 0xB4, 0x7C, 0x64, 0x06, 0x8C, 0xDF, 0x80, 0x9F,
     0x76, 0x02, 0x5A, 0x2D, 0xB4, 0x46, 0xE0, 0x3D, 0x7C, 0xF6, 0x2F, 0x34, 0xE7, 0x02, 0x45,
     0x7B, 0x02, 0xA4, 0xCF, 0x5D, 0x9D, 0xD5, 0x3C, 0xA5, 0x3A, 0x7C, 0xA6, 0x29, 0x78, 0x8C,
     0x67, 0xCA, 0x08, 0xBF, 0xEC, 0xCA, 0x43, 0xA9, 0x57, 0xAD, 0x16, 0xC9, 0x4E, 0x1C, 0xD8,
     0x75, 0xCA, 0x10, 0x7D, 0xCE, 0x7E, 0x01, 0x18, 0xF0, 0xDF, 0x6B, 0xFE, 0xE5, 0x1D, 0xDB,
     0xD9, 0x91, 0xC2, 0x6E, 0x60, 0xCD, 0x48, 0x58, 0xAA, 0x59, 0x2C, 0x82, 0x00, 0x75, 0xF2,
     0x9F, 0x52, 0x6C, 0x91, 0x7C, 0x6F, 0xE5, 0x40, 0x3E, 0xA7, 0xD4, 0xA5, 0x0C, 0xEC, 0x3B,
     0x73, 0x84, 0xDE, 0x88, 0x6E, 0x82, 0xD2, 0xEB, 0x4D, 0x4E, 0x42, 0xB5, 0xF2, 0xB1, 0x49,
     0xA8, 0x1E, 0xA7, 0xCE, 0x71, 0x44, 0xDC, 0x29, 0x94, 0xCF, 0xC4, 0x4E, 0x1F, 0x91, 0xCB,
     0xD4, 0x95}};

constexpr std::array<u8, 512> ROOT_PUBLIC_KEY_DEV = {
    {0xD0, 0x1F, 0xE1, 0x00, 0xD4, 0x35, 0x56, 0xB2, 0x4B, 0x56, 0xDA, 0xE9, 0x71, 0xB5, 0xA5,
     0xD3, 0x84, 0xB9, 0x30, 0x03, 0xBE, 0x1B, 0xBF, 0x28, 0xA2, 0x30, 0x5B, 0x06, 0x06, 0x45,
     0x46, 0x7D, 0x5B, 0x02, 0x51, 0xD2, 0x56, 0x1A, 0x27, 0x4F, 0x9E, 0x9F, 0x9C, 0xEC, 0x64,
     0x61, 0x50, 0xAB, 0x3D, 0x2A, 0xE3, 0x36, 0x68, 0x66, 0xAC, 0xA4, 0xBA, 0xE8, 0x1A, 0xE3,
     0xD7, 0x9A, 0xA6, 0xB0, 0x4A, 0x8B, 0xCB, 0xA7, 0xE6, 0xFB, 0x64, 0x89, 0x45, 0xEB, 0xDF,
     0xDB, 0x85, 0xBA, 0x09, 0x1F, 0xD7, 0xD1, 0x14, 0xB5, 0xA3, 0xA7, 0x80, 0xE3, 0xA2, 0x2E,
     0x6E, 0xCD, 0x87, 0xB5, 0xA4, 0xC6, 0xF9, 0x10, 0xE4, 0x03, 0x22, 0x08, 0x81, 0x4B, 0x0C,
     0xEE, 0xA1, 0xA1, 0x7D, 0xF7, 0x39, 0x69, 0x5F, 0x61, 0x7E, 0xF6, 0x35, 0x28, 0xDB, 0x94,
     0x96, 0x37, 0xA0, 0x56, 0x03, 0x7F, 0x7B, 0x32, 0x41, 0x38, 0x95, 0xC0, 0xA8, 0xF1, 0x98,
     0x2E, 0x15, 0x65, 0xE3, 0x8E, 0xED, 0xC2, 0x2E, 0x59, 0x0E, 0xE2, 0x67, 0x7B, 0x86, 0x09,
     0xF4, 0x8C, 0x2E, 0x30, 0x3F, 0xBC, 0x40, 0x5C, 0xAC, 0x18, 0x04, 0x2F, 0x82, 0x20, 0x84,
     0xE4, 0x93, 0x68, 0x03, 0xDA, 0x7F, 0x41, 0x34, 0x92, 0x48, 0x56, 0x2B, 0x8E, 0xE1, 0x2F,
     0x78, 0xF8, 0x03, 0x24, 0x63, 0x30, 0xBC, 0x7B, 0xE7, 0xEE, 0x72, 0x4A, 0xF4, 0x58, 0xA4,
     0x72, 0xE7, 0xAB, 0x46, 0xA1, 0xA7, 0xC1, 0x0C, 0x2F, 0x18, 0xFA, 0x07, 0xC3, 0xDD, 0xD8,
     0x98, 0x06, 0xA1, 0x1C, 0x9C, 0xC1, 0x30, 0xB2, 0x47, 0xA3, 0x3C, 0x8D, 0x47, 0xDE, 0x67,
     0xF2, 0x9E, 0x55, 0x77, 0xB1, 0x1C, 0x43, 0x49, 0x3D, 0x5B, 0xBA, 0x76, 0x34, 0xA7, 0xE4,
     0xE7, 0x15, 0x31, 0xB7, 0xDF, 0x59, 0x81, 0xFE, 0x24, 0xA1, 0x14, 0x55, 0x4C, 0xBD, 0x8F,
     0x00, 0x5C, 0xE1, 0xDB, 0x35, 0x08, 0x5C, 0xCF, 0xC7, 0x78, 0x06, 0xB6, 0xDE, 0x25, 0x40,
     0x68, 0xA2, 0x6C, 0xB5, 0x49, 0x2D, 0x45, 0x80, 0x43, 0x8F, 0xE1, 0xE5, 0xA9, 0xED, 0x75,
     0xC5, 0xED, 0x45, 0x1D, 0xCE, 0x78, 0x94, 0x39, 0xCC, 0xC3, 0xBA, 0x28, 0xA2, 0x31, 0x2A,
     0x1B, 0x87, 0x19, 0xEF, 0x0F, 0x73, 0xB7, 0x13, 0x95, 0x0C, 0x02, 0x59, 0x1A, 0x74, 0x62,
     0xA6, 0x07, 0xF3, 0x7C, 0x0A, 0xA7, 0xA1, 0x8F, 0xA9, 0x43, 0xA3, 0x6D, 0x75, 0x2A, 0x5F,
     0x41, 0x92, 0xF0, 0x13, 0x61, 0x00, 0xAA, 0x9C, 0xB4, 0x1B, 0xBE, 0x14, 0xBE, 0xB1, 0xF9,
     0xFC, 0x69, 0x2F, 0xDF, 0xA0, 0x94, 0x46, 0xDE, 0x5A, 0x9D, 0xDE, 0x2C, 0xA5, 0xF6, 0x8C,
     0x1C, 0x0C, 0x21, 0x42, 0x92, 0x87, 0xCB, 0x2D, 0xAA, 0xA3, 0xD2, 0x63, 0x75, 0x2F, 0x73,
     0xE0, 0x9F, 0xAF, 0x44, 0x79, 0xD2, 0x81, 0x74, 0x29, 0xF6, 0x98, 0x00, 0xAF, 0xDE, 0x6B,
     0x59, 0x2D, 0xC1, 0x98, 0x82, 0xBD, 0xF5, 0x81, 0xCC, 0xAB, 0xF2, 0xCB, 0x91, 0x02, 0x9E,
     0xF3, 0x5C, 0x4C, 0xFD, 0xBB, 0xFF, 0x49, 0xC1, 0xFA, 0x1B, 0x2F, 0xE3, 0x1D, 0xE7, 0xA5,
     0x60, 0xEC, 0xB4, 0x7E, 0xBC, 0xFE, 0x32, 0x42, 0x5B, 0x95, 0x6F, 0x81, 0xB6, 0x99, 0x17,
     0x48, 0x7E, 0x3B, 0x78, 0x91, 0x51, 0xDB, 0x2E, 0x78, 0xB1, 0xFD, 0x2E, 0xBE, 0x7E, 0x62,
     0x6B, 0x3E, 0xA1, 0x65, 0xB4, 0xFB, 0x00, 0xCC, 0xB7, 0x51, 0xAF, 0x50, 0x73, 0x29, 0xC4,
     0xA3, 0x93, 0x9E, 0xA6, 0xDD, 0x9C, 0x50, 0xA0, 0xE7, 0x38, 0x6B, 0x01, 0x45, 0x79, 0x6B,
     0x41, 0xAF, 0x61, 0xF7, 0x85, 0x55, 0x94, 0x4F, 0x3B, 0xC2, 0x2D, 0xC3, 0xBD, 0x0D, 0x00,
     0xF8, 0x79, 0x8A, 0x42, 0xB1, 0xAA, 0xA0, 0x83, 0x20, 0x65, 0x9A, 0xC7, 0x39, 0x5A, 0xB4,
     0xF3, 0x29}};

constexpr u32 DEFAULT_DEVICE_ID = 0x0403AC68;
constexpr u32 DEFAULT_KEY_ID = 0x6AAB8C59;

constexpr std::array<u8, 30> DEFAULT_PRIVATE_KEY = {{
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
}};

// clang-format off
constexpr Common::ec::Signature DEFAULT_SIGNATURE = {{
    // R
    0x00, 0xD8, 0x81, 0x63, 0xB2, 0x00, 0x6B, 0x0B, 0x54, 0x82, 0x88, 0x63, 0x81, 0x1C, 0x00, 0x71,
    0x12, 0xED, 0xB7, 0xFD, 0x21, 0xAB, 0x0E, 0x50, 0x0E, 0x1F, 0xBF, 0x78, 0xAD, 0x37,
    // S
    0x00, 0x71, 0x8D, 0x82, 0x41, 0xEE, 0x45, 0x11, 0xC7, 0x3B, 0xAC, 0x08, 0xB6, 0x83, 0xDC, 0x05,
    0xB8, 0xA8, 0x90, 0x1F, 0xA8, 0x2A, 0x0E, 0x4E, 0x76, 0xEF, 0x44, 0x72, 0x99, 0xF8,
}};
// clang-format on

constexpr std::array<u8, 16> COMMON_KEY_RETAIL = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
constexpr std::array<u8, 16> COMMON_KEY_DEV = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
constexpr std::array<u8, 16> SD_KEY = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};
constexpr std::array<u8, 16> KOREAN_COMMON_KEY = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}};

// =============================================================================================

namespace
{
constexpr size_t AES128_KEY_SIZE = 0x10;
constexpr u32 RSA_EXPONENT_65537_BE = 0x01000100;  // 0x00010001 as stored (big endian)

// Bytes of key material an object of the given kind holds (0 = no fixed size / no key).
size_t KeySize(IOSC::ObjectType type, IOSC::ObjectSubType subtype)
{
  using ST = IOSC::ObjectSubType;
  if (type == IOSC::TYPE_SECRET_KEY)
  {
    switch (subtype)
    {
    case ST::AES128:
      return 16;
    case ST::MAC:
      return 20;
    case ST::ECC233:
      return 30;
    default:
      return 0;
    }
  }
  if (type == IOSC::TYPE_PUBLIC_KEY)
  {
    switch (subtype)
    {
    case ST::RSA2048:
      return 256;
    case ST::RSA4096:
      return 512;
    case ST::ECC233:
      return 60;
    default:
      return 0;
    }
  }
  return 0;
}

u32 OwnerBit(u32 pid)
{
  return 1u << pid;
}

// Builds an unsigned ECC certificate for the key derived from `private_key`.
CertECC MakeEccCertificate(const std::string& issuer, const std::string& name,
                           const u8* private_key, u32 key_id)
{
  CertECC cert{};
  cert.signature.type = static_cast<SignatureType>(Common::swap32(u32(SignatureType::ECC)));
  const size_t issuer_len = std::min(issuer.size(), sizeof(cert.signature.issuer) - 1);
  std::memcpy(cert.signature.issuer, issuer.data(), issuer_len);
  cert.header.public_key_type = static_cast<PublicKeyType>(Common::swap32(u32(PublicKeyType::ECC)));
  const size_t name_len = std::min(name.size(), sizeof(cert.header.name) - 1);
  std::memcpy(cert.header.name, name.data(), name_len);
  cert.header.id = Common::swap32(key_id);
  cert.public_key = Common::ec::PrivToPub(private_key);
  return cert;
}
}  // namespace

IOSC::KeyEntry::KeyEntry() = default;

IOSC::KeyEntry::KeyEntry(ObjectType type_, ObjectSubType subtype_, std::vector<u8>&& data_,
                         u32 misc_data_, u32 owner_mask_)
    : in_use(true), type(type_), subtype(subtype_), data(std::move(data_)), misc_data(misc_data_),
      owner_mask(owner_mask_)
{
}

IOSC::KeyEntry::KeyEntry(ObjectType type_, ObjectSubType subtype_, std::vector<u8>&& data_,
                         u32 owner_mask_)
    : KeyEntry(type_, subtype_, std::move(data_), 0, owner_mask_)
{
}

void IOSC::KeyEntry::DoState(PointerWrap& p)
{
  p.Do(in_use);
  p.Do(type);
  p.Do(subtype);
  p.Do(data);
  p.Do(misc_data);
  p.Do(owner_mask);
}

IOSC::IOSC(ConsoleType console_type) : m_console_type(console_type)
{
  LoadDefaultEntries();
  // Real console keys, when the user has provided a dump, replace some of the defaults.
  LoadEntries();
}

IOSC::~IOSC() = default;

// --------------------------------------------------------------------------------------------
// Table management

IOSC::KeyEntries::iterator IOSC::FindFreeEntry()
{
  auto it = m_key_entries.begin();
  while (it != m_key_entries.end() && it->in_use)
    ++it;
  return it;
}

IOSC::KeyEntry* IOSC::FindEntry(Handle handle)
{
  if (handle >= m_key_entries.size())
    return nullptr;
  return &m_key_entries[handle];
}

const IOSC::KeyEntry* IOSC::FindEntry(Handle handle, SearchMode mode) const
{
  if (handle == HANDLE_ROOT_KEY && mode == SearchMode::IncludeRootKey)
    return &m_root_key_entry;
  if (handle >= m_key_entries.size())
    return nullptr;
  return &m_key_entries[handle];
}

IOSC::Handle IOSC::GetHandleFromIterator(IOSC::KeyEntries::iterator iterator) const
{
  ASSERT(iterator != m_key_entries.end());
  return static_cast<Handle>(std::distance(m_key_entries.cbegin(), KeyEntries::const_iterator{iterator}));
}

bool IOSC::IsDefaultHandle(Handle handle) const
{
  return handle == HANDLE_ROOT_KEY || handle <= HANDLE_NEW_COMMON_KEY;
}

bool IOSC::HasOwnership(Handle handle, u32 pid) const
{
  // Everyone may use the root key.
  if (handle == HANDLE_ROOT_KEY)
    return true;
  u32 owners;
  if (GetOwnership(handle, &owners) != IPC_SUCCESS)
    return false;
  return (owners & OwnerBit(pid)) != 0;
}

ReturnCode IOSC::GetOwnership(Handle handle, u32* owner) const
{
  const KeyEntry* entry = FindEntry(handle);
  if (entry == nullptr || !entry->in_use)
    return IOSC_EINVAL;
  *owner = entry->owner_mask;
  return IPC_SUCCESS;
}

ReturnCode IOSC::SetOwnership(Handle handle, u32 new_owner, u32 pid)
{
  if (!HasOwnership(handle, pid))
    return IOSC_EACCES;

  KeyEntry* entry = FindEntry(handle);
  if (entry == nullptr)
    return IOSC_EINVAL;

  // Only an object owned by the caller alone may be handed on. The caller stays an owner;
  // bits 0-2 of the new mask cannot be granted this way.
  const u32 caller = OwnerBit(pid);
  if ((entry->owner_mask & ~caller) != 0)
    return IOSC_EACCES;
  entry->owner_mask = (new_owner & ~7u) | caller;
  return IPC_SUCCESS;
}

ReturnCode IOSC::CreateObject(Handle* handle, ObjectType type, ObjectSubType subtype, u32 pid)
{
  const auto slot = FindFreeEntry();
  if (slot == m_key_entries.end())
    return IOSC_FAIL_ALLOC;

  // The slot's previous key bytes and misc word are left as they were.
  slot->in_use = true;
  slot->type = type;
  slot->subtype = subtype;
  slot->owner_mask = OwnerBit(pid);
  *handle = GetHandleFromIterator(slot);
  return IPC_SUCCESS;
}

ReturnCode IOSC::DeleteObject(Handle handle, u32 pid)
{
  if (IsDefaultHandle(handle) || !HasOwnership(handle, pid))
    return IOSC_EACCES;

  KeyEntry* entry = FindEntry(handle);
  if (entry == nullptr)
    return IOSC_EINVAL;
  entry->in_use = false;
  entry->data.clear();
  return IPC_SUCCESS;
}

// --------------------------------------------------------------------------------------------
// Key import

ReturnCode IOSC::ImportSecretKey(Handle dest_handle, Handle decrypt_handle, u8* iv,
                                 const u8* encrypted_key, u32 pid)
{
  std::array<u8, AES128_KEY_SIZE> plain;
  const ReturnCode result =
      Decrypt(decrypt_handle, iv, encrypted_key, plain.size(), plain.data(), pid);
  if (result != IPC_SUCCESS)
    return result;
  return ImportSecretKey(dest_handle, plain.data(), pid);
}

ReturnCode IOSC::ImportSecretKey(Handle dest_handle, const u8* decrypted_key, u32 pid)
{
  if (!HasOwnership(dest_handle, pid) || IsDefaultHandle(dest_handle))
    return IOSC_EACCES;

  KeyEntry* dest = FindEntry(dest_handle);
  if (dest == nullptr)
    return IOSC_EINVAL;

  // Only AES keys can be imported so far.
  if (dest->type != TYPE_SECRET_KEY || dest->subtype != ObjectSubType::AES128)
    return IOSC_INVALID_OBJTYPE;

  dest->data.assign(decrypted_key, decrypted_key + AES128_KEY_SIZE);
  return IPC_SUCCESS;
}

ReturnCode IOSC::ImportPublicKey(Handle dest_handle, const u8* public_key,
                                 const u8* public_key_exponent, u32 pid)
{
  if (!HasOwnership(dest_handle, pid) || IsDefaultHandle(dest_handle))
    return IOSC_EACCES;

  KeyEntry* dest = FindEntry(dest_handle);
  if (dest == nullptr)
    return IOSC_EINVAL;
  if (dest->type != TYPE_PUBLIC_KEY)
    return IOSC_INVALID_OBJTYPE;

  const size_t length = KeySize(dest->type, dest->subtype);
  if (length == 0)
    return IOSC_INVALID_OBJTYPE;
  dest->data.assign(public_key, public_key + length);

  // RSA keys keep their (big endian) exponent in the misc word.
  const bool is_rsa =
      dest->subtype == ObjectSubType::RSA2048 || dest->subtype == ObjectSubType::RSA4096;
  if (is_rsa)
  {
    ASSERT(public_key_exponent);
    std::memcpy(&dest->misc_data, public_key_exponent, sizeof(dest->misc_data));
  }
  return IPC_SUCCESS;
}

ReturnCode IOSC::ComputeSharedKey(Handle dest_handle, Handle private_handle, Handle public_handle,
                                  u32 pid)
{
  const bool owns_all = HasOwnership(dest_handle, pid) && HasOwnership(private_handle, pid) &&
                        HasOwnership(public_handle, pid);
  if (!owns_all || IsDefaultHandle(dest_handle))
    return IOSC_EACCES;

  KeyEntry* dest = FindEntry(dest_handle);
  const KeyEntry* mine = FindEntry(private_handle);
  const KeyEntry* theirs = FindEntry(public_handle);
  if (dest == nullptr || mine == nullptr || theirs == nullptr)
    return IOSC_EINVAL;

  const bool kinds_ok = dest->type == TYPE_SECRET_KEY && dest->subtype == ObjectSubType::AES128 &&
                        mine->type == TYPE_SECRET_KEY && mine->subtype == ObjectSubType::ECC233 &&
                        theirs->type == TYPE_PUBLIC_KEY && theirs->subtype == ObjectSubType::ECC233;
  if (!kinds_ok)
    return IOSC_INVALID_OBJTYPE;

  // The AES key is the first 16 bytes of SHA-1 over the x coordinate of the shared point.
  const std::array<u8, 0x3c> point =
      Common::ec::ComputeSharedSecret(mine->data.data(), theirs->data.data());
  const auto digest = Common::SHA1::CalculateDigest(point.data(), point.size() / 2);
  dest->data.assign(digest.begin(), digest.begin() + AES128_KEY_SIZE);
  return IPC_SUCCESS;
}

// --------------------------------------------------------------------------------------------
// Using keys

ReturnCode IOSC::DecryptEncrypt(Common::AES::Mode mode, Handle key_handle, u8* iv, const u8* input,
                                size_t size, u8* output, u32 pid) const
{
  if (!HasOwnership(key_handle, pid))
    return IOSC_EACCES;

  const KeyEntry* entry = FindEntry(key_handle);
  if (entry == nullptr)
    return IOSC_EINVAL;
  if (entry->type != TYPE_SECRET_KEY || entry->subtype != ObjectSubType::AES128)
    return IOSC_INVALID_OBJTYPE;
  // An AES object that never had a key imported.
  if (entry->data.size() != AES128_KEY_SIZE)
    return IOSC_FAIL_INTERNAL;

  const u8* key = entry->data.data();
  const auto cipher = mode == Common::AES::Mode::Encrypt ? Common::AES::CreateContextEncrypt(key) :
                                                           Common::AES::CreateContextDecrypt(key);
  cipher->Crypt(iv, iv, input, output, size);
  return IPC_SUCCESS;
}

ReturnCode IOSC::Encrypt(Handle key_handle, u8* iv, const u8* input, size_t size, u8* output,
                         u32 pid) const
{
  return DecryptEncrypt(Common::AES::Mode::Encrypt, key_handle, iv, input, size, output, pid);
}

ReturnCode IOSC::Decrypt(Handle key_handle, u8* iv, const u8* input, size_t size, u8* output,
                         u32 pid) const
{
  return DecryptEncrypt(Common::AES::Mode::Decrypt, key_handle, iv, input, size, output, pid);
}

ReturnCode IOSC::VerifyPublicKeySign(const std::array<u8, 20>& sha1, Handle signer_handle,
                                     const std::vector<u8>& signature, u32 pid) const
{
  if (!HasOwnership(signer_handle, pid))
    return IOSC_EACCES;

  const KeyEntry* signer = FindEntry(signer_handle, SearchMode::IncludeRootKey);
  if (signer == nullptr)
    return IOSC_EINVAL;
  // Key pairs are not supported, only public keys.
  if (signer->type != TYPE_PUBLIC_KEY)
    return IOSC_INVALID_OBJTYPE;

  if (signer->subtype == ObjectSubType::ECC233)
  {
    ASSERT(signer->data.size() == sizeof(CertECC::public_key));
    const bool valid =
        Common::ec::VerifySignature(signer->data.data(), signature.data(), sha1.data());
    return valid ? IPC_SUCCESS : IOSC_FAIL_CHECKVALUE;
  }

  if (signer->subtype != ObjectSubType::RSA2048 && signer->subtype != ObjectSubType::RSA4096)
    return IOSC_INVALID_OBJTYPE;

  const size_t modulus_size = signer->subtype == ObjectSubType::RSA2048 ? 0x100 : 0x200;
  ASSERT(signer->data.size() == modulus_size);
  ASSERT(signature.size() == modulus_size);

  mbedtls_rsa_context rsa;
  mbedtls_rsa_init(&rsa, MBEDTLS_RSA_PKCS_V15, 0);
  Common::ScopeGuard free_rsa{[&rsa] { mbedtls_rsa_free(&rsa); }};
  mbedtls_mpi_read_binary(&rsa.N, signer->data.data(), signer->data.size());
  mbedtls_mpi_read_binary(&rsa.E, reinterpret_cast<const u8*>(&signer->misc_data),
                          sizeof(signer->misc_data));
  rsa.len = signer->data.size();

  int status = mbedtls_rsa_pkcs1_verify(&rsa, nullptr, nullptr, MBEDTLS_RSA_PUBLIC,
                                        MBEDTLS_MD_SHA1, 0, sha1.data(), signature.data());

  // Development signatures are not always PKCS#1 padded. On those consoles, accept a signature
  // whose raw RSA result simply ends in the digest.
  if (status != 0 && m_console_type == ConsoleType::RVT)
  {
    std::vector<u8> raw(signature.size());
    const bool decoded = mbedtls_rsa_public(&rsa, signature.data(), raw.data()) == 0;
    if (decoded && std::equal(sha1.begin(), sha1.end(), raw.end() - static_cast<std::ptrdiff_t>(sha1.size())))
      status = 0;
  }

  if (status != 0)
  {
    WARN_LOG_FMT(IOS, "RSA signature check failed (mbedtls error {})", status);
    return IOSC_FAIL_CHECKVALUE;
  }
  return IPC_SUCCESS;
}

ReturnCode IOSC::ImportCertificate(const ES::CertReader& cert, Handle signer_handle,
                                   Handle dest_handle, u32 pid)
{
  if (!HasOwnership(signer_handle, pid) || !HasOwnership(dest_handle, pid))
    return IOSC_EACCES;

  const KeyEntry* signer = FindEntry(signer_handle, SearchMode::IncludeRootKey);
  const KeyEntry* dest = FindEntry(dest_handle, SearchMode::IncludeRootKey);
  if (signer == nullptr || dest == nullptr)
    return IOSC_EINVAL;
  if (signer->type != TYPE_PUBLIC_KEY || dest->type != TYPE_PUBLIC_KEY)
    return IOSC_INVALID_OBJTYPE;

  if (!cert.IsValid())
    return IOSC_INVALID_FORMAT;

  if (VerifyPublicKeySign(cert.GetSha1(), signer_handle, cert.GetSignatureData(), pid) !=
      IPC_SUCCESS)
  {
    return IOSC_FAIL_CHECKVALUE;
  }

  // For RSA keys the exponent is the last word of the key data.
  const std::vector<u8> key = cert.GetPublicKey();
  const u8* exponent = nullptr;
  if (cert.GetSignatureType() != SignatureType::ECC)
    exponent = key.data() + key.size() - 4;
  return ImportPublicKey(dest_handle, key.data(), exponent, pid);
}

// --------------------------------------------------------------------------------------------
// Console identity

bool IOSC::IsUsingDefaultId() const
{
  return GetDeviceId() == DEFAULT_DEVICE_ID;
}

u32 IOSC::GetDeviceId() const
{
  return m_key_entries[HANDLE_CONSOLE_ID].misc_data;
}

CertECC IOSC::GetDeviceCertificate() const
{
  const std::string issuer = fmt::format("Root-CA{:08x}-MS{:08x}", m_ca_id, m_ms_id);
  const std::string name = fmt::format("NG{:08x}", GetDeviceId());
  CertECC cert = MakeEccCertificate(issuer, name, m_key_entries[HANDLE_CONSOLE_KEY].data.data(),
                                    m_console_key_id);
  cert.signature.sig = m_console_signature;
  return cert;
}

// Signs data the way ES_Sign does: a fresh "AP" key is certified by the console key, and the
// data is signed with the AP key. The AP private key is fixed (the value 1) rather than random.
void IOSC::Sign(u8* sig_out, u8* ap_cert_out, u64 title_id, const u8* data, u32 data_size) const
{
  std::array<u8, 30> ap_private_key{};
  ap_private_key.back() = 1;

  const std::string issuer =
      fmt::format("Root-CA{:08x}-MS{:08x}-NG{:08x}", m_ca_id, m_ms_id, GetDeviceId());
  const std::string name = fmt::format("AP{:016x}", title_id);
  CertECC ap_cert = MakeEccCertificate(issuer, name, ap_private_key.data(), 0);

  // The certificate's signature covers everything from the issuer field on.
  const size_t signed_from = offsetof(CertECC, signature.issuer);
  const auto* cert_bytes = reinterpret_cast<const u8*>(&ap_cert);
  const auto cert_digest =
      Common::SHA1::CalculateDigest(cert_bytes + signed_from, sizeof(ap_cert) - signed_from);
  ap_cert.signature.sig =
      Common::ec::Sign(m_key_entries[HANDLE_CONSOLE_KEY].data.data(), cert_digest.data());
  std::memcpy(ap_cert_out, &ap_cert, sizeof(ap_cert));

  const auto data_digest = Common::SHA1::CalculateDigest(data, data_size);
  const Common::ec::Signature data_sig = Common::ec::Sign(ap_private_key.data(), data_digest.data());
  std::copy(data_sig.begin(), data_sig.end(), sig_out);
}

// --------------------------------------------------------------------------------------------
// Initial contents

void IOSC::LoadDefaultEntries()
{
  // Stand-in values for a console without a keys.bin. (A real IOS has its own fall-backs for an
  // unprogrammed OTP; those are not emulated.)
  const auto bytes = [](const auto& table) { return std::vector<u8>(table.begin(), table.end()); };

  m_key_entries[HANDLE_CONSOLE_KEY] = {TYPE_SECRET_KEY, ObjectSubType::ECC233,
                                       bytes(DEFAULT_PRIVATE_KEY), 3};
  m_console_signature = DEFAULT_SIGNATURE;
  m_console_key_id = DEFAULT_KEY_ID;
  m_key_entries[HANDLE_CONSOLE_ID] = {TYPE_DATA, ObjectSubType::Data, {}, DEFAULT_DEVICE_ID,
                                      0xFFFFFFF};
  m_key_entries[HANDLE_FS_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128,
                                  std::vector<u8>(AES128_KEY_SIZE), 5};
  m_key_entries[HANDLE_FS_MAC] = {TYPE_SECRET_KEY, ObjectSubType::MAC, std::vector<u8>(20), 5};

  // Retail and development consoles differ in common key, root key and the CA/MS numbers.
  // (Retail key blobs come from CA00000001, which is used here although IOSC itself defaults
  // to 2.)
  const bool dev = m_console_type == ConsoleType::RVT;
  m_key_entries[HANDLE_COMMON_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128,
                                      bytes(dev ? COMMON_KEY_DEV : COMMON_KEY_RETAIL), 3};
  m_root_key_entry = {TYPE_PUBLIC_KEY, ObjectSubType::RSA4096,
                      bytes(dev ? ROOT_PUBLIC_KEY_DEV : ROOT_PUBLIC_KEY), RSA_EXPONENT_65537_BE,
                      0};
  m_ms_id = dev ? 3 : 2;
  m_ca_id = dev ? 2 : 1;

  m_key_entries[HANDLE_PRNG_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128,
                                    std::vector<u8>(AES128_KEY_SIZE), 3};
  m_key_entries[HANDLE_SD_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128, bytes(SD_KEY), 3};

  for (const Handle version_slot :
       {HANDLE_BOOT2_VERSION, HANDLE_UNKNOWN_8, HANDLE_UNKNOWN_9, HANDLE_FS_VERSION})
  {
    m_key_entries[version_slot] = {TYPE_DATA, ObjectSubType::Version, {}, 3};
  }

  m_key_entries[HANDLE_NEW_COMMON_KEY] = {TYPE_SECRET_KEY, ObjectSubType::AES128,
                                          bytes(KOREAN_COMMON_KEY), 3};
}

void IOSC::LoadEntries()
{
  File::IOFile file{File::GetUserPath(D_WIIROOT_IDX) + "keys.bin", "rb"};
  if (!file)
  {
    WARN_LOG_FMT(IOS, "No keys.bin found; using the built-in default keys");
    return;
  }

  KeysBin dump;
  if (!file.ReadBytes(&dump, sizeof(dump)))
  {
    ERROR_LOG_FMT(IOS, "keys.bin could not be read");
    return;
  }

  const auto* raw = reinterpret_cast<const u8*>(&dump);
  m_key_entries[HANDLE_CONSOLE_KEY].data.assign(dump.ng_priv.begin(), dump.ng_priv.end());
  m_console_signature = dump.ng_sig;
  m_ms_id = Common::swap32(dump.ms_id);
  m_ca_id = Common::swap32(dump.ca_id);
  m_console_key_id = Common::swap32(dump.ng_key_id);
  m_key_entries[HANDLE_CONSOLE_ID].misc_data = Common::swap32(dump.ng_id);
  m_key_entries[HANDLE_FS_KEY].data.assign(dump.nand_key.begin(), dump.nand_key.end());
  m_key_entries[HANDLE_FS_MAC].data.assign(raw + KEYS_BIN_NAND_HMAC_OFFSET,
                                           raw + KEYS_BIN_NAND_HMAC_OFFSET +
                                               KEYS_BIN_NAND_HMAC_SIZE);
  m_key_entries[HANDLE_PRNG_KEY].data.assign(dump.backup_key.begin(), dump.backup_key.end());
  m_key_entries[HANDLE_BOOT2_VERSION].misc_data = dump.counters[0].boot2version;
}

void IOSC::DoState(PointerWrap& p)
{
  for (KeyEntry& entry : m_key_entries)
    entry.DoState(p);
  p.Do(m_console_signature);
  p.Do(m_ms_id);
  p.Do(m_ca_id);
  p.Do(m_console_key_id);
}
}  // namespace IOS::HLE
