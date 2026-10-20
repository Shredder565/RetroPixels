// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string_view>
#include <vector>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler
{
// Machine code that belongs at one address.
struct CodeBlock
{
  CodeBlock(u32 address) : block_address(address) {}

  void PushBigEndian(u32 val);

  u32 block_address;
  std::vector<u8> instructions;
};

// Assembles Gekko/Broadway source, including paired-single instructions, the simplified
// mnemonics of IBM's "PowerPC Microprocessor Family: The Programming Environments" and a small
// set of GNU as style directives. The first output starts at `base_virtual_address`; .locate,
// .skip and .align can start further blocks. Empty blocks are left out.
FailureOr<std::vector<CodeBlock>> Assemble(std::string_view assembly, u32 base_virtual_address);
}  // namespace Common::GekkoAssembler
