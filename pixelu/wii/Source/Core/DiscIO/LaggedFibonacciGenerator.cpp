// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/LaggedFibonacciGenerator.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Swap.h"

namespace DiscIO
{
namespace
{
// State word -> output word (host order). Bits 16-17 of the state are dropped.
constexpr u32 Twist(u32 x)
{
  return (x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000);
}

// Output word -> the state bits it determines (bits 16 and 17 come out as zero).
constexpr u32 Untwist(u32 y)
{
  return (y & 0xFF00FFFF) | ((y << 2) & 0x00FC0000);
}

// Output bits 22-23 and 24-25 both come from state bits 24-25, so they must agree.
constexpr bool CouldBeTwisted(u32 y)
{
  return ((y >> 2) & 0x00C00000) == (y & 0x00C00000);
}

u32 LoadBE(const u8* p)
{
  u32 value;
  std::memcpy(&value, p, sizeof(value));
  return Common::swap32(value);
}
}  // namespace

bool LaggedFibonacciGenerator::Expand(Words& words, const Words* expected)
{
  for (size_t n = SEED_SIZE; n < LONG_LAG; ++n)
  {
    const u32 value = (words[n - 17] << 23) ^ (words[n - 16] >> 9) ^ words[n - 1];
    if (expected && (value & ~u32(0x00030000)) != (*expected)[n])
      return false;
    words[n] = value;
  }
  return true;
}

void LaggedFibonacciGenerator::Start(const Words& words)
{
  for (size_t n = 0; n < LONG_LAG; ++n)
    m_table[n] = Common::swap32(Twist(words[n]));
  for (int n = 0; n < WARM_UP_ADVANCES; ++n)
    Advance();
  m_offset = 0;
}

void LaggedFibonacciGenerator::SetSeed(const u32 seed[SEED_SIZE])
{
  SetSeed(reinterpret_cast<const u8*>(seed));
}

void LaggedFibonacciGenerator::SetSeed(const u8 seed[SEED_SIZE * sizeof(u32)])
{
  Words words{};
  for (size_t n = 0; n < SEED_SIZE; ++n)
    words[n] = LoadBE(seed + n * sizeof(u32));
  Expand(words, nullptr);
  Start(words);
}

void LaggedFibonacciGenerator::Advance()
{
  // In place, front to back: the first 32 words use words from the end that haven't been
  // advanced yet; every later word uses one that already has.
  for (size_t n = 0; n < LONG_LAG; ++n)
    m_table[n] ^= n < SHORT_LAG ? m_table[n + LONG_LAG - SHORT_LAG] : m_table[n - SHORT_LAG];
}

void LaggedFibonacciGenerator::Unadvance(std::array<u32, LONG_LAG>& table, size_t begin,
                                         size_t end)
{
  // Back to front, so each word is undone while the word it was xored with still has the
  // value it had at that moment.
  for (size_t n = end; n-- > begin;)
    table[n] ^= n < SHORT_LAG ? table[n + LONG_LAG - SHORT_LAG] : table[n - SHORT_LAG];
}

void LaggedFibonacciGenerator::GetBytes(size_t count, u8* out)
{
  const u8* table = reinterpret_cast<const u8*>(m_table.data());
  while (count > 0)
  {
    const size_t n = std::min(count, TABLE_BYTES - m_offset);
    std::memcpy(out, table + m_offset, n);
    out += n;
    count -= n;
    m_offset += n;
    if (m_offset == TABLE_BYTES)
    {
      Advance();
      m_offset = 0;
    }
  }
}

u8 LaggedFibonacciGenerator::GetByte()
{
  u8 byte;
  GetBytes(1, &byte);
  return byte;
}

void LaggedFibonacciGenerator::Forward(size_t count)
{
  m_offset += count;
  for (; m_offset >= TABLE_BYTES; m_offset -= TABLE_BYTES)
    Advance();
}

size_t LaggedFibonacciGenerator::GetSeed(const u8* data, size_t size, size_t data_offset,
                                         u32 seed_out[SEED_SIZE])
{
  if ((reinterpret_cast<uintptr_t>(data) - data_offset) % sizeof(u32) != 0)
  {
    ASSERT(false);
    return 0;
  }

  // Work with whole stream words only.
  const size_t lead = (sizeof(u32) - data_offset % sizeof(u32)) % sizeof(u32);
  if (size < lead)
    return 0;
  const u8* words_in = data + lead;
  const size_t word_count = (size - lead) / sizeof(u32);
  const size_t first_word = (data_offset + lead) / sizeof(u32);

  // One table's worth of output is needed, and every word must have the twist's fingerprint.
  if (word_count < LONG_LAG)
    return 0;
  for (size_t n = 0; n < LONG_LAG; ++n)
  {
    if (!CouldBeTwisted(LoadBE(words_in + n * sizeof(u32))))
      return 0;
  }

  // Lay the words out by table position. They start at position `phase` of table number
  // `round`; the ones that wrap around to positions below `phase` are from the next table.
  const size_t round = first_word / LONG_LAG;
  const size_t phase = first_word % LONG_LAG;
  std::array<u32, LONG_LAG> table;
  for (size_t n = 0; n < LONG_LAG; ++n)
    std::memcpy(&table[(phase + n) % LONG_LAG], words_in + n * sizeof(u32), sizeof(u32));

  // Wind back to the table as it was straight after seeding.
  Unadvance(table, 0, phase);
  for (size_t n = 0; n < round + WARM_UP_ADVANCES; ++n)
    Unadvance(table, 0, LONG_LAG);

  Words output;
  Words known;
  for (size_t n = 0; n < LONG_LAG; ++n)
  {
    output[n] = Common::swap32(table[n]);
    known[n] = Untwist(output[n]);
  }

  // Bits 16-17 of a seed word n can be recovered from words n+15 and n+16, as the recurrence
  // makes word n+16 = (word n-1 << 23) ^ (word n >> 9) ^ word n+15. (For n = 0 the bits don't
  // affect any output, but they're filled the same way.)
  Words words{};
  for (size_t n = 0; n < SEED_SIZE; ++n)
    words[n] = known[n] | (((output[n + 16] ^ output[n + 15]) << 9) & 0x00030000);
  for (size_t n = 0; n < SEED_SIZE; ++n)
    seed_out[n] = Common::swap32(words[n]);

  // The seed is right if it regenerates the rest of the table.
  if (!Expand(words, &known))
    return 0;

  LaggedFibonacciGenerator generator;
  generator.Start(words);
  for (size_t n = 0; n < round; ++n)
    generator.Advance();
  generator.m_offset = data_offset % TABLE_BYTES;

  size_t matched = 0;
  while (matched < size && generator.GetByte() == data[matched])
    ++matched;
  return matched;
}

}  // namespace DiscIO
