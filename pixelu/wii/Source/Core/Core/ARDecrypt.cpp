// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Decoding of encrypted GameCube Action Replay codes.
//
// An encrypted AR line is 13 characters from a 32-symbol alphabet (written XXXX-XXXX-XXXXX).
// The 13 symbols carry 65 bits: a 64-bit DES block followed by one parity bit. The blocks of a
// code are DES-decrypted with the Action Replay's fixed key. The first decrypted line of a code is
// a verification line whose top nibble is a check value computed from a CRC-16 over the code.
//
// The DES below is a plain bit-level implementation of FIPS 46-3 (standard tables). Speed does not
// matter here: codes are decoded once when a game's ini is loaded.

#include "Core/ARDecrypt.h"

#include <array>
#include <bit>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

namespace ActionReplay
{
namespace
{
// The fixed DES key of the GameCube Action Replay (key material, kept byte for byte).
constexpr std::array<u8, 8> gensubtable{
    0x34, 0x1C, 0x84, 0x9E, 0xFD, 0xA4, 0xB6, 0x7B,
};

// ---- DES (FIPS 46-3) ----------------------------------------------------------------------------
// All permutation tables use the standard's numbering: bit 1 is the most significant bit.

constexpr std::array<u8, 64> kInitialPermutation{
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9,  1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7,
};

constexpr std::array<u8, 64> kFinalPermutation{
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9,  49, 17, 57, 25,
};

constexpr std::array<u8, 48> kExpansion{
    32, 1,  2,  3,  4,  5,  4,  5,  6,  7,  8,  9,  8,  9,  10, 11,
    12, 13, 12, 13, 14, 15, 16, 17, 16, 17, 18, 19, 20, 21, 20, 21,
    22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1,
};

constexpr std::array<u8, 32> kRoundPermutation{
    16, 7, 20, 21, 29, 12, 28, 17, 1,  15, 23, 26, 5,  18, 31, 10,
    2,  8, 24, 14, 32, 27, 3,  9,  19, 13, 30, 6,  22, 11, 4,  25,
};

constexpr std::array<u8, 56> kKeyPermutation1{
    57, 49, 41, 33, 25, 17, 9,  1,  58, 50, 42, 34, 26, 18, 10, 2,  59, 51, 43,
    35, 27, 19, 11, 3,  60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7,  62, 54,
    46, 38, 30, 22, 14, 6,  61, 53, 45, 37, 29, 21, 13, 5,  28, 20, 12, 4,
};

constexpr std::array<u8, 48> kKeyPermutation2{
    14, 17, 11, 24, 1,  5,  3,  28, 15, 6,  21, 10, 23, 19, 12, 4,
    26, 8,  16, 7,  27, 20, 13, 2,  41, 52, 31, 37, 47, 55, 30, 40,
    51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32,
};

constexpr std::array<u8, 16> kKeyRotations{1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

// The eight S-boxes, each 4 rows of 16 entries, indexed [row * 16 + column].
constexpr std::array<std::array<u8, 64>, 8> kSBoxes{{
    {14, 4,  13, 1, 2,  15, 11, 8,  3,  10, 6,  12, 5,  9,  0, 7,  0, 15, 7,  4,  14, 2,
     13, 1,  10, 6, 12, 11, 9,  5,  3,  8,  4,  1,  14, 8,  13, 6, 2, 11, 15, 12, 9,  7,
     3,  10, 5,  0, 15, 12, 8,  2,  4,  9,  1,  7,  5,  11, 3,  14, 10, 0, 6,  13},
    {15, 1,  8,  14, 6,  11, 3,  4,  9,  7, 2,  13, 12, 0, 5,  10, 3, 13, 4,  7,  15, 2,
     8,  14, 12, 0,  1,  10, 6,  9,  11, 5, 0,  14, 7,  11, 10, 4, 13, 1,  5,  8,  12, 6,
     9,  3,  2,  15, 13, 8,  10, 1,  3,  15, 4,  2,  11, 6,  7,  12, 0, 5,  14, 9},
    {10, 0,  9,  14, 6, 3,  15, 5,  1,  13, 12, 7,  11, 4,  2,  8,  13, 7, 0,  9,  3,  4,
     6,  10, 2,  8,  5, 14, 12, 11, 15, 1,  13, 6,  4,  9,  8,  15, 3,  0, 11, 1,  2,  12,
     5,  10, 14, 7,  1, 10, 13, 0,  6,  9,  8,  7,  4,  15, 14, 3,  11, 5, 2,  12},
    {7,  13, 14, 3,  0,  6,  9,  10, 1,  2, 8,  5, 11, 12, 4,  15, 13, 8, 11, 5,  6,  15,
     0,  3,  4,  7,  2,  12, 1,  10, 14, 9, 10, 6, 9,  0,  12, 11, 7,  13, 15, 1,  3,  14,
     5,  2,  8,  4,  3,  15, 0,  6,  10, 1, 13, 8, 9,  4,  5,  11, 12, 7,  2,  14},
    {2,  12, 4, 1,  7,  10, 11, 6,  8,  5,  3,  15, 13, 0, 14, 9,  14, 11, 2,  12, 4, 7,
     13, 1,  5, 0,  15, 10, 3,  9,  8,  6,  4,  2,  1,  11, 10, 13, 7,  8,  15, 9, 12, 5,
     6,  3,  0, 14, 11, 8,  12, 7,  1,  14, 2,  13, 6,  15, 0,  9,  10, 4,  5,  3},
    {12, 1,  10, 15, 9, 2,  6,  8,  0,  13, 3,  4,  14, 7,  5,  11, 10, 15, 4,  2,  7,  12,
     9,  5,  6,  1,  13, 14, 0, 11, 3,  8,  9,  14, 15, 5,  2,  8,  12, 3,  7,  0,  4,  10,
     1,  13, 11, 6,  4,  3,  2, 12, 9,  5,  15, 10, 11, 14, 1,  7,  6,  0,  8,  13},
    {4,  11, 2,  14, 15, 0, 8,  13, 3,  12, 9,  7,  5,  10, 6,  1,  13, 0, 11, 7,  4,  9,
     1,  10, 14, 3,  5,  12, 2, 15, 8,  6,  1,  4,  11, 13, 12, 3,  7,  14, 10, 15, 6, 8,
     0,  5,  9,  2,  6,  11, 13, 8, 1,  4,  10, 7,  9,  5,  0,  15, 14, 2,  3,  12},
    {13, 2,  8, 4,  6,  15, 11, 1,  10, 9,  3,  14, 5,  0,  12, 7,  1,  15, 13, 8, 10, 3,
     7,  4,  12, 5, 6,  11, 0,  14, 9,  2,  7,  11, 4,  1,  9,  12, 14, 2,  0,  6, 10, 13,
     15, 3,  5,  8, 2,  1,  14, 7,  4,  10, 8,  13, 15, 12, 9,  0,  3,  5,  6,  11},
}};

// Gathers bits of `input` (an `in_width`-bit value) as listed in `table` into a new value.
template <std::size_t N>
constexpr u64 Permute(u64 input, unsigned in_width, const std::array<u8, N>& table)
{
  u64 output = 0;
  for (const u8 position : table)
    output = (output << 1) | ((input >> (in_width - position)) & 1);
  return output;
}

using RoundKeys = std::array<u64, 16>;

constexpr RoundKeys MakeRoundKeys(const std::array<u8, 8>& key_bytes)
{
  u64 key = 0;
  for (const u8 byte : key_bytes)
    key = (key << 8) | byte;

  const u64 permuted = Permute(key, 64, kKeyPermutation1);
  u32 c = static_cast<u32>(permuted >> 28) & 0x0FFFFFFF;
  u32 d = static_cast<u32>(permuted) & 0x0FFFFFFF;

  const auto rotate28 = [](u32 half, unsigned count) {
    return ((half << count) | (half >> (28 - count))) & 0x0FFFFFFF;
  };

  RoundKeys keys{};
  for (std::size_t round = 0; round < keys.size(); ++round)
  {
    c = rotate28(c, kKeyRotations[round]);
    d = rotate28(d, kKeyRotations[round]);
    keys[round] = Permute((u64{c} << 28) | d, 56, kKeyPermutation2);
  }
  return keys;
}

constexpr RoundKeys s_round_keys = MakeRoundKeys(gensubtable);

constexpr u32 Feistel(u32 half, u64 round_key)
{
  const u64 mixed = Permute(half, 32, kExpansion) ^ round_key;
  u32 substituted = 0;
  for (std::size_t box = 0; box < kSBoxes.size(); ++box)
  {
    const auto six = static_cast<unsigned>((mixed >> (42 - 6 * box)) & 0x3F);
    const unsigned row = ((six >> 4) & 2) | (six & 1);
    const unsigned column = (six >> 1) & 0xF;
    substituted = (substituted << 4) | kSBoxes[box][row * 16 + column];
  }
  return static_cast<u32>(Permute(substituted, 32, kRoundPermutation));
}

constexpr u64 DESDecryptBlock(u64 block)
{
  const u64 permuted = Permute(block, 64, kInitialPermutation);
  u32 left = static_cast<u32>(permuted >> 32);
  u32 right = static_cast<u32>(permuted);
  for (std::size_t round = 0; round < s_round_keys.size(); ++round)
  {
    const u32 next_right = left ^ Feistel(right, s_round_keys[s_round_keys.size() - 1 - round]);
    left = right;
    right = next_right;
  }
  return Permute((u64{right} << 32) | left, 64, kFinalPermutation);
}

// The two words of a line form the DES block in memory order, i.e. each word byte-swapped.
void DecryptLine(u32& first, u32& second)
{
  const u64 block = (u64{Common::swap32(first)} << 32) | Common::swap32(second);
  const u64 plain = DESDecryptBlock(block);
  first = Common::swap32(static_cast<u32>(plain >> 32));
  second = Common::swap32(static_cast<u32>(plain));
}

// ---- Verification ------------------------------------------------------------------------------

// CRC-16/KERMIT (reflected polynomial 0x1021, zero start) over the words' bytes, low byte first.
u16 CodeChecksum(std::span<const u32> words)
{
  u16 crc = 0;
  for (const u32 word : words)
  {
    for (unsigned shift = 0; shift < 32; shift += 8)
    {
      crc ^= static_cast<u8>(word >> shift);
      for (int bit = 0; bit < 8; ++bit)
        crc = static_cast<u16>((crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1);
    }
  }
  return crc;
}

u32 CheckNibble(std::span<const u32> words)
{
  const u16 crc = CodeChecksum(words);
  return (crc ^ (crc >> 4) ^ (crc >> 8) ^ (crc >> 12)) & 0xF;
}

// ---- Text form ---------------------------------------------------------------------------------

// Value of one character. The alphabet skips I, L, O and S, which read as 1, 1, 0 and 5.
u32 SymbolValue(char c)
{
  constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRTUVWXYZ";
  const char upper = Common::ToUpper(c);
  switch (upper)
  {
  case 'I':
  case 'L':
    return 1;
  case 'O':
    return 0;
  case 'S':
    return 5;
  default:
    break;
  }
  const std::size_t index = alphabet.find(upper);
  return index == std::string_view::npos ? 0 : static_cast<u32>(index);
}

// Turns 13-character lines into word pairs. Returns the index of the first line whose parity bit
// does not match.
std::optional<std::size_t> LinesToWords(std::span<const std::string> lines, std::vector<u32>& words)
{
  words.reserve(lines.size() * 2);
  for (std::size_t line_index = 0; line_index < lines.size(); ++line_index)
  {
    const std::string& line = lines[line_index];

    // 12 symbols give the first 60 bits; the top 4 bits of the 13th finish the block and its
    // lowest bit is the parity of the block.
    u64 bits = 0;
    for (std::size_t i = 0; i < 12; ++i)
      bits = (bits << 5) | SymbolValue(line[i]);
    const u32 last = SymbolValue(line[12]);
    bits = (bits << 4) | (last >> 1);

    const auto first = static_cast<u32>(bits >> 32);
    const auto second = static_cast<u32>(bits);
    words.push_back(first);
    words.push_back(second);

    if ((std::popcount(first ^ second) & 1) != static_cast<int>(last & 1))
      return line_index;
  }
  return std::nullopt;
}

// Decrypts all lines in place and checks the verification line. On success, returns the game ID
// and region fields of the verification line.
std::optional<GameIDAndRegion> DecryptWords(std::span<u32> words)
{
  for (std::size_t i = 0; i + 1 < words.size(); i += 2)
    DecryptLine(words[i], words[i + 1]);

  const u32 check = words[0] >> 28;
  words[0] &= 0x0FFFFFFF;
  if (check != CheckNibble(words))
    return std::nullopt;

  // The verification line packs (after the check nibble) an 11-bit game ID and a 17-bit code ID;
  // the next word would hold the master-code flag, an unknown bit and a 2-bit region. As in the
  // program this was modelled on, the region bits are taken from the first word plus 4 rather
  // than from the second word.
  const u32 head = words[0];
  return GameIDAndRegion{(head >> 17) & 0x7FF, ((head + 4) >> 28) & 3};
}
}  // namespace

std::optional<GameIDAndRegion> DecryptARCode(std::span<const std::string> encrypted_lines,
                                             std::vector<AREntry>* result)
{
  if (encrypted_lines.empty())
    return std::nullopt;

  std::vector<u32> words;
  if (const auto bad_line = LinesToWords(encrypted_lines, words))
  {
    PanicAlertFmtT(
        "Action Replay Code Decryption Error:\nParity Check Failed\n\nCulprit Code:\n{0}",
        encrypted_lines[*bad_line]);
    return std::nullopt;
  }

  const std::optional<GameIDAndRegion> info = DecryptWords(words);
  if (info)
  {
    INFO_LOG_FMT(ACTIONREPLAY, "Decrypted AR Code: GameID:{:08X} Region:{}", info->game_id,
                 info->region);
    // The verification line itself is not part of the code.
    for (std::size_t i = 2; i < words.size(); i += 2)
    {
      result->emplace_back(words[i], words[i + 1]);
      DEBUG_LOG_FMT(ACTIONREPLAY, "Decrypted AR Code: {:08X} {:08X}", words[i], words[i + 1]);
    }
    return info;
  }

  ERROR_LOG_FMT(ACTIONREPLAY,
                "Decryption Error: CRC Check Failed. "
                "First Code in Block (should be verification code): {}",
                encrypted_lines[0]);
  for (std::size_t i = 0; i < words.size(); i += 2)
  {
    result->emplace_back(words[i], words[i + 1]);
    WARN_LOG_FMT(ACTIONREPLAY, "Decrypted AR Code without verification code: {:08X} {:08X}",
                 words[i], words[i + 1]);
  }
  return std::nullopt;
}

}  // namespace ActionReplay
