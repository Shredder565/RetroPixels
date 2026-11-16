// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// The LZO1X stream is a series of instructions. Each one is a back-reference (length and
// distance into the bytes already produced) whose last two "S" bits say how many literal bytes
// (0-3) follow it; a longer literal run is its own instruction, allowed only after a reference
// with S = 0. Instruction bytes by range:
//   0-15     after S=0: literal run of 3+L bytes (L=0: extended length)
//            after S=1..3: 2-byte reference, distance (next<<2)+D+1
//            after a run of 4+ literals: 3-byte reference, distance (next<<2)+D+2049
//   16-31    reference of 2+L bytes (L=0: extended), distance 16384 + H<<14 + 14-bit field;
//            distance 16384 exactly marks the end of the stream
//   32-63    reference of 2+L bytes (L=0: extended), 14-bit distance field + 1
//   64-127   reference of 3-4 bytes, distance (next<<3)+D+1
//   128-255  reference of 5-8 bytes, distance (next<<3)+D+1
// An extended length is a count of zero bytes (255 each) and a final non-zero byte. A first byte
// above 17 is a literal run of (byte - 17) bytes.

#include "Common/LZO1X.h"

#include <cstring>
#include <vector>

namespace Common::LZO1X
{
namespace
{
constexpr std::size_t kMinMatch = 4;
constexpr std::size_t kMaxDistance = 16384 + 0x7fff;
constexpr int kHashBits = 14;

u32 Load32(const u8* p)
{
  return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
}

u32 HashOf(const u8* p)
{
  return (Load32(p) * 2654435761u) >> (32 - kHashBits);
}

class Writer
{
public:
  explicit Writer(u8* out) : m_start(out), m_out(out) {}

  std::size_t Size() const { return m_out - m_start; }

  void Byte(u32 b) { *m_out++ = static_cast<u8>(b); }

  // The remainder of a length that did not fit in the instruction's own field (always >= 1).
  void Extended(std::size_t rest)
  {
    while (rest > 255)
    {
      Byte(0);
      rest -= 255;
    }
    Byte(static_cast<u32>(rest));
  }

  void Literals(const u8* from, std::size_t count)
  {
    if (count == 0)
      return;
    if (Size() == 0 && count <= 238)
    {
      Byte(17 + static_cast<u32>(count));
    }
    else if (count <= 3)
    {
      // Carried in the S bits of the reference just written.
      m_start[m_s_bits] |= static_cast<u8>(count);
    }
    else if (count <= 18)
    {
      Byte(static_cast<u32>(count - 3));
    }
    else
    {
      Byte(0);
      Extended(count - 18);
    }
    std::memcpy(m_out, from, count);
    m_out += count;
  }

  void Reference(std::size_t length, std::size_t distance)
  {
    if (length <= 8 && distance <= 2048)
    {
      const u32 d = static_cast<u32>(distance - 1);
      const u32 op = length <= 4 ? 0x40 | u32(length - 3) << 5 : 0x80 | u32(length - 5) << 5;
      m_s_bits = Size();
      Byte(op | (d & 7) << 2);
      Byte(d >> 3);
      return;
    }

    u32 field;
    if (distance <= 16384)
    {
      if (length <= 33)
      {
        Byte(0x20 | u32(length - 2));
      }
      else
      {
        Byte(0x20);
        Extended(length - 33);
      }
      field = static_cast<u32>(distance - 1);
    }
    else
    {
      const u32 m4_offset = static_cast<u32>(distance - 16384);
      const u32 high = (m4_offset >> 14) << 3;
      if (length <= 9)
      {
        Byte(0x10 | high | u32(length - 2));
      }
      else
      {
        Byte(0x10 | high);
        Extended(length - 9);
      }
      field = m4_offset & 0x3fff;
    }
    m_s_bits = Size();
    Byte(field << 2);
    Byte(field >> 6);
  }

  void End()
  {
    Byte(0x11);
    Byte(0);
    Byte(0);
  }

private:
  u8* m_start;
  u8* m_out;
  std::size_t m_s_bits = 0;
};
}  // namespace

std::size_t Compress(const u8* in, std::size_t length, u8* out)
{
  Writer writer(out);
  std::vector<u32> last_seen(std::size_t{1} << kHashBits, 0);

  std::size_t pos = 0;
  std::size_t literal_start = 0;
  while (length >= kMinMatch && pos <= length - kMinMatch)
  {
    const u32 hash = HashOf(in + pos);
    const std::size_t candidate = last_seen[hash];
    last_seen[hash] = static_cast<u32>(pos + 1);

    if (candidate != 0)
    {
      const std::size_t from = candidate - 1;
      const std::size_t distance = pos - from;
      if (distance <= kMaxDistance && Load32(in + from) == Load32(in + pos))
      {
        std::size_t match = kMinMatch;
        while (pos + match < length && in[from + match] == in[pos + match])
          ++match;

        writer.Literals(in + literal_start, pos - literal_start);
        writer.Reference(match, distance);
        // Remember a few positions inside the match so later data can refer back into it.
        const std::size_t end = pos + match;
        for (std::size_t p = pos + 1; p < end && p + kMinMatch <= length; p += 2)
          last_seen[HashOf(in + p)] = static_cast<u32>(p + 1);
        pos = end;
        literal_start = pos;
        continue;
      }
    }
    ++pos;
  }

  writer.Literals(in + literal_start, length - literal_start);
  writer.End();
  return writer.Size();
}

bool Decompress(const u8* in, std::size_t length, u8* out, std::size_t capacity,
                std::size_t* out_length)
{
  const u8* const in_end = in + length;
  std::size_t produced = 0;
  *out_length = 0;

  // How many literals the last instruction carried: 0, 1-3, or 4 for a literal run.
  u32 state = 0;

  auto read = [&](u32* value) {
    if (in == in_end)
      return false;
    *value = *in++;
    return true;
  };
  auto extended = [&](std::size_t base, std::size_t* value) {
    std::size_t total = base;
    for (;;)
    {
      u32 b;
      if (!read(&b))
        return false;
      if (b != 0)
      {
        *value = total + b;
        return true;
      }
      total += 255;
      if (total > capacity + 255)
        return false;
    }
  };
  auto literals = [&](std::size_t count) {
    if (count > static_cast<std::size_t>(in_end - in) || count > capacity - produced)
      return false;
    std::memcpy(out + produced, in, count);
    in += count;
    produced += count;
    return true;
  };
  auto copy = [&](std::size_t count, std::size_t distance) {
    if (distance > produced || count > capacity - produced)
      return false;
    // Byte by byte: the source may overlap the bytes being written.
    for (std::size_t i = 0; i < count; ++i, ++produced)
      out[produced] = out[produced - distance];
    return true;
  };

  if (in != in_end && *in > 17)
  {
    const u32 count = *in++ - 17;
    if (!literals(count))
      return false;
    state = count < 4 ? count : 4;
  }

  for (;;)
  {
    u32 op;
    if (!read(&op))
      return false;

    std::size_t count;
    std::size_t distance;
    u32 s_bits;

    if (op < 16)
    {
      if (state == 0)
      {
        if (op != 0)
          count = op + 3;
        else if (!extended(18, &count))
          return false;
        if (!literals(count))
          return false;
        state = 4;
        continue;
      }
      u32 next;
      if (!read(&next))
        return false;
      if (state == 4)
      {
        count = 3;
        distance = (std::size_t(next) << 2) + (op >> 2) + 2049;
      }
      else
      {
        count = 2;
        distance = (std::size_t(next) << 2) + (op >> 2) + 1;
      }
      s_bits = op & 3;
    }
    else if (op >= 64)
    {
      u32 next;
      if (!read(&next))
        return false;
      count = op >= 128 ? 5 + ((op >> 5) & 3) : 3 + ((op >> 5) & 1);
      distance = (std::size_t(next) << 3) + ((op >> 2) & 7) + 1;
      s_bits = op & 3;
    }
    else
    {
      const bool is_m3 = op >= 32;
      const u32 field_bits = is_m3 ? (op & 31) : (op & 7);
      if (field_bits != 0)
        count = 2 + field_bits;
      else if (!extended(is_m3 ? 33 : 9, &count))
        return false;
      u32 lo, hi;
      if (!read(&lo) || !read(&hi))
        return false;
      const std::size_t field = (lo >> 2) | (std::size_t(hi) << 6);
      s_bits = lo & 3;
      if (is_m3)
      {
        distance = field + 1;
      }
      else
      {
        distance = 16384 + (std::size_t((op >> 3) & 1) << 14) + field;
        if (distance == 16384)
        {
          *out_length = produced;
          return in == in_end;
        }
      }
    }

    if (!copy(count, distance) || !literals(s_bits))
      return false;
    state = s_bits;
  }
}
}  // namespace Common::LZO1X
