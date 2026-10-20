// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Crypto/bn.h"

#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"

// Numbers are big-endian byte strings, so carries and borrows run from the last byte to the
// first.

int bn_compare(const u8* a, const u8* b, size_t n)
{
  return std::memcmp(a, b, n);
}

void bn_sub_modulus(u8* a, const u8* N, size_t n)
{
  int borrow = 0;
  for (size_t i = n; i-- > 0;)
  {
    const int diff = static_cast<int>(a[i]) - static_cast<int>(N[i]) - borrow;
    a[i] = static_cast<u8>(diff);
    borrow = diff < 0 ? 1 : 0;
  }
}

void bn_add(u8* d, const u8* a, const u8* b, const u8* N, size_t n)
{
  unsigned carry = 0;
  for (size_t i = n; i-- > 0;)
  {
    const unsigned sum = static_cast<unsigned>(a[i]) + b[i] + carry;
    d[i] = static_cast<u8>(sum);
    carry = sum >> 8;
  }

  // A carry out of the top byte means the true sum is >= 2^(8n) > N: take N off once (the
  // subtraction wraps back into range). Then reduce once more if still at or above N.
  if (carry != 0)
    bn_sub_modulus(d, N, n);
  if (bn_compare(d, N, n) >= 0)
    bn_sub_modulus(d, N, n);
}

void bn_mul(u8* d, const u8* a, const u8* b, const u8* N, size_t n)
{
  // Left-to-right binary multiplication: for each bit of a, double the accumulator and add b
  // when the bit is set, reducing after every step.
  std::vector<u8> acc(n, 0);
  for (size_t i = 0; i < n; ++i)
  {
    for (int bit = 7; bit >= 0; --bit)
    {
      bn_add(acc.data(), acc.data(), acc.data(), N, n);
      if ((a[i] >> bit) & 1)
        bn_add(acc.data(), acc.data(), b, N, n);
    }
  }
  std::memcpy(d, acc.data(), n);
}

void bn_exp(u8* d, const u8* a, const u8* N, size_t n, const u8* e, size_t en)
{
  // Left-to-right square and multiply.
  std::vector<u8> result(n, 0);
  std::vector<u8> squared(n);
  result[n - 1] = 1;
  for (size_t i = 0; i < en; ++i)
  {
    for (int bit = 7; bit >= 0; --bit)
    {
      bn_mul(squared.data(), result.data(), result.data(), N, n);
      if ((e[i] >> bit) & 1)
        bn_mul(result.data(), squared.data(), a, N, n);
      else
        result = squared;
    }
  }
  std::memcpy(d, result.data(), n);
}

void bn_inv(u8* d, const u8* a, const u8* N, size_t n)
{
  // Fermat: a^(N-2) = a^-1 for prime N.
  std::vector<u8> exponent(N, N + n);
  std::vector<u8> two(n, 0);
  two[n - 1] = 2;
  bn_sub_modulus(exponent.data(), two.data(), n);
  bn_exp(d, a, N, n, exponent.data(), n);
}
