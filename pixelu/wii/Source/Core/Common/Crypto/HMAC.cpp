// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Crypto/HMAC.h"

#include <algorithm>
#include <array>

#include "Common/Crypto/SHA1.h"

namespace Common::HMAC
{
bool HMACWithSHA1(std::span<const u8> key, std::span<const u8> msg, u8* out)
{
  constexpr size_t BLOCK = 64;  // SHA-1 block size

  // Keys longer than a block are hashed first; shorter ones are zero padded.
  std::array<u8, BLOCK> padded_key{};
  if (key.size() > BLOCK)
  {
    const SHA1::Digest hashed = SHA1::CalculateDigest(key.data(), key.size());
    std::ranges::copy(hashed, padded_key.begin());
  }
  else
  {
    std::ranges::copy(key, padded_key.begin());
  }

  std::array<u8, BLOCK> pad;

  std::ranges::transform(padded_key, pad.begin(), [](u8 b) { return static_cast<u8>(b ^ 0x36); });
  const auto inner = SHA1::CreateContext();
  inner->Update(pad);
  inner->Update(msg);
  const SHA1::Digest inner_digest = inner->Finish();

  std::ranges::transform(padded_key, pad.begin(), [](u8 b) { return static_cast<u8>(b ^ 0x5c); });
  const auto outer = SHA1::CreateContext();
  outer->Update(pad);
  outer->Update(inner_digest);
  const SHA1::Digest mac = outer->Finish();

  std::ranges::copy(mac, out);
  return true;
}
}  // namespace Common::HMAC
