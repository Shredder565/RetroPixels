// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Assembler/GekkoAssembler.h"

#include <utility>
#include <variant>
#include <vector>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/AssemblerTables.h"
#include "Common/Assembler/GekkoIRGen.h"
#include "Common/Assert.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler
{
void CodeBlock::PushBigEndian(u32 val)
{
  for (int shift = 24; shift >= 0; shift -= 8)
    instructions.push_back(static_cast<u8>(val >> shift));
}

FailureOr<std::vector<CodeBlock>> Assemble(std::string_view assembly, u32 base_virtual_address)
{
  FailureOr<detail::GekkoIR> parsed = detail::ParseToIR(assembly, base_virtual_address);
  if (IsFailure(parsed))
    return std::move(GetFailure(parsed));

  const detail::GekkoIR& ir = GetT(parsed);
  std::vector<CodeBlock> out;

  for (const detail::IrBlock& block : ir.blocks)
  {
    CodeBlock code(block.address);
    for (const detail::IrItem& item : block.items)
    {
      if (const auto* instr = std::get_if<detail::IrInstruction>(&item))
      {
        ASSERT(instr->operands.len <= detail::MAX_OPERANDS);
        detail::OperandList ops;
        const auto first = ir.operand_pool.begin() + instr->operands.begin;
        ops.Copy(first, first + instr->operands.len);

        FailureOr<u32> word =
            detail::EncodeInstruction(instr->mnemonic_index, instr->extended, ops);
        if (IsFailure(word))
        {
          AssemblerError& err = GetFailure(word);
          err.error_line = instr->source_line;
          err.line = instr->line;
          return std::move(err);
        }
        code.PushBigEndian(GetT(word));
      }
      else if (const auto* bytes = std::get_if<detail::IrBytes>(&item))
      {
        code.instructions.insert(code.instructions.end(), bytes->begin(), bytes->end());
      }
      else
      {
        code.instructions.insert(code.instructions.end(), std::get<detail::IrPadding>(item), 0);
      }
    }

    if (!code.instructions.empty())
      out.push_back(std::move(code));
  }
  return out;
}
}  // namespace Common::GekkoAssembler
