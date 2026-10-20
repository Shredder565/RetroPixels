// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DiscIO/WIACompression.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <bzlib.h>
#include <lzma.h>
#include <zstd.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/Swap.h"
#include "DiscIO/LaggedFibonacciGenerator.h"
#include "DiscIO/WIABlob.h"

namespace DiscIO
{
namespace
{
// Purge stores runs of data as (u32 BE offset, u32 BE size, bytes) records; everything not
// covered is zero. A SHA-1 of everything before it ends the stream.
constexpr size_t PURGE_RECORD_HEADER = 8;
constexpr size_t PURGE_HASH = Common::SHA1::DIGEST_LEN;

// RVZ packing: records of a u32 BE size, followed either by that many bytes of data or (when
// bit 31 of the size is set) by a junk generator seed.
constexpr size_t RVZ_SIZE_FIELD = sizeof(u32);
constexpr size_t RVZ_SEED_BYTES = LaggedFibonacciGenerator::SEED_SIZE * sizeof(u32);
constexpr u32 RVZ_JUNK_FLAG = 0x80000000;
// The junk on a disc restarts its sequence at every 32 KiB sector.
constexpr u64 JUNK_SECTOR = 0x8000;

// LZMA2's one-byte dictionary size code (as in the .xz format), for codes 0 to 39.
u32 Lzma2DictSize(u32 code)
{
  return (2 | (code & 1)) << (code / 2 + 11);
}

unsigned int ClampToUInt(size_t x)
{
  return static_cast<unsigned int>(std::min<size_t>(x, std::numeric_limits<unsigned int>::max()));
}

// ---------------------------------------------------------------------------------------------

class CopyDecoder final : public Decoder
{
public:
  bool Step(DecodeInput& in, DecodeOutput& out) override
  {
    const size_t n = std::min(in.loaded - in.pos, out.capacity - out.pos);
    std::memcpy(out.data + out.pos, in.data + in.pos, n);
    in.pos += n;
    out.pos += n;
    m_finished = in.pos == in.total;
    return true;
  }
};

class PurgeDecoder final : public Decoder
{
public:
  explicit PurgeDecoder(u64 output_size) : m_output_size(output_size) {}

  bool Step(DecodeInput& in, DecodeOutput& out) override
  {
    if (!m_hash)
    {
      // Whatever precedes the records (the hash exception lists) is covered by the hash too.
      m_hash = Common::SHA1::CreateContext();
      m_hash->Update(in.data, in.pos);
    }

    while (!m_finished && in.pos != in.loaded &&
           (m_header_fill < PURGE_RECORD_HEADER || out.pos != out.capacity))
    {
      if (m_header_fill == 0 && in.total >= PURGE_HASH && in.pos == in.total - PURGE_HASH)
        return Trailer(in, out);

      if (m_header_fill < PURGE_RECORD_HEADER)
      {
        const size_t n = std::min(in.loaded - in.pos, PURGE_RECORD_HEADER - m_header_fill);
        std::memcpy(m_header.data() + m_header_fill, in.data + in.pos, n);
        m_hash->Update(in.data + in.pos, n);
        in.pos += n;
        m_header_fill += n;
        if (m_header_fill < PURGE_RECORD_HEADER)
          return true;
      }

      const u64 start = Common::swap32(m_header.data());
      const u64 end = start + Common::swap32(m_header.data() + 4);

      // Zeroes up to the record, then the record's bytes.
      if (m_produced < start)
        Zeroes(out, start - m_produced);
      if (m_produced >= start && m_produced < end)
      {
        const size_t n = std::min<size_t>({end - m_produced, out.capacity - out.pos,
                                           in.loaded - in.pos});
        std::memcpy(out.data + out.pos, in.data + in.pos, n);
        m_hash->Update(in.data + in.pos, n);
        in.pos += n;
        out.pos += n;
        m_produced += n;
      }
      if (m_produced >= end)
        m_header_fill = 0;  // on to the next record
    }
    return true;
  }

private:
  void Zeroes(DecodeOutput& out, u64 count)
  {
    const size_t n = static_cast<size_t>(std::min<u64>(count, out.capacity - out.pos));
    std::memset(out.data + out.pos, 0, n);
    out.pos += n;
    m_produced += n;
  }

  bool Trailer(DecodeInput& in, DecodeOutput& out)
  {
    // After the last record, the output is zero up to its full size.
    Zeroes(out, m_output_size - m_produced);
    if (m_produced != m_output_size || in.loaded != in.total)
      return true;

    const Common::SHA1::Digest actual = m_hash->Finish();
    const bool good = std::memcmp(actual.data(), in.data + in.pos, PURGE_HASH) == 0;
    in.pos += PURGE_HASH;
    m_finished = true;
    return good;
  }

  const u64 m_output_size;
  std::unique_ptr<Common::SHA1::Context> m_hash;
  std::array<u8, PURGE_RECORD_HEADER> m_header{};
  size_t m_header_fill = 0;
  u64 m_produced = 0;
};

class Bzip2Decoder final : public Decoder
{
public:
  ~Bzip2Decoder() override
  {
    if (m_open)
      BZ2_bzDecompressEnd(&m_stream);
  }

  bool Step(DecodeInput& in, DecodeOutput& out) override
  {
    if (!m_open)
    {
      if (BZ2_bzDecompressInit(&m_stream, 0, 0) != BZ_OK)
        return false;
      m_open = true;
    }

    char* const next_in = reinterpret_cast<char*>(const_cast<u8*>(in.data + in.pos));
    char* const next_out = reinterpret_cast<char*>(out.data + out.pos);
    m_stream.next_in = next_in;
    m_stream.avail_in = ClampToUInt(in.loaded - in.pos);
    m_stream.next_out = next_out;
    m_stream.avail_out = ClampToUInt(out.capacity - out.pos);

    const int result = BZ2_bzDecompress(&m_stream);
    in.pos += m_stream.next_in - next_in;
    out.pos += m_stream.next_out - next_out;

    m_finished = result == BZ_STREAM_END;
    return result == BZ_OK || result == BZ_STREAM_END;
  }

private:
  bz_stream m_stream{};
  bool m_open = false;
};

class LzmaDecoder final : public Decoder
{
public:
  LzmaDecoder(bool lzma2, const u8* props, size_t props_size)
  {
    if (!lzma2 && props_size == 5)
    {
      // One byte packing lc/lp/pb, then the dictionary size (u32 LE).
      const u8 packed = props[0];
      std::memcpy(&m_options.dict_size, props + 1, sizeof(u32));
      if (packed < 9 * 5 * 5)
      {
        m_options.lc = packed % 9;
        m_options.lp = packed / 9 % 5;
        m_options.pb = packed / 9 / 5;
        m_usable = true;
      }
    }
    else if (lzma2 && props_size == 1 && props[0] <= 40)
    {
      m_options.dict_size = props[0] == 40 ? 0xFFFFFFFF : Lzma2DictSize(props[0]);
      m_usable = true;
    }

    m_filters[0] = {lzma2 ? LZMA_FILTER_LZMA2 : LZMA_FILTER_LZMA1, &m_options};
    m_filters[1] = {LZMA_VLI_UNKNOWN, nullptr};
  }

  ~LzmaDecoder() override
  {
    if (m_open)
      lzma_end(&m_stream);
  }

  bool Step(DecodeInput& in, DecodeOutput& out) override
  {
    if (!m_open)
    {
      if (!m_usable || lzma_raw_decoder(&m_stream, m_filters) != LZMA_OK)
        return false;
      m_open = true;
    }

    const u8* const next_in = in.data + in.pos;
    u8* const next_out = out.data + out.pos;
    m_stream.next_in = next_in;
    m_stream.avail_in = in.loaded - in.pos;
    m_stream.next_out = next_out;
    m_stream.avail_out = out.capacity - out.pos;

    const lzma_ret result = lzma_code(&m_stream, LZMA_RUN);
    in.pos += m_stream.next_in - next_in;
    out.pos += m_stream.next_out - next_out;

    m_finished = result == LZMA_STREAM_END;
    return result == LZMA_OK || result == LZMA_STREAM_END;
  }

private:
  lzma_stream m_stream = LZMA_STREAM_INIT;
  lzma_options_lzma m_options{};
  lzma_filter m_filters[2];
  bool m_usable = false;
  bool m_open = false;
};

class ZstdDecoder final : public Decoder
{
public:
  ZstdDecoder() : m_stream(ZSTD_createDStream()) {}
  ~ZstdDecoder() override { ZSTD_freeDStream(m_stream); }

  bool Step(DecodeInput& in, DecodeOutput& out) override
  {
    if (!m_stream)
      return false;

    ZSTD_inBuffer src{in.data, in.loaded, in.pos};
    ZSTD_outBuffer dst{out.data, out.capacity, out.pos};
    const size_t result = ZSTD_decompressStream(m_stream, &dst, &src);
    in.pos = src.pos;
    out.pos = dst.pos;

    m_finished = result == 0;
    return !ZSTD_isError(result);
  }

private:
  ZSTD_DStream* m_stream;
};

// ---------------------------------------------------------------------------------------------

class RVZUnpacker final : public Decoder
{
public:
  RVZUnpacker(std::unique_ptr<Decoder> inner, std::vector<u8> already_decoded, u64 data_offset,
              u32 packed_size)
      : m_inner(std::move(inner)), m_packed(std::move(already_decoded)),
        m_packed_filled(m_packed.size()), m_inner_produced(m_packed.size()),
        m_data_offset(data_offset), m_packed_size(packed_size)
  {
  }

  bool Finished() const override
  {
    return m_record_left == 0 && m_inner_produced == m_packed_size &&
           m_packed_filled == m_packed_read && m_inner->Finished();
  }

  bool Step(DecodeInput& in, DecodeOutput& out) override
  {
    while (out.pos != out.capacity && !Finished())
    {
      if (m_record_left == 0)
      {
        // Start of a record: decode its header into m_packed.
        if (m_packed_read == m_packed_filled)
        {
          m_packed.assign(RVZ_SIZE_FIELD, 0);
          m_packed_filled = 0;
          m_packed_read = 0;
        }

        if (const std::optional<bool> wait = NeedPacked(in, m_packed_read + RVZ_SIZE_FIELD))
          return *wait;
        const u32 field = Common::swap32(m_packed.data() + m_packed_read);

        m_in_junk = (field & RVZ_JUNK_FLAG) != 0;
        if (m_in_junk)
        {
          const size_t seed_at = m_packed_read + RVZ_SIZE_FIELD;
          if (const std::optional<bool> wait = NeedPacked(in, seed_at + RVZ_SEED_BYTES))
            return *wait;
          m_junk.SetSeed(m_packed.data() + seed_at);
          m_junk.Forward(m_data_offset % JUNK_SECTOR);
          m_packed_read += RVZ_SEED_BYTES;
        }
        m_packed_read += RVZ_SIZE_FIELD;
        m_record_left = field & ~RVZ_JUNK_FLAG;
      }

      size_t n = std::min<size_t>(m_record_left, out.capacity - out.pos);
      if (m_in_junk)
      {
        m_junk.GetBytes(n, out.data + out.pos);
        out.pos += n;
      }
      else if (m_packed_read != m_packed_filled)
      {
        // Data bytes that were decoded along with a header.
        n = std::min(n, m_packed_filled - m_packed_read);
        std::memcpy(out.data + out.pos, m_packed.data() + m_packed_read, n);
        m_packed_read += n;
        out.pos += n;
      }
      else
      {
        // Straight from the inner decoder into the output, but no further than this record.
        DecodeOutput limited{out.data, out.pos + n, out.pos};
        if (!m_inner->Step(in, limited))
          return false;
        n = limited.pos - out.pos;
        out.pos = limited.pos;
        if (!CountInner(n))
          return false;
        if (n == 0)
          return true;  // needs more input
      }

      m_data_offset += n;
      m_record_left -= static_cast<u32>(n);
    }

    // With the output full, let the inner decoder consume what's left of the input (zstd may
    // still have to read its frame's end).
    if (out.pos == out.capacity && in.loaded != in.pos)
    {
      if (!m_inner->Step(in, out))
        return false;
    }
    return true;
  }

private:
  // Makes sure m_packed holds `needed` bytes. nullopt: they're there. Otherwise the value to
  // return from Step: true to wait for more input, false on an error.
  std::optional<bool> NeedPacked(DecodeInput& in, size_t needed)
  {
    if (m_packed.size() < needed)
      m_packed.resize(needed);
    if (m_packed_filled >= needed)
      return std::nullopt;

    DecodeOutput into{m_packed.data(), m_packed.size(), m_packed_filled};
    if (!m_inner->Step(in, into))
      return false;
    const size_t n = into.pos - m_packed_filled;
    m_packed_filled = into.pos;
    if (!CountInner(n))
      return false;
    if (m_packed_filled < needed)
      return true;
    return std::nullopt;
  }

  // The inner decoder may not produce more than the packed size the group entry claims.
  bool CountInner(size_t n)
  {
    m_inner_produced += n;
    return m_inner_produced <= m_packed_size;
  }

  std::unique_ptr<Decoder> m_inner;
  std::vector<u8> m_packed;  // record headers (and sometimes data) decoded by m_inner
  size_t m_packed_filled;
  size_t m_packed_read = 0;
  size_t m_inner_produced;
  u64 m_data_offset;
  const u32 m_packed_size;

  u32 m_record_left = 0;
  bool m_in_junk = false;
  LaggedFibonacciGenerator m_junk;
};

// ---------------------------------------------------------------------------------------------

// Output storage shared by the library-backed encoders: grows by the input size when input is
// fed, and in small steps whenever the library runs out of room.
class GrowingOutput
{
public:
  void Reset() { m_bytes.clear(); }
  // Grows the buffer; returns the new room after `used` bytes.
  size_t Grow(size_t used, size_t extra)
  {
    m_bytes.resize(m_bytes.size() + extra);
    return m_bytes.size() - used;
  }
  u8* At(size_t pos) { return m_bytes.data() + pos; }
  const u8* Data() const { return m_bytes.data(); }

  static constexpr size_t STEP = 0x100;

private:
  std::vector<u8> m_bytes;
};

class PurgeEncoder final : public Encoder
{
public:
  bool Begin(std::optional<u64>) override
  {
    m_out.clear();
    m_size = 0;
    m_hash = Common::SHA1::CreateContext();
    return true;
  }

  bool HashOnly(const u8* data, size_t size) override
  {
    m_hash->Update(data, size);
    return true;
  }

  bool Feed(const u8* data, size_t size) override
  {
    ASSERT_MSG(DISCIO, m_size == 0, "PurgeEncoder takes its input in one piece");

    // Records never take more room than the zeroes they leave out, so this always suffices.
    m_out.resize(size + PURGE_RECORD_HEADER + PURGE_HASH);

    size_t pos = 0;
    while (true)
    {
      const size_t start = static_cast<size_t>(
          std::find_if(data + pos, data + size, [](u8 b) { return b != 0; }) - data);
      if (start == size)
        break;

      // A record ends at its last non-zero byte before a run of more zeroes than a record
      // header would cost (or at the end of the input).
      size_t end = start;
      size_t zero_run = 0;
      for (size_t i = start; i < size && zero_run <= PURGE_RECORD_HEADER; ++i)
      {
        if (data[i] == 0)
        {
          ++zero_run;
        }
        else
        {
          zero_run = 0;
          end = i + 1;
        }
      }

      const u32 header[2] = {Common::swap32(static_cast<u32>(start)),
                             Common::swap32(static_cast<u32>(end - start))};
      std::memcpy(m_out.data() + m_size, header, sizeof(header));
      m_size += sizeof(header);
      std::memcpy(m_out.data() + m_size, data + start, end - start);
      m_size += end - start;
      pos = end;
    }
    return true;
  }

  bool Finish() override
  {
    m_hash->Update(m_out.data(), m_size);
    const Common::SHA1::Digest digest = m_hash->Finish();
    std::memcpy(m_out.data() + m_size, digest.data(), digest.size());
    m_size += digest.size();
    ASSERT(m_size <= m_out.size());
    return true;
  }

  const u8* Data() const override { return m_out.data(); }
  size_t Size() const override { return m_size; }

private:
  std::vector<u8> m_out;
  size_t m_size = 0;
  std::unique_ptr<Common::SHA1::Context> m_hash;
};

class Bzip2Encoder final : public Encoder
{
public:
  explicit Bzip2Encoder(int level) : m_level(level) {}
  ~Bzip2Encoder() override { BZ2_bzCompressEnd(&m_stream); }

  bool Begin(std::optional<u64>) override
  {
    ASSERT_MSG(DISCIO, m_stream.state == nullptr, "Bzip2Encoder::Begin called twice");
    m_out.Reset();
    m_stream.next_out = reinterpret_cast<char*>(m_out.At(0));
    m_stream.avail_out = 0;
    return BZ2_bzCompressInit(&m_stream, m_level, 0, 0) == BZ_OK;
  }

  bool Feed(const u8* data, size_t size) override
  {
    m_stream.next_in = reinterpret_cast<char*>(const_cast<u8*>(data));
    m_stream.avail_in = static_cast<unsigned int>(size);
    Grow(size);
    while (m_stream.avail_in != 0)
    {
      if (m_stream.avail_out == 0)
        Grow(GrowingOutput::STEP);
      if (BZ2_bzCompress(&m_stream, BZ_RUN) != BZ_RUN_OK)
        return false;
    }
    return true;
  }

  bool Finish() override
  {
    bool ok = true;
    int result;
    do
    {
      if (m_stream.avail_out == 0)
        Grow(GrowingOutput::STEP);
      result = BZ2_bzCompress(&m_stream, BZ_FINISH);
      if (result != BZ_FINISH_OK && result != BZ_STREAM_END)
        ok = false;
    } while (result == BZ_FINISH_OK);

    if (BZ2_bzCompressEnd(&m_stream) != BZ_OK)
      ok = false;
    return ok;
  }

  const u8* Data() const override { return m_out.Data(); }
  size_t Size() const override
  {
    return static_cast<size_t>(reinterpret_cast<const u8*>(m_stream.next_out) - m_out.Data());
  }

private:
  void Grow(size_t extra)
  {
    const size_t used = Size();
    m_stream.avail_out = static_cast<unsigned int>(m_out.Grow(used, extra));
    m_stream.next_out = reinterpret_cast<char*>(m_out.At(used));
  }

  bz_stream m_stream{};
  GrowingOutput m_out;
  int m_level;
};

class LzmaEncoder final : public Encoder
{
public:
  LzmaEncoder(bool lzma2, int level, u8* props_out, u8* props_size_out)
  {
    // lzma_lzma_preset returns true on failure.
    if (lzma_lzma_preset(&m_options, static_cast<uint32_t>(level)))
      return;
    m_usable = true;

    if (lzma2)
    {
      if (props_size_out)
        *props_size_out = 1;
      if (props_out)
      {
        // The smallest code whose dictionary is big enough.
        u8 code = 0;
        while (code < 40 && m_options.dict_size > Lzma2DictSize(code))
          ++code;
        props_out[0] = code;
      }
    }
    else
    {
      if (props_size_out)
        *props_size_out = 5;
      if (props_out)
      {
        ASSERT(m_options.lc < 9 && m_options.lp < 5 && m_options.pb < 5);
        props_out[0] = static_cast<u8>((m_options.pb * 5 + m_options.lp) * 9 + m_options.lc);
        std::memcpy(props_out + 1, &m_options.dict_size, sizeof(u32));
      }
    }

    m_filters[0] = {lzma2 ? LZMA_FILTER_LZMA2 : LZMA_FILTER_LZMA1, &m_options};
    m_filters[1] = {LZMA_VLI_UNKNOWN, nullptr};
  }

  ~LzmaEncoder() override { lzma_end(&m_stream); }

  bool Begin(std::optional<u64>) override
  {
    if (!m_usable)
      return false;
    m_out.Reset();
    m_stream.next_out = m_out.At(0);
    m_stream.avail_out = 0;
    return lzma_raw_encoder(&m_stream, m_filters) == LZMA_OK;
  }

  bool Feed(const u8* data, size_t size) override
  {
    m_stream.next_in = data;
    m_stream.avail_in = size;
    Grow(size);
    while (m_stream.avail_in != 0)
    {
      if (m_stream.avail_out == 0)
        Grow(GrowingOutput::STEP);
      if (lzma_code(&m_stream, LZMA_RUN) != LZMA_OK)
        return false;
    }
    return true;
  }

  bool Finish() override
  {
    while (true)
    {
      if (m_stream.avail_out == 0)
        Grow(GrowingOutput::STEP);
      const lzma_ret result = lzma_code(&m_stream, LZMA_FINISH);
      if (result == LZMA_STREAM_END)
        return true;
      if (result != LZMA_OK)
        return false;
    }
  }

  const u8* Data() const override { return m_out.Data(); }
  size_t Size() const override { return static_cast<size_t>(m_stream.next_out - m_out.Data()); }

private:
  void Grow(size_t extra)
  {
    const size_t used = Size();
    m_stream.avail_out = m_out.Grow(used, extra);
    m_stream.next_out = m_out.At(used);
  }

  lzma_stream m_stream = LZMA_STREAM_INIT;
  lzma_options_lzma m_options{};
  lzma_filter m_filters[2]{};
  GrowingOutput m_out;
  bool m_usable = false;
};

class ZstdEncoder final : public Encoder
{
public:
  explicit ZstdEncoder(int level) : m_stream(ZSTD_createCStream())
  {
    // No content size in the frame header: the group entry already records it.
    if (ZSTD_isError(ZSTD_CCtx_setParameter(m_stream, ZSTD_c_compressionLevel, level)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(m_stream, ZSTD_c_contentSizeFlag, 0)))
    {
      ZSTD_freeCStream(m_stream);
      m_stream = nullptr;
    }
  }

  ~ZstdEncoder() override { ZSTD_freeCStream(m_stream); }

  bool Begin(std::optional<u64> input_size) override
  {
    if (!m_stream)
      return false;
    m_out.Reset();
    m_dst = {};
    if (ZSTD_isError(ZSTD_CCtx_reset(m_stream, ZSTD_reset_session_only)))
      return false;
    if (input_size && ZSTD_isError(ZSTD_CCtx_setPledgedSrcSize(m_stream, *input_size)))
      return false;
    return true;
  }

  bool Feed(const u8* data, size_t size) override
  {
    ZSTD_inBuffer src{data, size, 0};
    Grow(size);
    while (src.pos != src.size)
    {
      if (m_dst.pos == m_dst.size)
        Grow(GrowingOutput::STEP);
      if (ZSTD_isError(ZSTD_compressStream(m_stream, &m_dst, &src)))
        return false;
    }
    return true;
  }

  bool Finish() override
  {
    while (true)
    {
      if (m_dst.pos == m_dst.size)
        Grow(GrowingOutput::STEP);
      const size_t left = ZSTD_endStream(m_stream, &m_dst);
      if (ZSTD_isError(left))
        return false;
      if (left == 0)
        return true;
    }
  }

  const u8* Data() const override { return m_out.Data(); }
  size_t Size() const override { return m_dst.pos; }

private:
  void Grow(size_t extra)
  {
    m_dst.size = m_dst.pos + m_out.Grow(m_dst.pos, extra);
    m_dst.dst = m_out.At(0);
  }

  ZSTD_CStream* m_stream;
  ZSTD_outBuffer m_dst{};
  GrowingOutput m_out;
};
}  // namespace

Decoder::~Decoder() = default;
Encoder::~Encoder() = default;

std::unique_ptr<Decoder> MakeDecoder(WIARVZCompressionType type, const u8* compressor_data,
                                     size_t compressor_data_size, u64 purge_output_size)
{
  switch (type)
  {
  case WIARVZCompressionType::None:
    return std::make_unique<CopyDecoder>();
  case WIARVZCompressionType::Purge:
    return std::make_unique<PurgeDecoder>(purge_output_size);
  case WIARVZCompressionType::Bzip2:
    return std::make_unique<Bzip2Decoder>();
  case WIARVZCompressionType::LZMA:
    return std::make_unique<LzmaDecoder>(false, compressor_data, compressor_data_size);
  case WIARVZCompressionType::LZMA2:
    return std::make_unique<LzmaDecoder>(true, compressor_data, compressor_data_size);
  case WIARVZCompressionType::Zstd:
    return std::make_unique<ZstdDecoder>();
  }
  return nullptr;
}

std::unique_ptr<Decoder> MakeRVZUnpacker(std::unique_ptr<Decoder> inner,
                                         std::vector<u8> already_decoded, u64 data_offset,
                                         u32 packed_size)
{
  return std::make_unique<RVZUnpacker>(std::move(inner), std::move(already_decoded), data_offset,
                                       packed_size);
}

std::unique_ptr<Encoder> MakeEncoder(WIARVZCompressionType type, int level,
                                     u8* compressor_data_out, u8* compressor_data_size_out)
{
  switch (type)
  {
  case WIARVZCompressionType::None:
    return nullptr;
  case WIARVZCompressionType::Purge:
    return std::make_unique<PurgeEncoder>();
  case WIARVZCompressionType::Bzip2:
    return std::make_unique<Bzip2Encoder>(level);
  case WIARVZCompressionType::LZMA:
  case WIARVZCompressionType::LZMA2:
    return std::make_unique<LzmaEncoder>(type == WIARVZCompressionType::LZMA2, level,
                                         compressor_data_out, compressor_data_size_out);
  case WIARVZCompressionType::Zstd:
    return std::make_unique<ZstdEncoder>(level);
  }
  return nullptr;
}

}  // namespace DiscIO
