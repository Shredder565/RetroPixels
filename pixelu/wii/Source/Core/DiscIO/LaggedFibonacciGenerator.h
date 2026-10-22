// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>

#include "Common/CommonTypes.h"

namespace DiscIO
{
// The pseudorandom generator that produces the "junk" padding on GameCube and Wii discs, a
// lagged Fibonacci generator with lags 521 and 32 over 32-bit words. RVZ stores a 17-word
// seed instead of the junk itself.
//
// Seeding fills 521 words from the 17 seed words with a shift/xor recurrence. The words are
// then output as big endian bytes with an odd twist: bits 16-23 of each output word are bits
// 18-25 of the state word. Each time all 2084 bytes have been output, the table is advanced
// by xoring every word with the word 32 (cyclically 521 - 32) places before it. Four advances
// happen right after seeding, before any output.
class LaggedFibonacciGenerator
{
public:
  static constexpr size_t SEED_SIZE = 17;

  // Tries to find the seed of the junk in data[0, size), where data[0] is byte data_offset of
  // the junk stream. On success, writes the seed (as big endian words) to seed_out and returns
  // how many bytes from the start of data the seed reproduces, which may be anything from 0
  // to size. Requires data - data_offset to be 4-byte aligned.
  static size_t GetSeed(const u8* data, size_t size, size_t data_offset, u32 seed_out[SEED_SIZE]);

  // A seed as written by GetSeed (big endian words in memory).
  void SetSeed(const u32 seed[SEED_SIZE]);
  // A seed as 68 bytes holding 17 big endian words.
  void SetSeed(const u8 seed[SEED_SIZE * sizeof(u32)]);

  // Output, after SetSeed.
  void GetBytes(size_t count, u8* out);
  u8 GetByte();

  // Skips `count` bytes of output.
  void Forward(size_t count);

private:
  static constexpr size_t LONG_LAG = 521;
  static constexpr size_t SHORT_LAG = 32;
  static constexpr size_t TABLE_BYTES = LONG_LAG * sizeof(u32);
  static constexpr int WARM_UP_ADVANCES = 4;

  // Host-order state words, before the output twist.
  using Words = std::array<u32, LONG_LAG>;

  // Fills words [SEED_SIZE, LONG_LAG) from the seed words. With `expected`, fails as soon as a
  // word disagrees with the bits of it that are known.
  static bool Expand(Words& words, const Words* expected);

  // Installs freshly expanded words as the output table and does the warm-up advances.
  void Start(const Words& words);

  void Advance();
  // Undoes Advance() for table positions [begin, end), provided positions outside that range
  // already hold their pre-advance values.
  static void Unadvance(std::array<u32, LONG_LAG>& table, size_t begin, size_t end);

  // The current output bytes. Kept as raw memory: each word holds four output bytes in order.
  std::array<u32, LONG_LAG> m_table{};
  size_t m_offset = 0;  // next byte of m_table to output
};

}  // namespace DiscIO
