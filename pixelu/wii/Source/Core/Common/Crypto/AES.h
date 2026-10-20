// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <memory>

#include "Common/CommonTypes.h"

// AES with a 128-bit key. Block ciphers in this emulator only ever need CBC (the Wii's disc,
// NAND and IOS crypto) and OFB (WiiConnect24 downloads).

namespace Common::AES
{
enum class Mode
{
  Decrypt,
  Encrypt,
};

// A key schedule for one direction. Crypt() runs CBC over whole 16-byte blocks; a length
// that is not a multiple of BLOCK_SIZE is refused (returns false, nothing written).
class Context
{
public:
  static constexpr size_t KEY_SIZE = 16;
  static constexpr size_t BLOCK_SIZE = 16;

  Context() = default;
  virtual ~Context() = default;

  // iv: the starting IV (nullptr means all zero). iv_out (optional) receives the IV that
  // continues the chain. buf_in and buf_out may be the same buffer.
  virtual bool Crypt(const u8* iv, u8* iv_out, const u8* buf_in, u8* buf_out,
                     size_t len) const = 0;

  bool Crypt(const u8* iv, const u8* buf_in, u8* buf_out, size_t len) const
  {
    return Crypt(iv, nullptr, buf_in, buf_out, len);
  }

  bool CryptIvZero(const u8* buf_in, u8* buf_out, size_t len) const
  {
    return Crypt(nullptr, nullptr, buf_in, buf_out, len);
  }
};

std::unique_ptr<Context> CreateContextEncrypt(const u8* key);
std::unique_ptr<Context> CreateContextDecrypt(const u8* key);

// AES-128-OFB (encryption and decryption are the same operation). Used for WiiConnect24.
// Any length is allowed; iv_out (optional) receives the final keystream block.
void CryptOFB(const u8* key, const u8* iv, u8* iv_out, const u8* buf_in, u8* buf_out, size_t size);
}  // namespace Common::AES
