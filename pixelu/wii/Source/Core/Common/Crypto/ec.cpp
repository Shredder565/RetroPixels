// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Crypto/ec.h"

#include <array>
#include <cstring>

#include "Common/CommonTypes.h"
#include "Common/Crypto/bn.h"
#include "Common/Random.h"

// sect233r1: y^2 + xy = x^3 + x^2 + b over GF(2^233), reduction polynomial
// f(x) = x^233 + x^74 + 1, affine coordinates, the point at infinity stored as (0, 0).
//
// Field elements travel as 30-byte (240-bit) big-endian strings, so an input can carry bits
// 233..239. Those are handled the same way as by the code this replaces, so that results stay
// identical for every possible input, valid or not:
//  * Square() reduces the full 240-bit value.
//  * Mul(a, b) ignores bits >= 233 of both operands, except that the high bits of b are XORed
//    into the result when bit 0 of a is set.
//  * Add() is a plain XOR of all 240 bits.

namespace Common::ec
{
namespace
{
constexpr size_t ELT_BYTES = 30;
constexpr int FIELD_BITS = 233;

struct Fe
{
  // Bit k (coefficient of x^k) is bit k % 64 of w[k / 64].
  std::array<u64, 4> w{};

  static Fe FromBytes(const u8* bytes)
  {
    Fe r;
    for (size_t i = 0; i < ELT_BYTES; ++i)
    {
      const size_t bit = (ELT_BYTES - 1 - i) * 8;
      r.w[bit / 64] |= static_cast<u64>(bytes[i]) << (bit % 64);
    }
    return r;
  }

  void ToBytes(u8* bytes) const
  {
    for (size_t i = 0; i < ELT_BYTES; ++i)
    {
      const size_t bit = (ELT_BYTES - 1 - i) * 8;
      bytes[i] = static_cast<u8>(w[bit / 64] >> (bit % 64));
    }
  }

  bool IsZero() const { return (w[0] | w[1] | w[2] | w[3]) == 0; }
  bool Bit(int k) const { return ((w[k / 64] >> (k % 64)) & 1) != 0; }
  void FlipBit0() { w[0] ^= 1; }
};

constexpr u64 LOW_TOP_MASK = (u64{1} << (FIELD_BITS - 192)) - 1;  // bits 192..232 of w[3]

Fe operator+(const Fe& a, const Fe& b)
{
  Fe r;
  for (size_t i = 0; i < 4; ++i)
    r.w[i] = a.w[i] ^ b.w[i];
  return r;
}

Fe Truncated(const Fe& a)
{
  Fe r = a;
  r.w[3] &= LOW_TOP_MASK;
  return r;
}

Fe HighPart(const Fe& a)
{
  Fe r;
  r.w[3] = a.w[3] & ~LOW_TOP_MASK;
  return r;
}

// Reduces a polynomial of up to 512 bits modulo f, using x^233 = x^74 + 1.
Fe Reduce(std::array<u64, 8> v)
{
  // A word at index i (i >= 4) holds x^(64i + j) = x^(64(i-4) + 23 + j) + x^(64(i-3) + 33 + j).
  for (size_t i = 7; i >= 4; --i)
  {
    const u64 t = v[i];
    v[i] = 0;
    v[i - 4] ^= t << 23;
    v[i - 3] ^= t >> 41;
    v[i - 3] ^= t << 33;
    v[i - 2] ^= t >> 31;
  }
  // Bits 233..255 sit in the top of word 3.
  const u64 t = v[3] >> (FIELD_BITS - 192);
  v[3] &= LOW_TOP_MASK;
  v[0] ^= t;
  v[1] ^= t << (74 - 64);

  Fe r;
  for (size_t i = 0; i < 4; ++i)
    r.w[i] = v[i];
  return r;
}

Fe Square(const Fe& a)
{
  // Squaring in characteristic 2 spreads bit k to bit 2k.
  std::array<u64, 8> v{};
  for (int k = 0; k < 256; ++k)
  {
    if (a.Bit(k))
      v[(2 * k) / 64] |= u64{1} << ((2 * k) % 64);
  }
  return Reduce(v);
}

Fe Mul(const Fe& a, const Fe& b)
{
  const Fe bt = Truncated(b);
  std::array<u64, 8> v{};
  for (int k = 0; k < FIELD_BITS; ++k)
  {
    if (!a.Bit(k))
      continue;
    const int word = k / 64;
    const int shift = k % 64;
    for (int i = 0; i < 4; ++i)
    {
      v[i + word] ^= bt.w[i] << shift;
      if (shift != 0)
        v[i + word + 1] ^= bt.w[i] >> (64 - shift);
    }
  }
  Fe r = Reduce(v);
  if (a.Bit(0))
    r = r + HighPart(b);
  return r;
}

// x^(2^n) * y
Fe SquareTimes(Fe x, int n, const Fe& y)
{
  while (n-- > 0)
    x = Square(x);
  return Mul(x, y);
}

// a^(2^233 - 2) via an Itoh-Tsujii addition chain on 2^k - 1 exponents
// (1, 2, 3, 6, 7, 14, 28, 29, 58, 116, 232), followed by one squaring.
Fe Invert(const Fe& a)
{
  const Fe e2 = SquareTimes(a, 1, a);
  const Fe e3 = SquareTimes(e2, 1, a);
  const Fe e6 = SquareTimes(e3, 3, e3);
  const Fe e7 = SquareTimes(e6, 1, a);
  const Fe e14 = SquareTimes(e7, 7, e7);
  const Fe e28 = SquareTimes(e14, 14, e14);
  const Fe e29 = SquareTimes(e28, 1, a);
  const Fe e58 = SquareTimes(e29, 29, e29);
  const Fe e116 = SquareTimes(e58, 58, e58);
  const Fe e232 = SquareTimes(e116, 116, e116);
  return Square(e232);
}

Fe Divide(const Fe& num, const Fe& den)
{
  return Mul(num, Invert(den));
}

struct Pt
{
  Fe x;
  Fe y;

  static Pt FromBytes(const u8* bytes) { return {Fe::FromBytes(bytes), Fe::FromBytes(bytes + 30)}; }
  void ToBytes(u8* bytes) const
  {
    x.ToBytes(bytes);
    y.ToBytes(bytes + 30);
  }
  bool IsInfinity() const { return x.IsZero() && y.IsZero(); }
};

Pt Double(const Pt& p)
{
  if (p.x.IsZero())
    return {};

  // lambda = x + y/x; x' = lambda^2 + lambda + a (a = 1); y' = x^2 + (lambda + 1) x'
  const Fe lambda = Divide(p.y, p.x) + p.x;
  Pt r;
  r.x = Square(lambda) + lambda;
  r.x.FlipBit0();
  r.y = Mul(lambda, r.x) + r.x + Square(p.x);
  return r;
}

Pt Add(const Pt& p, const Pt& q)
{
  if (p.IsInfinity())
    return q;
  if (q.IsInfinity())
    return p;

  const Fe dx = p.x + q.x;
  const Fe dy = p.y + q.y;
  if (dx.IsZero())
  {
    // Same x: either the same point (double it) or opposite points (sum is infinity).
    if (dy.IsZero())
      return Double(p);
    return {};
  }

  // lambda = (y1 + y2) / (x1 + x2); x3 = lambda^2 + lambda + x1 + x2 + a;
  // y3 = lambda (x1 + x3) + x3 + y1
  const Fe lambda = Divide(dy, dx);
  Fe t = Square(lambda) + lambda + q.x;
  t.FlipBit0();
  Pt r;
  r.x = t + p.x;
  r.y = Mul(lambda, t) + p.y + r.x;
  return r;
}

// k * p for a 30-byte big-endian scalar (all 240 bits are used), double-and-add from the top.
Pt Multiply(const u8* k, const Pt& p)
{
  Pt acc;
  for (size_t i = 0; i < ELT_BYTES; ++i)
  {
    for (int bit = 7; bit >= 0; --bit)
    {
      acc = Double(acc);
      if ((k[i] >> bit) & 1)
        acc = Add(acc, p);
    }
  }
  return acc;
}

// Curve constants from SEC 2 / FIPS 186 (sect233r1).
constexpr u8 ORDER[ELT_BYTES] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0xe9, 0x74, 0xe7, 0x2f,
                                 0x8a, 0x69, 0x22, 0x03, 0x1d, 0x26, 0x03, 0xcf, 0xe0, 0xd7};

constexpr u8 BASE_POINT[ELT_BYTES * 2] = {
    // x
    0x00, 0xfa, 0xc9, 0xdf, 0xcb, 0xac, 0x83, 0x13, 0xbb, 0x21, 0x39, 0xf1, 0xbb, 0x75, 0x5f,
    0xef, 0x65, 0xbc, 0x39, 0x1f, 0x8b, 0x36, 0xf8, 0xf8, 0xeb, 0x73, 0x71, 0xfd, 0x55, 0x8b,
    // y
    0x01, 0x00, 0x6a, 0x08, 0xa4, 0x19, 0x03, 0x35, 0x06, 0x78, 0xe5, 0x85, 0x28, 0xbe, 0xbf,
    0x8a, 0x0b, 0xef, 0xf8, 0x67, 0xa7, 0xca, 0x36, 0x71, 0x6f, 0x7e, 0x01, 0xf8, 0x10, 0x52};

Pt BasePoint()
{
  return Pt::FromBytes(BASE_POINT);
}

// Reduces v (30 bytes) below the group order with a single subtraction.
void ReduceOnce(u8* v)
{
  if (bn_compare(v, ORDER, ELT_BYTES) >= 0)
    bn_sub_modulus(v, ORDER, ELT_BYTES);
}

// The 20-byte hash as a 30-byte number.
std::array<u8, ELT_BYTES> HashToScalar(const u8* hash)
{
  std::array<u8, ELT_BYTES> e{};
  std::memcpy(e.data() + 10, hash, 20);
  return e;
}
}  // namespace

Signature Sign(const u8* key, const u8* hash)
{
  const auto e = HashToScalar(hash);

  // Nonce: 233 random bits, redrawn until it is below the order.
  u8 nonce[ELT_BYTES];
  do
  {
    Common::Random::Generate(nonce, sizeof(nonce));
    nonce[0] &= 1;
  } while (bn_compare(nonce, ORDER, ELT_BYTES) >= 0);

  // r = x(nonce * G) mod n
  u8 r[ELT_BYTES];
  Multiply(nonce, BasePoint()).x.ToBytes(r);
  ReduceOnce(r);

  // s = nonce^-1 * (e + r * key) mod n
  u8 d[ELT_BYTES];
  std::memcpy(d, key, ELT_BYTES);
  ReduceOnce(d);
  u8 rd[ELT_BYTES];
  bn_mul(rd, r, d, ORDER, ELT_BYTES);
  u8 sum[ELT_BYTES];
  bn_add(sum, rd, e.data(), ORDER, ELT_BYTES);
  u8 nonce_inv[ELT_BYTES];
  bn_inv(nonce_inv, nonce, ORDER, ELT_BYTES);
  u8 s[ELT_BYTES];
  bn_mul(s, nonce_inv, sum, ORDER, ELT_BYTES);

  Signature sig;
  std::memcpy(sig.data(), r, ELT_BYTES);
  std::memcpy(sig.data() + ELT_BYTES, s, ELT_BYTES);
  return sig;
}

bool VerifySignature(const u8* public_key, const u8* signature, const u8* hash)
{
  const u8* r = signature;
  const u8* s = signature + ELT_BYTES;
  const auto e = HashToScalar(hash);

  u8 s_inv[ELT_BYTES];
  bn_inv(s_inv, s, ORDER, ELT_BYTES);
  u8 u1[ELT_BYTES];
  u8 u2[ELT_BYTES];
  bn_mul(u1, e.data(), s_inv, ORDER, ELT_BYTES);
  bn_mul(u2, r, s_inv, ORDER, ELT_BYTES);

  // x(u1 * G + u2 * Q) mod n must equal r.
  const Pt sum = Add(Multiply(u1, BasePoint()), Multiply(u2, Pt::FromBytes(public_key)));
  u8 x[ELT_BYTES];
  sum.x.ToBytes(x);
  ReduceOnce(x);
  return bn_compare(x, r, ELT_BYTES) == 0;
}

PublicKey PrivToPub(const u8* key)
{
  PublicKey out;
  Multiply(key, BasePoint()).ToBytes(out.data());
  return out;
}

std::array<u8, 60> ComputeSharedSecret(const u8* private_key, const u8* public_key)
{
  std::array<u8, 60> out;
  Multiply(private_key, Pt::FromBytes(public_key)).ToBytes(out.data());
  return out;
}
}  // namespace Common::ec
