// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/DSPDisassembler.h"

#include <limits>
#include <string>
#include <utility>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"

#include "Core/DSP/DSPTables.h"

namespace DSP
{
namespace
{
// The operand's bits, moved down (or, for a negative shift, up) into place.
u32 ExtractField(const param2_t& field, u16 word)
{
  const u32 bits = word & field.mask;
  if (field.lshift < 0)
    return bits << -field.lshift;
  return bits >> field.lshift;
}

// Which extension, if any, rides along in the low bits of word. In the 0x3xxx
// group only seven bits are free for it; an all-zero field means none.
const DSPOPCTemplate* FindExtension(const DSPOPCTemplate& main_op, u16 word)
{
  const int group = main_op.opcode >> 12;
  if (group == 0x3)
    return (word & 0x007f) != 0 ? FindExtOpInfoByOpcode(word & 0x007f) : nullptr;
  if (group > 0x3)
    return (word & 0x00ff) != 0 ? FindExtOpInfoByOpcode(word) : nullptr;
  return nullptr;
}
}  // namespace

DSPDisassembler::DSPDisassembler(const AssemblerSettings& settings) : settings_(settings)
{
}

bool DSPDisassembler::Disassemble(std::span<const u16> code, std::string& text)
{
  if (code.size() > std::numeric_limits<u16>::max())
  {
    text.append("; code too large for 16-bit addressing\n");
    return false;
  }

  u16 pc = 0;
  while (pc < code.size())
  {
    const bool ok = DisassembleOpcode(code, &pc, text);
    text.append("\n");
    if (!ok)
      return false;
  }
  return true;
}

std::string DSPDisassembler::FormatOperands(const DSPOPCTemplate& op, u16 word1, u16 word2) const
{
  std::string out;
  for (int i = 0; i < op.param_count; ++i)
  {
    const param2_t& field = op.params[i];
    if (i != 0)
      out += ", ";

    u32 value = ExtractField(field, field.loc >= 1 ? word2 : word1);
    u32 kind = field.type;

    // The multiplier forms that may name either half of an $ax register print
    // like plain register operands.
    if ((kind & 0xff) == 0x10)
      kind &= 0xff00;

    // Register operands: the field is an offset from the base register encoded in
    // the kind; for the _D forms it selects the accumulator *not* named.
    if ((kind & P_REG) != 0)
    {
      const u32 base = (kind & P_REGS_MASK) >> 8;
      if (kind == P_ACC_D || kind == P_ACCM_D)
        value = base | (~value & 1);
      else
        value |= base;
      kind &= ~static_cast<u32>(P_REGS_MASK);
    }

    switch (kind)
    {
    case P_REG:
      out += settings_.decode_registers ? fmt::format("${}", pdregname(static_cast<int>(value))) :
                                          fmt::format("${}", value);
      break;

    case P_PRG:
      out += settings_.decode_registers ? fmt::format("@${}", pdregname(static_cast<int>(value))) :
                                          fmt::format("@${}", value);
      break;

    case P_VAL:
    case P_ADDR_I:
    case P_ADDR_D:
      out += settings_.decode_names ? pdname(static_cast<u16>(value)) :
                                      fmt::format("0x{:04x}", value);
      break;

    case P_IMM:
      if (field.size == 2)
      {
        out += fmt::format("#0x{:04x}", value);
      }
      else if (field.mask == 0x003f)
      {
        // Shift counts are 6-bit signed numbers.
        const int count = (value & 0x20) != 0 ? static_cast<int>(value) - 64 : static_cast<int>(value);
        out += fmt::format("#{}", count);
      }
      else
      {
        out += fmt::format("#0x{:02x}", value);
      }
      break;

    case P_MEM:
      // A one-word instruction holds a short address: a sign-extended byte.
      if (field.size != 2)
        value = static_cast<u16>(static_cast<s8>(value & 0xff));
      out += settings_.decode_names ? fmt::format("@{}", pdname(static_cast<u16>(value))) :
                                      fmt::format("@0x{:04x}", value);
      break;

    default:
      ERROR_LOG_FMT(DSPLLE, "Unknown parameter type: {:x}", std::to_underlying(field.type));
      break;
    }
  }
  return out;
}

bool DSPDisassembler::DisassembleOpcode(std::span<const u16> code, u16* pc, std::string& dest)
{
  return DisassembleOpcode(code.data(), code.size(), pc, dest);
}

bool DSPDisassembler::DisassembleOpcode(const u16* binbuf, size_t binbuf_size, u16* pc,
                                        std::string& dest)
{
  const u16 index = *pc & 0x7fff;
  if (index >= binbuf_size)
  {
    // *pc is left alone.
    dest.append("; outside memory");
    return false;
  }

  const u16 word1 = binbuf[index];
  const DSPOPCTemplate* op = FindOpInfoByOpcode(word1);
  if (op == nullptr)
    op = &cw;
  const DSPOPCTemplate* ext = FindExtension(*op, word1);

  if (settings_.show_pc)
    dest += fmt::format("{:04x} ", index);

  u16 word2 = 0;
  if (op->size == 2)
  {
    if (index + 1u >= binbuf_size)
    {
      if (settings_.show_hex)
        dest += fmt::format("{:04x} ???? ", word1);
      dest += "; Insufficient data for large immediate";
      *pc = static_cast<u16>(*pc + op->size);
      return false;
    }
    word2 = binbuf[index + 1];
    if (settings_.show_hex)
      dest += fmt::format("{:04x} {:04x} ", word1, word2);
  }
  else if (settings_.show_hex)
  {
    dest += fmt::format("{:04x}      ", word1);
  }

  std::string mnemonic = op->name;
  if (ext != nullptr)
  {
    mnemonic += settings_.ext_separator;
    mnemonic += ext->name;
  }
  if (settings_.lower_case_ops)
    Common::ToLower(&mnemonic);

  if (settings_.print_tabs)
    dest += mnemonic + '\t';
  else
    dest += fmt::format("{:<12}", mnemonic);

  if (op->param_count > 0)
    dest += FormatOperands(*op, word1, word2);

  if (ext != nullptr)
  {
    dest += op->param_count > 0 ? " : " : ": ";
    if (ext->param_count > 0)
      dest += FormatOperands(*ext, word1, word2);
  }

  if (op->opcode_mask == 0)
    dest += "\t\t; *** UNKNOWN OPCODE ***";

  // An instruction with an extension is one word long.
  *pc = static_cast<u16>(*pc + (ext != nullptr ? ext->size : op->size));
  return true;
}
}  // namespace DSP
