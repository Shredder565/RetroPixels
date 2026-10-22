// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Crypto/AES.h"

#include <cstring>
#include <memory>

#include <mbedtls/aes.h>

#include "Common/Assert.h"
#include "Common/CPUDetect.h"

#if defined(_M_X86_64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <immintrin.h>
#endif
#endif

// GCC and Clang only emit AES-NI instructions in functions that ask for them; MSVC always can.
#if defined(_MSC_VER) && !defined(__clang__)
#define PIXELU_AESNI_FN
#else
#define PIXELU_AESNI_FN [[gnu::target("aes")]]
#endif

namespace Common::AES
{
namespace
{
constexpr size_t ROUNDS = 10;  // AES-128

// Portable fallback built on mbedtls.
class MbedContext final : public Context
{
public:
  MbedContext(const u8* key, Mode mode) : m_mode(mode)
  {
    mbedtls_aes_init(&m_aes);
    const int rc = mode == Mode::Encrypt ? mbedtls_aes_setkey_enc(&m_aes, key, 128) :
                                           mbedtls_aes_setkey_dec(&m_aes, key, 128);
    ASSERT(rc == 0);
  }
  ~MbedContext() override { mbedtls_aes_free(&m_aes); }

  MbedContext(const MbedContext&) = delete;
  MbedContext& operator=(const MbedContext&) = delete;

  bool Crypt(const u8* iv, u8* iv_out, const u8* buf_in, u8* buf_out, size_t len) const override
  {
    if (len % BLOCK_SIZE != 0)
      return false;

    u8 chain[BLOCK_SIZE] = {};
    if (iv != nullptr)
      std::memcpy(chain, iv, BLOCK_SIZE);

    const int op = m_mode == Mode::Encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT;
    // mbedtls takes a non-const context although CBC does not modify it.
    if (mbedtls_aes_crypt_cbc(&m_aes, op, len, chain, buf_in, buf_out) != 0)
      return false;

    if (iv_out != nullptr)
      std::memcpy(iv_out, chain, BLOCK_SIZE);
    return true;
  }

private:
  mutable mbedtls_aes_context m_aes;
  Mode m_mode;
};

#if defined(_M_X86_64)

// AES-NI implementation. Round keys follow the FIPS-197 schedule; for decryption the middle
// keys are passed through InvMixColumns (aesimc) and the order is reversed, which is what the
// "equivalent inverse cipher" used by aesdec expects.
class NiContext final : public Context
{
public:
  PIXELU_AESNI_FN NiContext(const u8* key, Mode mode) : m_mode(mode)
  {
    __m128i k[ROUNDS + 1];
    k[0] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(key));
    k[1] = Expand<0x01>(k[0]);
    k[2] = Expand<0x02>(k[1]);
    k[3] = Expand<0x04>(k[2]);
    k[4] = Expand<0x08>(k[3]);
    k[5] = Expand<0x10>(k[4]);
    k[6] = Expand<0x20>(k[5]);
    k[7] = Expand<0x40>(k[6]);
    k[8] = Expand<0x80>(k[7]);
    k[9] = Expand<0x1b>(k[8]);
    k[10] = Expand<0x36>(k[9]);

    if (mode == Mode::Encrypt)
    {
      for (size_t i = 0; i <= ROUNDS; ++i)
        Store(i, k[i]);
    }
    else
    {
      Store(0, k[ROUNDS]);
      for (size_t i = 1; i < ROUNDS; ++i)
        Store(i, _mm_aesimc_si128(k[ROUNDS - i]));
      Store(ROUNDS, k[0]);
    }
  }

  bool Crypt(const u8* iv, u8* iv_out, const u8* buf_in, u8* buf_out, size_t len) const override
  {
    if (len % BLOCK_SIZE != 0)
      return false;

    const size_t blocks = len / BLOCK_SIZE;
    if (m_mode == Mode::Encrypt)
      EncryptCBC(iv, iv_out, buf_in, buf_out, blocks);
    else
      DecryptCBC(iv, iv_out, buf_in, buf_out, blocks);
    return true;
  }

private:
  // One step of the key schedule: RotWord/SubWord/Rcon come from aeskeygenassist (dword 3),
  // and the running XOR of the previous key's words is built with two byte shifts.
  template <int Rcon>
  PIXELU_AESNI_FN static __m128i Expand(__m128i prev)
  {
    const __m128i assist = _mm_shuffle_epi32(_mm_aeskeygenassist_si128(prev, Rcon), 0xff);
    prev = _mm_xor_si128(prev, _mm_slli_si128(prev, 4));
    prev = _mm_xor_si128(prev, _mm_slli_si128(prev, 8));
    return _mm_xor_si128(prev, assist);
  }

  PIXELU_AESNI_FN void Store(size_t index, __m128i value)
  {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(m_keys + index * BLOCK_SIZE), value);
  }

  PIXELU_AESNI_FN __m128i Key(size_t index) const
  {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(m_keys + index * BLOCK_SIZE));
  }

  PIXELU_AESNI_FN static __m128i LoadIV(const u8* iv)
  {
    return iv != nullptr ? _mm_loadu_si128(reinterpret_cast<const __m128i*>(iv)) :
                           _mm_setzero_si128();
  }

  PIXELU_AESNI_FN void EncryptCBC(const u8* iv, u8* iv_out, const u8* in, u8* out,
                                  size_t blocks) const
  {
    __m128i keys[ROUNDS + 1];
    for (size_t i = 0; i <= ROUNDS; ++i)
      keys[i] = Key(i);

    __m128i chain = LoadIV(iv);
    for (size_t b = 0; b < blocks; ++b)
    {
      __m128i state = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + b * BLOCK_SIZE));
      state = _mm_xor_si128(_mm_xor_si128(state, chain), keys[0]);
      for (size_t r = 1; r < ROUNDS; ++r)
        state = _mm_aesenc_si128(state, keys[r]);
      chain = _mm_aesenclast_si128(state, keys[ROUNDS]);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + b * BLOCK_SIZE), chain);
    }

    if (iv_out != nullptr)
      _mm_storeu_si128(reinterpret_cast<__m128i*>(iv_out), chain);
  }

  // CBC decryption has no dependency between blocks, so groups of blocks are decrypted
  // together to keep the AES unit busy. Every input block of a group is read before any
  // output is written, so in-place use is safe.
  PIXELU_AESNI_FN void DecryptCBC(const u8* iv, u8* iv_out, const u8* in, u8* out,
                                  size_t blocks) const
  {
    constexpr size_t GROUP = 8;

    __m128i keys[ROUNDS + 1];
    for (size_t i = 0; i <= ROUNDS; ++i)
      keys[i] = Key(i);

    __m128i chain = LoadIV(iv);
    size_t b = 0;
    for (; b + GROUP <= blocks; b += GROUP)
    {
      __m128i cipher[GROUP];
      __m128i state[GROUP];
      for (size_t j = 0; j < GROUP; ++j)
      {
        cipher[j] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + (b + j) * BLOCK_SIZE));
        state[j] = _mm_xor_si128(cipher[j], keys[0]);
      }
      for (size_t r = 1; r < ROUNDS; ++r)
      {
        for (size_t j = 0; j < GROUP; ++j)
          state[j] = _mm_aesdec_si128(state[j], keys[r]);
      }
      for (size_t j = 0; j < GROUP; ++j)
      {
        state[j] = _mm_aesdeclast_si128(state[j], keys[ROUNDS]);
        state[j] = _mm_xor_si128(state[j], j == 0 ? chain : cipher[j - 1]);
      }
      for (size_t j = 0; j < GROUP; ++j)
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + (b + j) * BLOCK_SIZE), state[j]);
      chain = cipher[GROUP - 1];
    }

    for (; b < blocks; ++b)
    {
      const __m128i cipher =
          _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + b * BLOCK_SIZE));
      __m128i state = _mm_xor_si128(cipher, keys[0]);
      for (size_t r = 1; r < ROUNDS; ++r)
        state = _mm_aesdec_si128(state, keys[r]);
      state = _mm_xor_si128(_mm_aesdeclast_si128(state, keys[ROUNDS]), chain);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(out + b * BLOCK_SIZE), state);
      chain = cipher;
    }

    if (iv_out != nullptr)
      _mm_storeu_si128(reinterpret_cast<__m128i*>(iv_out), chain);
  }

  u8 m_keys[(ROUNDS + 1) * BLOCK_SIZE];
  Mode m_mode;
};

#endif

std::unique_ptr<Context> MakeContext(const u8* key, Mode mode)
{
#if defined(_M_X86_64)
  // When the compiler is allowed to use AVX, the intrinsics above become VEX-encoded AES
  // instructions, which additionally need an AVX-capable CPU.
#if defined(__AVX__)
  const bool use_ni = cpu_info.bAES && cpu_info.bAVX;
#else
  const bool use_ni = cpu_info.bAES;
#endif
  if (use_ni)
    return std::make_unique<NiContext>(key, mode);
#endif
  return std::make_unique<MbedContext>(key, mode);
}
}  // namespace

std::unique_ptr<Context> CreateContextEncrypt(const u8* key)
{
  return MakeContext(key, Mode::Encrypt);
}

std::unique_ptr<Context> CreateContextDecrypt(const u8* key)
{
  return MakeContext(key, Mode::Decrypt);
}

void CryptOFB(const u8* key, const u8* iv, u8* iv_out, const u8* buf_in, u8* buf_out, size_t size)
{
  u8 stream[16] = {};
  if (iv != nullptr)
    std::memcpy(stream, iv, sizeof(stream));

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  // OFB only ever runs the forward cipher, for both directions.
  const int rc = mbedtls_aes_setkey_enc(&aes, key, 128);
  ASSERT(rc == 0);
  size_t offset = 0;
  mbedtls_aes_crypt_ofb(&aes, size, &offset, stream, buf_in, buf_out);
  mbedtls_aes_free(&aes);

  if (iv_out != nullptr)
    std::memcpy(iv_out, stream, sizeof(stream));
}
}  // namespace Common::AES
