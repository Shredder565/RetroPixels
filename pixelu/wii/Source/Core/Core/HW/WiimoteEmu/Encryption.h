// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

// The register "encryption" of Wii Remote extensions. The Wii writes 16 bytes of key material to
// registers 0x40-0x4f; when 0xaa is written to 0xf0 the extension then scrambles every register
// read with two 8-byte tables derived from that key. The algorithm and its tables were published
// on WiiBrew ("Wiimote/Extension Controllers").

namespace WiimoteEmu
{
using SBox = std::array<u8, 256>;

class EncryptionKey
{
public:
  using RandData = std::array<u8, 10>;
  using KeyData = std::array<u8, 6>;

  // Scramble / unscramble len bytes that were read starting at register addr.
  void Encrypt(u8* data, u32 addr, u32 len) const;
  void Decrypt(u8* data, u32 addr, u32 len) const;

  // Fills ft and sb from the key material and a pair of substitution boxes.
  void GenerateTables(const RandData& rand, const KeyData& key, const SBox& sbox_a,
                      const SBox& sbox_b);

  std::array<u8, 8> ft = {};
  std::array<u8, 8> sb = {};
};

// Original extensions and many third-party ones use different constants.
enum class ExtensionKeyScheme
{
  FirstParty,
  ThirdParty,
};

// Derives the tables an extension uses from what was written to its 0x40-0x4f registers.
EncryptionKey GenerateExtensionKey(ExtensionKeyScheme scheme, const std::array<u8, 16>& key_data);

}  // namespace WiimoteEmu
