// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Convenience wrappers around the DSP assembler and disassembler, and helpers
// for DSP code files (big-endian 16-bit words).

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

namespace DSP
{
bool Assemble(const std::string& text, std::vector<u16>& code, bool force = false);
// The listing shows the instruction words, and the addresses if line_numbers is set.
bool Disassemble(std::span<const u16> code, bool line_numbers, std::string& text);
// Prints the differences to stdout. True if the two are identical.
bool Compare(std::span<const u16> code1, std::span<const u16> code2);

// Byte strings holding the code in big-endian order, as stored in files.
std::string CodeToBinaryStringBE(std::span<const u16> code);
std::vector<u16> BinaryStringBEToCode(const std::string& str);

std::optional<std::vector<u16>> LoadBinary(const std::string& filename);
bool SaveBinary(std::span<const u16> code, const std::string& filename);

// Writes DSP_UC_<crc>.bin (the code as given) and a .txt listing to the DSP dump folder.
bool DumpDSPCode(const u8* code_be, size_t size_in_bytes, u32 crc);
}  // namespace DSP
