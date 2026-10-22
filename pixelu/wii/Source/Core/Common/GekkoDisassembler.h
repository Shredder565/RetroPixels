// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include "Common/CommonTypes.h"

namespace Common
{
// Turns one 32-bit Gekko (PowerPC 750CL) instruction word into text.
//
// The result is "<mnemonic>\t<operands>". Words that do not decode to a known instruction come
// back as "(ill)\t<hex>", and the all-zero word as "\t---".
class GekkoDisassembler final
{
public:
  GekkoDisassembler() = delete;

  // Decodes `opcode`, fetched from `current_instruction_address`. The address is only used to
  // print the destination of PC-relative branches. When `big_endian` is false the word is
  // byte-swapped first.
  static std::string Disassemble(u32 opcode, u32 current_instruction_address,
                                 bool big_endian = true);

  // Fixed-width register labels used by the debugger's register views (" r1 (sp)", "f10", ...).
  // Indices past 31 give nullptr.
  static const char* GetGPRName(u32 index);
  static const char* GetFPRName(u32 index);
};
}  // namespace Common
