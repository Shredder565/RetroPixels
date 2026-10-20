// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"

// ECDSA and ECDH on sect233r1 (NIST B-233), the curve the Wii uses for its device and title
// certificates. Field elements and scalars are 30-byte big-endian numbers; a point is X then Y
// (60 bytes); a signature is r then s (60 bytes); hashes are 20-byte SHA-1 digests.

namespace Common::ec
{
using Signature = std::array<u8, 60>;
using PublicKey = std::array<u8, 60>;

// Signs a hash with a 30-byte private key, using a fresh random nonce.
Signature Sign(const u8* key, const u8* hash);

// Checks a 60-byte signature over a hash against a 60-byte public key.
bool VerifySignature(const u8* public_key, const u8* signature, const u8* hash);

// ECDH: the point private_key * public_key (60 bytes).
std::array<u8, 60> ComputeSharedSecret(const u8* private_key, const u8* public_key);

// The public key (60 bytes) belonging to a 30-byte private key.
PublicKey PrivToPub(const u8* key);
}  // namespace Common::ec
