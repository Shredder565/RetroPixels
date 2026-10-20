// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <span>

#include "Common/CommonTypes.h"

namespace Common::HMAC
{
// HMAC-SHA1 (RFC 2104). Writes the 20-byte MAC to out. Returns whether it succeeded.
bool HMACWithSHA1(std::span<const u8> key, std::span<const u8> msg, u8* out);
}  // namespace Common::HMAC
