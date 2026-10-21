// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// Figure layout (public MIFARE Classic and Skylanders format documentation):
// - block 0: NUID, BCC, ATQA, SAK; block 1 (0x10): figure ID, variant at 0x1C, checksum at 0x1E;
// - every fourth block is a sector trailer: key A, access bits, key B;
// - two save areas at 0x80 and 0x240 (and two more at 0x110 and 0x2D0), each with a counter. The
//   one whose counter is one higher than the other's is the newest.
// Blocks 8 and up, except the trailers, are AES-128-ECB encrypted with a per-block key: the MD5
// of the first two blocks, the block number and a fixed string.

#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string>
#include <utility>

#include <mbedtls/aes.h>
#include <mbedtls/md5.h>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/Random.h"
#include "Common/StringUtil.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderCrypto.h"

namespace IOS::HLE::USB
{
namespace
{
// Byte array of ascii string " Copyright (C) 2010 Activision. All Rights Reserved.". The space at
// the start of the string is intentional
static constexpr std::array<u8, 0x35> HASH_CONST = {
    0x20, 0x43, 0x6F, 0x70, 0x79, 0x72, 0x69, 0x67, 0x68, 0x74, 0x20, 0x28, 0x43, 0x29,
    0x20, 0x32, 0x30, 0x31, 0x30, 0x20, 0x41, 0x63, 0x74, 0x69, 0x76, 0x69, 0x73, 0x69,
    0x6F, 0x6E, 0x2E, 0x20, 0x41, 0x6C, 0x6C, 0x20, 0x52, 0x69, 0x67, 0x68, 0x74, 0x73,
    0x20, 0x52, 0x65, 0x73, 0x65, 0x72, 0x76, 0x65, 0x64, 0x2E, 0x20};

// Where the block number goes in the key seed; the fixed string follows it.
constexpr std::size_t SEED_BLOCK_INDEX = 0x20;

// MIFARE access bits written into the trailers of new figures (sector 0, then the others).
constexpr std::array<u8, 4> FIRST_SECTOR_ACCESS{0x0F, 0x0F, 0x0F, 0x69};
constexpr std::array<u8, 4> OTHER_SECTOR_ACCESS{0x7F, 0x0F, 0x08, 0x69};

constexpr std::size_t SECTOR_SIZE = 4 * BLOCK_SIZE;
constexpr std::size_t KEY_A_OFFSET = 3 * BLOCK_SIZE;
constexpr std::size_t ACCESS_BITS_OFFSET = KEY_A_OFFSET + 6;

// Trailers and the first two sectors are stored in the clear.
bool IsPlainBlock(std::size_t block)
{
  return block < 8 || block % 4 == 3;
}

u16 Read16(std::span<const u8> bytes, std::size_t offset)
{
  return static_cast<u16>(bytes[offset] | (bytes[offset + 1] << 8));
}

u32 Read32(std::span<const u8> bytes, std::size_t offset)
{
  return static_cast<u32>(Read16(bytes, offset)) |
         (static_cast<u32>(Read16(bytes, offset + 2)) << 16);
}

u64 Read64(std::span<const u8> bytes, std::size_t offset)
{
  return static_cast<u64>(Read32(bytes, offset)) |
         (static_cast<u64>(Read32(bytes, offset + 4)) << 32);
}

void Write16(std::span<u8> bytes, std::size_t offset, u16 value)
{
  bytes[offset] = static_cast<u8>(value);
  bytes[offset + 1] = static_cast<u8>(value >> 8);
}

void Write32(std::span<u8> bytes, std::size_t offset, u32 value)
{
  Write16(bytes, offset, static_cast<u16>(value));
  Write16(bytes, offset + 2, static_cast<u16>(value >> 16));
}

// The AES key for one block.
std::array<u8, 16> BlockKey(std::array<u8, 0x56> seed, std::size_t block)
{
  seed[SEED_BLOCK_INDEX] = static_cast<u8>(block);
  std::array<u8, 16> key{};
  mbedtls_md5_ret(seed.data(), seed.size(), key.data());
  return key;
}

// The newest copy of the main save area (counter at +9).
std::size_t NewestArea(std::span<const u8> plain)
{
  return plain[0x89] + 1U != plain[0x249] ? 0x80 : 0x240;
}

// The save area that a write goes to: the older one.
std::pair<std::size_t, std::size_t> MainAreaToWrite(std::span<const u8> plain)
{
  if (plain[0x89] != plain[0x249] + 1U)
    return {0x80, 0x240};
  return {0x240, 0x80};
}

std::pair<std::size_t, std::size_t> SecondAreaToWrite(std::span<const u8> plain)
{
  if (plain[0x112] != plain[0x2D2] + 1U)
    return {0x110, 0x2D0};
  return {0x2D0, 0x110};
}

// After a main area changed: its checksums and counter (one past the other copy's).
void SealMainArea(std::span<u8> plain, std::size_t area, std::size_t other)
{
  using namespace SkylanderCrypto;
  u8* const base = plain.data() + area;
  ComputeChecksumType3(base + 0x50, base + 0xA);
  ComputeChecksumType2(base + 0x10, base + 0xC);
  base[9] = static_cast<u8>(plain[other + 9] + 1);
  ComputeChecksumType1(base, base + 0xE);
}

// The second area is not edited, but its copy is moved forward with the main one.
void AdvanceSecondArea(std::span<u8> plain)
{
  const auto [area, other] = SecondAreaToWrite(plain);
  u8* const base = plain.data() + area;
  base[2] = static_cast<u8>(plain[other + 2] + 1);
  SkylanderCrypto::ComputeChecksumType6(base, base);
}

SkylanderDateTime ReadDateTime(std::span<const u8> bytes, std::size_t offset)
{
  return {.minute = bytes[offset],
          .hour = bytes[offset + 1],
          .day = bytes[offset + 2],
          .month = bytes[offset + 3],
          .year = Read16(bytes, offset + 4)};
}

void WriteDateTime(std::span<u8> bytes, std::size_t offset, const SkylanderDateTime& time)
{
  bytes[offset] = time.minute;
  bytes[offset + 1] = time.hour;
  bytes[offset + 2] = time.day;
  bytes[offset + 3] = time.month;
  Write16(bytes, offset + 4, time.year);
}

// Area offsets of the character values.
constexpr std::size_t MONEY = 0x03;
constexpr std::size_t PLAYTIME = 0x05;
constexpr std::size_t VILLAINS = 0x14;
constexpr std::size_t NICKNAME_FIRST_HALF = 0x20;
constexpr std::size_t NICKNAME_SECOND_HALF = 0x40;
constexpr std::size_t LAST_PLACED = 0x50;
constexpr std::size_t HERO_LEVEL = 0x5A;
constexpr std::size_t LAST_RESET = 0x60;
// The nickname is split over two blocks of eight UTF-16 units.
constexpr std::size_t NICKNAME_HALF = 8;
}  // namespace

SkylanderFigure::SkylanderFigure(const std::string& file_path)
    : m_sky_file(file_path, "w+b"), m_data{}
{
}

SkylanderFigure::SkylanderFigure(File::IOFile file) : m_sky_file(std::move(file)), m_data{}
{
  m_sky_file.Seek(0, File::SeekOrigin::Begin);
  m_sky_file.ReadBytes(m_data.data(), m_data.size());
}

void SkylanderFigure::WriteSectorTrailers()
{
  for (std::size_t sector = 0; sector < 16; ++sector)
  {
    const auto& access = sector == 0 ? FIRST_SECTOR_ACCESS : OTHER_SECTOR_ACCESS;
    std::ranges::copy(access, m_data.begin() + static_cast<std::ptrdiff_t>(
                                                   sector * SECTOR_SIZE + ACCESS_BITS_OFFSET));
  }
}

void SkylanderFigure::WriteSectorKeys()
{
  const std::span<const u8, 4> nuid{m_data.data(), 4};
  for (u8 sector = 0; sector < 16; ++sector)
  {
    // Six bytes, most significant first.
    const u64 key = SkylanderCrypto::CalculateKeyA(sector, nuid);
    const std::size_t at = sector * SECTOR_SIZE + KEY_A_OFFSET;
    for (std::size_t i = 0; i < 6; ++i)
      m_data[at + i] = static_cast<u8>(key >> (8 * (5 - i)));
  }
}

bool SkylanderFigure::Create(u16 sky_id, u16 sky_var,
                             std::optional<std::array<u8, 4>> requested_nuid)
{
  if (!m_sky_file)
    return false;

  m_data.fill(0);
  WriteSectorTrailers();

  if (requested_nuid.has_value())
    std::ranges::copy(*requested_nuid, m_data.begin());
  else
    Common::Random::Generate(m_data.data(), 4);
  // Block check character, then ATQA and SAK.
  m_data[4] = static_cast<u8>(m_data[0] ^ m_data[1] ^ m_data[2] ^ m_data[3]);
  m_data[5] = 0x81;
  m_data[6] = 0x01;
  m_data[7] = 0x0F;

  Write16(m_data, 0x10, sky_id);
  Write16(m_data, 0x1C, sky_var);
  SkylanderCrypto::ComputeChecksumType0(m_data.data(), m_data.data() + 0x1E);

  WriteSectorKeys();
  Save();
  return true;
}

void SkylanderFigure::Save()
{
  m_sky_file.Seek(0, File::SeekOrigin::Begin);
  m_sky_file.WriteBytes(m_data.data(), FIGURE_SIZE);
}

void SkylanderFigure::Close()
{
  m_sky_file.Close();
}

bool SkylanderFigure::FileIsOpen() const
{
  return m_sky_file.IsOpen();
}

void SkylanderFigure::GetBlock(u8 index, u8* dest) const
{
  std::memcpy(dest, m_data.data() + index * BLOCK_SIZE, BLOCK_SIZE);
}

void SkylanderFigure::SetBlock(u8 block, const u8* buf)
{
  std::memcpy(m_data.data() + block * BLOCK_SIZE, buf, BLOCK_SIZE);
}

// The first two blocks, a slot for the block number, then the fixed string.
std::array<u8, 0x56> SkylanderFigure::BlockKeySeed() const
{
  std::array<u8, 0x56> seed{};
  std::copy_n(m_data.begin(), 2 * BLOCK_SIZE, seed.begin());
  std::ranges::copy(HASH_CONST, seed.begin() + SEED_BLOCK_INDEX + 1);
  return seed;
}

void SkylanderFigure::DecryptFigure(std::array<u8, FIGURE_SIZE>* dest) const
{
  const auto seed = BlockKeySeed();
  Dump plain{};

  for (std::size_t block = 0; block < BLOCK_COUNT; ++block)
  {
    const u8* const in = &m_data[block * BLOCK_SIZE];
    u8* const out = &plain[block * BLOCK_SIZE];
    if (IsPlainBlock(block))
    {
      std::copy_n(in, BLOCK_SIZE, out);
      continue;
    }
    // Unused blocks are all zero and stay so.
    if (std::all_of(in, in + BLOCK_SIZE, [](u8 b) { return b == 0; }))
      continue;

    const auto key = BlockKey(seed, block);
    mbedtls_aes_context aes{};
    mbedtls_aes_setkey_dec(&aes, key.data(), 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, in, out);
  }

  *dest = plain;
  DEBUG_LOG_FMT(IOS_USB, "Decrypted figure:\n{}", HexDump(plain.data(), plain.size()));
}

// Encrypts every block that is not stored in the clear (unused zero blocks included).
void SkylanderFigure::StoreEncrypted(std::span<const u8, FIGURE_SIZE> plain)
{
  const auto seed = BlockKeySeed();
  Dump encrypted{};

  for (std::size_t block = 0; block < BLOCK_COUNT; ++block)
  {
    const u8* const in = &plain[block * BLOCK_SIZE];
    u8* const out = &encrypted[block * BLOCK_SIZE];
    if (IsPlainBlock(block))
    {
      std::copy_n(in, BLOCK_SIZE, out);
      continue;
    }

    const auto key = BlockKey(seed, block);
    mbedtls_aes_context aes{};
    mbedtls_aes_setkey_enc(&aes, key.data(), 128);
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, in, out);
  }

  m_data = encrypted;
  DEBUG_LOG_FMT(IOS_USB, "Encrypted figure:\n{}", HexDump(encrypted.data(), encrypted.size()));
}

FigureData SkylanderFigure::GetData() const
{
  FigureData data{};
  data.figure_id = Read16(m_data, 0x10);
  data.variant_id = Read16(m_data, 0x1C);

  // Figures that are not in the list count as items.
  const auto known = list_skylanders.find({data.figure_id, data.variant_id});
  const Type type = known != list_skylanders.end() ? known->second.type : Type::Item;
  data.normalized_type = NormalizeSkylanderType(type);

  if (data.normalized_type != Type::Skylander && data.normalized_type != Type::Trophy)
    return data;

  Dump plain{};
  DecryptFigure(&plain);
  const std::span<const u8> area = std::span{plain}.subspan(NewestArea(plain));

  if (data.normalized_type == Type::Trophy)
  {
    data.trophy_data.unlocked_villains = area[VILLAINS];
    return data;
  }

  SkylanderData& sky = data.skylander_data;
  sky = {};
  sky.money = Read16(area, MONEY);
  sky.hero_level = Read16(area, HERO_LEVEL);
  sky.playtime = Read32(area, PLAYTIME);
  sky.last_reset = ReadDateTime(area, LAST_RESET);
  sky.last_placed = ReadDateTime(area, LAST_PLACED);
  sky.toy_code = SkylanderCrypto::ComputeToyCode(Read64(plain, 0x14));
  for (std::size_t i = 0; i < NICKNAME_HALF; ++i)
  {
    sky.nickname[i] = Read16(area, NICKNAME_FIRST_HALF + 2 * i);
    sky.nickname[NICKNAME_HALF + i] = Read16(area, NICKNAME_SECOND_HALF + 2 * i);
  }
  return data;
}

void SkylanderFigure::SetData(FigureData* data)
{
  Dump plain{};
  DecryptFigure(&plain);

  const bool is_skylander = data->normalized_type == Type::Skylander;
  if (is_skylander || data->normalized_type == Type::Trophy)
  {
    const auto [area_offset, other_offset] = MainAreaToWrite(plain);
    const std::span<u8> area = std::span{plain}.subspan(area_offset);

    if (is_skylander)
    {
      const SkylanderData& sky = data->skylander_data;
      Write16(area, MONEY, sky.money);
      Write16(area, HERO_LEVEL, sky.hero_level);
      Write32(area, PLAYTIME, sky.playtime);
      WriteDateTime(area, LAST_RESET, sky.last_reset);
      WriteDateTime(area, LAST_PLACED, sky.last_placed);
      for (std::size_t i = 0; i < NICKNAME_HALF; ++i)
      {
        Write16(area, NICKNAME_FIRST_HALF + 2 * i, sky.nickname[i]);
        Write16(area, NICKNAME_SECOND_HALF + 2 * i, sky.nickname[NICKNAME_HALF + i]);
      }
    }
    else
    {
      area[VILLAINS] = data->trophy_data.unlocked_villains;
    }

    SealMainArea(plain, area_offset, other_offset);
    AdvanceSecondArea(plain);
  }

  StoreEncrypted(plain);
  Save();
}
}  // namespace IOS::HLE::USB
