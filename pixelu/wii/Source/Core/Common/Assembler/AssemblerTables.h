// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail
{
// Operand shapes the parser accepts after a mnemonic. "OpN" is N comma separated operands,
// "Off1" an operand followed by a parenthesised one ("d(rA)"), and "XOrY" an optional leading
// operand.
enum class ParseAlg
{
  None,
  Op1,
  NoneOrOp1,
  Op1Off1,
  Op2,
  Op1Or2,
  Op3,
  Op2Or3,
  Op4,
  Op5,
  Op1Off1Op2,
};

// What the parser knows about a mnemonic. `mnemonic_index` is an opaque handle that the encoder
// understands; it also selects the record/overflow/link/absolute/prediction variant.
struct ParseInfo
{
  size_t mnemonic_index;
  ParseAlg parse_algorithm;
};

// Looks up an instruction mnemonic, ignoring case. Plain mnemonics are tried first, then the
// simplified ("extended") ones; `*extended` says which table matched.
const ParseInfo* FindMnemonic(std::string_view name, bool* extended);

// Special purpose register names accepted as operands ("lr", "hid2", "dma_u", ...).
std::optional<u32> FindSprName(std::string_view name);

// Directive names, without the leading dot.
const GekkoDirective* FindDirective(std::string_view name);

constexpr size_t MAX_OPERANDS = 5;

// Operand values of one instruction, each tagged with the source columns it came from.
struct OperandList
{
  std::array<Tagged<Interval, u32>, MAX_OPERANDS> list;
  u32 count;
  bool overfill;

  constexpr u32 operator[](size_t index) const { return ValueOf(list[index]); }
  constexpr u32& operator[](size_t index) { return ValueOf(list[index]); }

  // Inserts a value with no source location before position `before`, shifting the rest up.
  void Insert(size_t before, u32 val);

  template <typename It>
  void Copy(It begin, It end)
  {
    count = 0;
    while (begin != end && count < MAX_OPERANDS)
    {
      list[count++] = *begin;
      ++begin;
    }
    overfill = begin != end;
  }
};

// Turns a parsed mnemonic and its operand values into a machine word. On failure the error's
// column and length point at the offending operand; its line fields are left for the caller.
FailureOr<u32> EncodeInstruction(size_t mnemonic_index, bool extended, OperandList operands);
}  // namespace Common::GekkoAssembler::detail
