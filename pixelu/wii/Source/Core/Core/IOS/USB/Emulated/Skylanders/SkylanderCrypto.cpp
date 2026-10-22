// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/USB/Emulated/Skylanders/SkylanderCrypto.h"

#include <array>
#include <cstddef>
#include <span>

#include "Common/CommonTypes.h"

namespace IOS::HLE::USB::SkylanderCrypto
{
namespace
{
// A CRC of `Width` bits processed most significant bit first, without reflection or a final
// XOR.
template <unsigned Width>
u64 ShiftRegisterCRC(std::span<const u8> data, u64 polynomial, u64 initial)
{
  constexpr u64 MASK = (u64{1} << Width) - 1;
  constexpr u64 TOP_BIT = u64{1} << (Width - 1);

  u64 reg = initial & MASK;
  for (const u8 byte : data)
  {
    reg ^= u64{byte} << (Width - 8);
    for (int bit = 0; bit < 8; ++bit)
    {
      const bool carry = (reg & TOP_BIT) != 0;
      reg = (reg << 1) & MASK;
      if (carry)
        reg ^= polynomial & MASK;
    }
  }
  return reg;
}

void StoreChecksum(std::span<const u8> input, u8* output)
{
  // Stored least significant byte first.
  const u16 crc = ComputeCRC16(input);
  output[0] = static_cast<u8>(crc);
  output[1] = static_cast<u8>(crc >> 8);
}

// Copies `size` bytes from `from` to `to`.
void Take(std::span<u8> to, const u8* from, std::size_t size)
{
  for (std::size_t i = 0; i < size; ++i)
    to[i] = from[i];
}
}  // namespace

u16 ComputeCRC16(std::span<const u8> data)
{
  return static_cast<u16>(ShiftRegisterCRC<16>(data, 0x1021, 0xFFFF));
}

u64 ComputeCRC48(std::span<const u8> data)
{
  return ShiftRegisterCRC<48>(data, 0x42f0e1eba9ea3693,
                              2ULL * 2ULL * 3ULL * 1103ULL * 12868356821ULL);
}

u64 CalculateKeyA(u8 sector, std::span<const u8, 0x4> nuid)
{
  // Sector 0 uses one fixed key on every figure.
  if (sector == 0)
  {
    return 73ULL * 2017ULL * 560381651ULL;
  }

  // The others are the CRC48 of the NUID and the sector number, with its six bytes reversed.
  const std::array<u8, 5> input{nuid[0], nuid[1], nuid[2], nuid[3], sector};
  const u64 crc = ComputeCRC48(input);
  u64 key = 0;
  for (int i = 0; i < 6; ++i)
    key = (key << 8) | ((crc >> (8 * i)) & 0xFF);
  return key;
}

// The header block pair: 0x1E bytes.
void ComputeChecksumType0(const u8* data_start, u8* output)
{
  std::array<u8, 0x1E> input{};
  Take(input, data_start, input.size());
  StoreChecksum(input, output);
}

// One block whose last two bytes (the checksum itself) count as 05 00.
void ComputeChecksumType1(const u8* data_start, u8* output)
{
  std::array<u8, 0x10> input{};
  Take(input, data_start, 0x0E);
  input[0x0E] = 0x05;
  input[0x0F] = 0x00;
  StoreChecksum(input, output);
}

// Three data blocks around a sector trailer: two blocks, skip 0x10 bytes, one more block.
void ComputeChecksumType2(const u8* data_start, u8* output)
{
  std::array<u8, 0x30> input{};
  Take(input, data_start, 0x20);
  Take(std::span{input}.subspan(0x20), data_start + 0x30, 0x10);
  StoreChecksum(input, output);
}

// As type 2, followed by 0xE0 zero bytes.
void ComputeChecksumType3(const u8* data_start, u8* output)
{
  std::array<u8, 0x110> input{};
  Take(input, data_start, 0x20);
  Take(std::span{input}.subspan(0x20), data_start + 0x30, 0x10);
  StoreChecksum(input, output);
}

// Four data blocks around a sector trailer, starting with 06 01 in place of the checksum.
void ComputeChecksumType6(const u8* data_start, u8* output)
{
  std::array<u8, 0x40> input{};
  Take(input, data_start, 0x20);
  Take(std::span{input}.subspan(0x20), data_start + 0x30, 0x20);
  input[0] = 0x06;
  input[1] = 0x01;
  StoreChecksum(input, output);
}

std::array<u8, 11> ComputeToyCode(u64 code)
{
  std::array<u8, 11> text{};
  if (code == 0)
  {
    text[0] = 'N';
    text[1] = '/';
    text[2] = 'A';
    return text;
  }

  // Ten base-29 digits, most significant first, split 5-5 by a dash.
  constexpr char DIGITS[] = "23456789BCDFGHJKLMNPQRSTVWXYZ";
  constexpr std::size_t DIGIT_COUNT = 10;
  for (std::size_t i = 0; i < DIGIT_COUNT; ++i)
  {
    const std::size_t from_end = DIGIT_COUNT - 1 - i;
    const std::size_t position = from_end < 5 ? from_end : from_end + 1;
    text[position] = static_cast<u8>(DIGITS[code % 29]);
    code /= 29;
  }
  text[5] = '-';
  return text;
}
}  // namespace IOS::HLE::USB::SkylanderCrypto
