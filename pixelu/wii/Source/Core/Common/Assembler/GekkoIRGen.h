// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string_view>
#include <variant>
#include <vector>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail
{
// An instruction whose operands have been evaluated but not yet range checked or encoded.
struct IrInstruction
{
  size_t mnemonic_index = 0;
  bool extended = false;
  // Slice of GekkoIR::operand_pool.
  Interval operands{0, 0};
  std::string_view source_line;
  size_t line = 0;
};

using IrBytes = std::vector<u8>;
// A run of zero bytes.
using IrPadding = size_t;
using IrItem = std::variant<IrInstruction, IrBytes, IrPadding>;

// Output placed contiguously from `address`.
struct IrBlock
{
  explicit IrBlock(u32 start) : address(start) {}

  u32 EndAddress() const { return address + size; }

  u32 address;
  u32 size = 0;
  std::vector<IrItem> items;
};

struct GekkoIR
{
  std::vector<IrBlock> blocks;
  // Every instruction operand in source order, tagged with its source columns.
  std::vector<Tagged<Interval, u32>> operand_pool;
};

// Parses `assembly` and resolves every symbol, placing the first instruction at
// `base_virtual_address`.
FailureOr<GekkoIR> ParseToIR(std::string_view assembly, u32 base_virtual_address);
}  // namespace Common::GekkoAssembler::detail
