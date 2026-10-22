// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Crypto/SHA1.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include <fmt/format.h>
#include <mbedtls/sha1.h>

#include "Common/Assert.h"
#include "Common/CPUDetect.h"
#include "Common/CommonTypes.h"

#if defined(_M_X86_64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#define PIXELU_SHANI_FN
#else
#define PIXELU_SHANI_FN [[gnu::target("sha,ssse3")]]
#endif

namespace Common::SHA1
{
namespace
{
// Software path: mbedtls.
class MbedContext final : public Context
{
public:
  MbedContext()
  {
    mbedtls_sha1_init(&m_ctx);
    const int rc = mbedtls_sha1_starts_ret(&m_ctx);
    ASSERT(rc == 0);
  }
  ~MbedContext() override { mbedtls_sha1_free(&m_ctx); }

  MbedContext(const MbedContext&) = delete;
  MbedContext& operator=(const MbedContext&) = delete;

  void Update(const u8* msg, size_t len) override
  {
    const int rc = mbedtls_sha1_update_ret(&m_ctx, msg, len);
    ASSERT(rc == 0);
  }

  Digest Finish() override
  {
    Digest out{};
    const int rc = mbedtls_sha1_finish_ret(&m_ctx, out.data());
    ASSERT(rc == 0);
    return out;
  }

  bool HwAccelerated() const override { return false; }

private:
  mbedtls_sha1_context m_ctx;
};

#if defined(_M_X86_64)

// SHA-NI path. The five state words live in two registers the way the SHA instructions want
// them: A..D in one (A in the top dword) and E in the top dword of the other.
class NiContext final : public Context
{
public:
  PIXELU_SHANI_FN NiContext()
  {
    m_abcd = _mm_set_epi32(0x67452301, static_cast<int>(0xefcdab89), static_cast<int>(0x98badcfe),
                           0x10325476);
    m_e = _mm_set_epi32(static_cast<int>(0xc3d2e1f0), 0, 0, 0);
  }

  void Update(const u8* msg, size_t len) override
  {
    if (len == 0)
      return;
    m_total += len;

    if (m_fill != 0)
    {
      const size_t take = std::min(len, BLOCK - m_fill);
      std::memcpy(m_pending + m_fill, msg, take);
      m_fill += take;
      msg += take;
      len -= take;
      if (m_fill < BLOCK)
        return;
      Compress(m_pending, 1);
      m_fill = 0;
    }

    const size_t whole = len / BLOCK;
    if (whole != 0)
    {
      Compress(msg, whole);
      msg += whole * BLOCK;
      len -= whole * BLOCK;
    }

    std::memcpy(m_pending, msg, len);
    m_fill = len;
  }

  Digest Finish() override
  {
    // Padding: 0x80, zeros up to 56 mod 64, then the bit length as a big-endian u64.
    const u64 bits = static_cast<u64>(m_total) * 8;
    u8 tail[BLOCK * 2] = {};
    std::memcpy(tail, m_pending, m_fill);
    tail[m_fill] = 0x80;
    const size_t tail_len = m_fill < BLOCK - 8 ? BLOCK : BLOCK * 2;
    for (int i = 0; i < 8; ++i)
      tail[tail_len - 1 - i] = static_cast<u8>(bits >> (8 * i));
    Compress(tail, tail_len / BLOCK);
    m_fill = 0;
    return Output();
  }

  bool HwAccelerated() const override { return true; }

private:
  static constexpr size_t BLOCK = 64;

  PIXELU_SHANI_FN static __m128i Swap(__m128i v)
  {
    // Reverse all 16 bytes: big-endian words, first word in the top dword.
    const __m128i order = _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    return _mm_shuffle_epi8(v, order);
  }

  // Four rounds per step, 20 steps. w holds the last four message-schedule vectors, indexed
  // modulo 4; step S >= 4 replaces w[S % 4] with W[4S..4S+3] before using it. `prev` is the
  // A..D value from before the previous step, which sha1nexte turns into that step's E.
  template <int S>
  PIXELU_SHANI_FN static void Step(__m128i& abcd, __m128i& prev, __m128i& e0, __m128i (&w)[4])
  {
    constexpr int cur = S % 4;
    if constexpr (S >= 4)
    {
      const __m128i t = _mm_xor_si128(_mm_sha1msg1_epu32(w[cur], w[(S + 1) % 4]), w[(S + 2) % 4]);
      w[cur] = _mm_sha1msg2_epu32(t, w[(S + 3) % 4]);
    }

    const __m128i e = S == 0 ? _mm_add_epi32(e0, w[cur]) : _mm_sha1nexte_epu32(prev, w[cur]);
    prev = abcd;
    abcd = _mm_sha1rnds4_epu32(abcd, e, S / 5);

    if constexpr (S < 19)
      Step<S + 1>(abcd, prev, e0, w);
  }

  PIXELU_SHANI_FN void Compress(const u8* data, size_t blocks)
  {
    __m128i abcd = m_abcd;
    __m128i e = m_e;
    for (size_t n = 0; n < blocks; ++n, data += BLOCK)
    {
      __m128i w[4];
      for (int i = 0; i < 4; ++i)
        w[i] = Swap(_mm_loadu_si128(reinterpret_cast<const __m128i*>(data + 16 * i)));

      const __m128i abcd_in = abcd;
      const __m128i e_in = e;
      __m128i prev = abcd;
      Step<0>(abcd, prev, e, w);
      e = _mm_sha1nexte_epu32(prev, e_in);
      abcd = _mm_add_epi32(abcd, abcd_in);
    }
    m_abcd = abcd;
    m_e = e;
  }

  PIXELU_SHANI_FN Digest Output() const
  {
    Digest out{};
    _mm_storeu_si128(reinterpret_cast<__m128i*>(out.data()), Swap(m_abcd));
    const u32 e_be = static_cast<u32>(_mm_cvtsi128_si32(Swap(m_e)));
    std::memcpy(out.data() + 16, &e_be, sizeof(e_be));
    return out;
  }

  __m128i m_abcd;
  __m128i m_e;
  u8 m_pending[BLOCK] = {};
  size_t m_fill = 0;
  size_t m_total = 0;
};

#endif
}  // namespace

std::unique_ptr<Context> CreateContext()
{
#if defined(_M_X86_64)
  if (cpu_info.bSHA1 && cpu_info.bSSSE3)
    return std::make_unique<NiContext>();
#endif
  return std::make_unique<MbedContext>();
}

Digest CalculateDigest(const u8* msg, size_t len)
{
  const auto context = CreateContext();
  context->Update(msg, len);
  return context->Finish();
}

std::string DigestToString(const Digest& digest)
{
  std::string text;
  text.reserve(digest.size() * 2);
  for (const u8 byte : digest)
    text += fmt::format("{:02X}", byte);
  return text;
}
}  // namespace Common::SHA1
