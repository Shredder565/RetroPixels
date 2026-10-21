// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <span>

#include "Common/CommonTypes.h"

// Checksums and codes stored on Skylanders figures (MIFARE Classic 1K tags), as documented by
// the public figure format write-ups.
namespace IOS::HLE::USB::SkylanderCrypto
{
// CRC-16/CCITT-FALSE (polynomial 0x1021, initial value 0xFFFF).
u16 ComputeCRC16(std::span<const u8> data);
// A 48-bit CRC used to derive the sector keys.
u64 ComputeCRC48(std::span<const u8> data);
// The MIFARE key A of `sector` for a tag with this NUID.
u64 CalculateKeyA(u8 sector, std::span<const u8, 0x4> nuid);

// The per-area checksums. Each reads from `data_start` and writes two bytes to `output`.
void ComputeChecksumType0(const u8* data_start, u8* output);
void ComputeChecksumType1(const u8* data_start, u8* output);
void ComputeChecksumType2(const u8* data_start, u8* output);
void ComputeChecksumType3(const u8* data_start, u8* output);
void ComputeChecksumType6(const u8* data_start, u8* output);

// The "XXXXX-XXXXX" web code printed with a figure ("N/A" and zeros for code 0).
std::array<u8, 11> ComputeToyCode(u64 code);
}  // namespace IOS::HLE::USB::SkylanderCrypto
