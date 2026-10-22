// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Assembler/GekkoIRGen.h"

#include <bit>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/GekkoLexer.h"
#include "Common/Assembler/GekkoParser.h"
#include "Common/Assert.h"

namespace Common::GekkoAssembler::detail
{
namespace
{
// One step of an instruction operand's expression, kept in postfix order so the operand can be
// evaluated after the whole input (and so every label) is known.
struct Step
{
  enum class Kind : u8
  {
    Value,         // push `value`
    Symbol,        // push a label (relative to `value` unless `absolute`) or a constant
    LocalForward,  // push the distance from `value` to the next local label `label_number`
    Unary,
    Binary,
    Relative,  // pop x, push x - `value`
  };

  Kind kind = Kind::Value;
  AsmOp op = AsmOp::Add;
  u32 value = 0;
  bool absolute = false;
  std::string_view name;
  u32 label_number = 0;
  size_t search_from = 0;
  // What to report if this step cannot be evaluated.
  std::optional<AssemblerError> failure;
};

using Program = std::vector<Step>;

u32 ApplyBinary32(AsmOp op, u32 lhs, u32 rhs)
{
  switch (op)
  {
  case AsmOp::Or:
    return lhs | rhs;
  case AsmOp::Xor:
    return lhs ^ rhs;
  case AsmOp::And:
    return lhs & rhs;
  case AsmOp::Lsh:
    return lhs << (rhs & 31);
  case AsmOp::Rsh:
    return lhs >> (rhs & 31);
  case AsmOp::Add:
    return lhs + rhs;
  case AsmOp::Sub:
    return lhs - rhs;
  case AsmOp::Mul:
    return lhs * rhs;
  case AsmOp::Div:
    return rhs == 0 ? 0 : lhs / rhs;
  default:
    return 0;
  }
}

u64 ApplyBinary64(AsmOp op, u64 lhs, u64 rhs)
{
  switch (op)
  {
  case AsmOp::Or:
    return lhs | rhs;
  case AsmOp::Xor:
    return lhs ^ rhs;
  case AsmOp::And:
    return lhs & rhs;
  case AsmOp::Lsh:
    return lhs << (rhs & 63);
  case AsmOp::Rsh:
    return lhs >> (rhs & 63);
  case AsmOp::Add:
    return lhs + rhs;
  case AsmOp::Sub:
    return lhs - rhs;
  case AsmOp::Mul:
    return lhs * rhs;
  case AsmOp::Div:
    return rhs == 0 ? 0 : lhs / rhs;
  default:
    return 0;
  }
}

constexpr std::string_view kDivideByZero = "Division by zero";

class IrBuilder final : public ParsePlugin
{
public:
  IrBuilder(GekkoIR& out, u32 base_address) : m_ir(out) { m_ir.blocks.emplace_back(base_address); }

  // --- statements ---------------------------------------------------------------------------

  void OnLabelDecl(std::string_view name) override
  {
    if (!ClaimName(name))
      return;
    m_labels.emplace(std::string(name), Here());
  }

  void OnNumericLabelDecl(std::string_view, u32 num) override
  {
    m_local_labels.emplace_back(num, Here());
  }

  void OnVarDecl(std::string_view name) override
  {
    if (!ClaimName(name))
      return;
    // The constant exists (as zero) while its own value is being evaluated.
    m_constants[std::string(name)] = 0;
    m_pending_var = std::string(name);
  }

  void OnInstructionPre(const ParseInfo& info, bool extended) override
  {
    m_in_instruction = true;
    m_instr = IrInstruction{info.mnemonic_index, extended, Interval{m_ir.operand_pool.size(), 0},
                            m_owner->lexer.CurrentLine(), m_owner->lexer.LineNumber()};
  }

  void OnInstructionPost(const ParseInfo&, bool) override
  {
    m_instr.operands.len = m_ir.operand_pool.size() - m_instr.operands.begin;
    Block().items.emplace_back(m_instr);
    Block().size += 4;
  }

  void OnOperandPre() override
  {
    m_operand_col = m_owner->lexer.ColNumber();
    m_program.clear();
  }

  void OnOperandPost() override
  {
    const size_t end_col = m_owner->lexer.ColNumber();
    m_ir.operand_pool.emplace_back(Interval{m_operand_col, end_col - m_operand_col}, 0);
    m_operand_programs.push_back(std::move(m_program));
    m_program = {};
  }

  void OnDirectivePre(GekkoDirective directive) override
  {
    m_in_instruction = false;
    m_directive = directive;
    m_values.clear();
    m_floats.clear();
  }

  void OnResolvedExprPost() override
  {
    switch (m_directive)
    {
    case GekkoDirective::Byte:
      AppendBigEndian(m_values.back(), 1);
      break;
    case GekkoDirective::_2byte:
      AppendBigEndian(m_values.back(), 2);
      break;
    case GekkoDirective::_4byte:
      AppendBigEndian(m_values.back(), 4);
      break;
    case GekkoDirective::_8byte:
      AppendBigEndian(m_values.back(), 8);
      break;
    default:
      return;
    }
    m_values.clear();
  }

  void OnDirectivePost(GekkoDirective directive) override
  {
    switch (directive)
    {
    case GekkoDirective::Float:
      for (const double v : m_floats)
        AppendBigEndian(std::bit_cast<u32>(static_cast<float>(v)), 4);
      break;
    case GekkoDirective::Double:
      for (const double v : m_floats)
        AppendBigEndian(std::bit_cast<u64>(v), 8);
      break;
    case GekkoDirective::DefVar:
      m_constants[m_pending_var] = m_values.back();
      m_pending_var.clear();
      break;
    case GekkoDirective::Locate:
      NewBlock(static_cast<u32>(m_values.back()));
      break;
    case GekkoDirective::Zeros:
      Pad(static_cast<u32>(m_values.back()));
      break;
    case GekkoDirective::Skip:
      if (const u32 len = static_cast<u32>(m_values.back()); len != 0)
        NewBlock(Here() + len);
      break;
    case GekkoDirective::PadAlign:
    {
      const u32 pad = AlignmentGap(static_cast<u32>(m_values.back()));
      if (pad != 0)
        Pad(pad);
      break;
    }
    case GekkoDirective::Align:
    {
      const u32 pad = AlignmentGap(static_cast<u32>(m_values.back()));
      if (pad != 0)
        NewBlock(Here() + pad);
      break;
    }
    case GekkoDirective::Ascii:
    case GekkoDirective::Asciz:
    {
      IrBytes bytes;
      ConvertStringLiteral(m_string, &bytes);
      if (directive == GekkoDirective::Asciz)
        bytes.push_back(0);
      AppendBytes(bytes);
      break;
    }
    default:
      break;
    }
    m_values.clear();
  }

  // --- expressions --------------------------------------------------------------------------

  void OnTerminal(Terminal type, const AssemblerToken& tok) override
  {
    if (type == Terminal::Str)
    {
      m_string = tok.token_val;
      return;
    }
    if (m_in_instruction)
      DeferredTerminal(type, tok);
    else
      ImmediateTerminal(type, tok);
  }

  void OnHiaddr(std::string_view id) override { AddressHalf(id, true); }
  void OnLoaddr(std::string_view id) override { AddressHalf(id, false); }

  void OnCloseParen(ParenType type) override
  {
    if (type != ParenType::RelConv)
      return;
    if (m_in_instruction)
    {
      Step s;
      s.kind = Step::Kind::Relative;
      s.value = Here();
      m_program.push_back(std::move(s));
    }
    else
    {
      m_values.back() -= Here();
    }
  }

  void OnOperator(AsmOp op) override
  {
    const bool unary = op == AsmOp::Neg || op == AsmOp::Not;
    if (m_in_instruction)
    {
      Step s;
      s.kind = unary ? Step::Kind::Unary : Step::Kind::Binary;
      s.op = op;
      if (op == AsmOp::Div)
        s.failure = ErrorAtNextToken(std::string(kDivideByZero));
      m_program.push_back(std::move(s));
      return;
    }

    if (unary)
    {
      u64& v = m_values.back();
      v = op == AsmOp::Neg ? u64{0u - static_cast<u32>(v)} : ~v;
      return;
    }
    const u64 rhs = m_values.back();
    m_values.pop_back();
    if (op == AsmOp::Div && rhs == 0)
    {
      m_owner->EmitErrorHere(std::string(kDivideByZero));
      return;
    }
    m_values.back() = ApplyBinary64(op, m_values.back(), rhs);
  }

  // Evaluates every instruction operand now that all labels are known.
  void PostParseAction() override
  {
    for (size_t i = 0; i < m_operand_programs.size(); i++)
    {
      std::optional<AssemblerError> failure;
      const u32 value = Evaluate(m_operand_programs[i], &failure);
      if (failure)
      {
        m_owner->error = std::move(failure);
        return;
      }
      ValueOf(m_ir.operand_pool[i]) = value;
    }
  }

private:
  IrBlock& Block() { return m_ir.blocks.back(); }
  u32 Here() { return Block().EndAddress(); }

  void NewBlock(u32 address) { m_ir.blocks.emplace_back(address); }

  // Bytes needed to bring the current address to a multiple of 2^bits.
  u32 AlignmentGap(u32 bits)
  {
    const u32 unit = 1u << (bits & 31);
    const u32 misalignment = Here() & (unit - 1);
    return misalignment == 0 ? 0 : unit - misalignment;
  }

  void AppendBytes(const IrBytes& bytes)
  {
    IrBlock& b = Block();
    if (b.items.empty() || !std::holds_alternative<IrBytes>(b.items.back()))
      b.items.emplace_back(IrBytes{});
    IrBytes& dst = std::get<IrBytes>(b.items.back());
    dst.insert(dst.end(), bytes.begin(), bytes.end());
    b.size += static_cast<u32>(bytes.size());
  }

  void AppendBigEndian(u64 value, size_t width)
  {
    IrBytes bytes(width);
    for (size_t i = 0; i < width; i++)
      bytes[i] = static_cast<u8>(value >> (8 * (width - 1 - i)));
    AppendBytes(bytes);
  }

  void Pad(u32 count)
  {
    IrBlock& b = Block();
    if (b.items.empty() || !std::holds_alternative<IrPadding>(b.items.back()))
      b.items.emplace_back(IrPadding{0});
    std::get<IrPadding>(b.items.back()) += count;
    b.size += count;
  }

  bool ClaimName(std::string_view name)
  {
    if (m_names.insert(std::string(name)).second)
      return true;
    m_owner->EmitErrorHere(fmt::format("Label/Constant {} is already defined", name));
    return false;
  }

  std::optional<u32> FindLabel(std::string_view name) const
  {
    const auto it = m_labels.find(name);
    return it == m_labels.end() ? std::nullopt : std::optional(it->second);
  }

  std::optional<u64> FindConstant(std::string_view name) const
  {
    const auto it = m_constants.find(name);
    return it == m_constants.end() ? std::nullopt : std::optional(it->second);
  }

  // Address of the closest local label `num` defined so far.
  std::optional<u32> FindLocalBackward(u32 num) const
  {
    for (auto it = m_local_labels.rbegin(); it != m_local_labels.rend(); ++it)
    {
      if (it->first == num)
        return it->second;
    }
    return std::nullopt;
  }

  // An error located at the parser's next token, `len` columns wide (default: the token).
  AssemblerError ErrorAtNextToken(std::string message, std::optional<size_t> len = std::nullopt)
  {
    const Lexer& lex = m_owner->lexer;
    const size_t width = len ? *len : lex.LookaheadRef().token_val.size();
    return AssemblerError{std::move(message), lex.CurrentLine(), lex.LineNumber(),
                          lex.ColNumber(), width};
  }

  void PushValue(u32 v)
  {
    Step s;
    s.value = v;
    m_program.push_back(std::move(s));
  }

  void PushSymbol(std::string_view name, bool absolute)
  {
    Step s;
    s.kind = Step::Kind::Symbol;
    s.name = name;
    s.absolute = absolute;
    s.value = Here();
    s.failure = ErrorAtNextToken(fmt::format("Unresolved symbol '{}'", name), name.size());
    m_program.push_back(std::move(s));
  }

  void PushBinary(AsmOp op)
  {
    Step s;
    s.kind = Step::Kind::Binary;
    s.op = op;
    m_program.push_back(std::move(s));
  }

  // Operand of an instruction: labels are relative to the instruction unless resolved through
  // @ha/@l, and anything defined later is looked up at the end.
  void DeferredTerminal(Terminal type, const AssemblerToken& tok)
  {
    switch (type)
    {
    case Terminal::Dot:
      PushValue(Here());
      break;

    case Terminal::Id:
      if (const auto label = FindLabel(tok.token_val))
        PushValue(*label - Here());
      else if (const auto constant = FindConstant(tok.token_val))
        PushValue(static_cast<u32>(*constant));
      else
        PushSymbol(tok.token_val, false);
      break;

    case Terminal::NumLabFwd:
    {
      const u32 num = tok.EvalToken<u32>().value_or(0);
      Step s;
      s.kind = Step::Kind::LocalForward;
      s.label_number = num;
      s.search_from = m_local_labels.size();
      s.value = Here();
      s.failure = ErrorAtNextToken(fmt::format("No numeric label '{}' found below here", num),
                                   tok.token_val.size());
      m_program.push_back(std::move(s));
      break;
    }

    case Terminal::NumLabBwd:
    {
      const u32 num = tok.EvalToken<u32>().value_or(0);
      if (const auto addr = FindLocalBackward(num))
        PushValue(*addr - Here());
      else
        m_owner->EmitErrorHere(fmt::format("No numeric label '{}' found above here", num));
      break;
    }

    default:
    {
      const std::optional<u32> v = tok.EvalToken<u32>();
      ASSERT(v.has_value());
      PushValue(v.value_or(0));
      break;
    }
    }
  }

  // Directive argument: everything must be known already, and labels are absolute.
  void ImmediateTerminal(Terminal type, const AssemblerToken& tok)
  {
    switch (type)
    {
    case Terminal::Flt:
      if (m_directive == GekkoDirective::Float)
        m_floats.push_back(tok.EvalToken<float>().value_or(0.0f));
      else
        m_floats.push_back(tok.EvalToken<double>().value_or(0.0));
      break;

    case Terminal::Dot:
      m_values.push_back(Here());
      break;

    case Terminal::Id:
      if (const auto label = FindLabel(tok.token_val))
        m_values.push_back(*label);
      else if (const auto constant = FindConstant(tok.token_val))
        m_values.push_back(*constant);
      else
        m_owner->EmitErrorHere(
            fmt::format("Undefined reference to Label/Constant '{}'", tok.ValStr()));
      break;

    case Terminal::NumLabFwd:
      m_owner->EmitErrorHere(
          "Forward label references not supported in fully resolved expressions");
      break;

    case Terminal::NumLabBwd:
    {
      const u32 num = tok.EvalToken<u32>().value_or(0);
      if (const auto addr = FindLocalBackward(num))
        m_values.push_back(*addr);
      else
        m_owner->EmitErrorHere(fmt::format("No numeric label '{}' found above here", num));
      break;
    }

    default:
    {
      const std::optional<u64> v = tok.EvalToken<u64>();
      ASSERT(v.has_value());
      m_values.push_back(v.value_or(0));
      break;
    }
    }
  }

  void AddressHalf(std::string_view id, bool high)
  {
    if (m_in_instruction)
    {
      PushSymbol(id, true);
      if (high)
      {
        PushValue(16);
        PushBinary(AsmOp::Rsh);
      }
      PushValue(0xffff);
      PushBinary(AsmOp::And);
      return;
    }

    std::optional<u32> base = FindLabel(id);
    if (!base)
    {
      if (const auto constant = FindConstant(id))
        base = static_cast<u32>(*constant);
    }
    if (!base)
    {
      m_owner->EmitErrorHere(fmt::format("Undefined reference to Label/Constant '{}'", id));
      return;
    }
    m_values.push_back(high ? (*base >> 16) & 0xffff : *base & 0xffff);
  }

  // Runs an operand program. Reports the first step that fails, in source order.
  u32 Evaluate(const Program& program, std::optional<AssemblerError>* failure) const
  {
    const auto fail = [failure](const Step& s) {
      if (!*failure && s.failure)
        *failure = *s.failure;
    };

    std::vector<u32> stack;
    for (const Step& s : program)
    {
      switch (s.kind)
      {
      case Step::Kind::Value:
        stack.push_back(s.value);
        break;

      case Step::Kind::Symbol:
        if (const auto label = FindLabel(s.name))
          stack.push_back(s.absolute ? *label : *label - s.value);
        else if (const auto constant = FindConstant(s.name))
          stack.push_back(static_cast<u32>(*constant));
        else
        {
          fail(s);
          stack.push_back(0);
        }
        break;

      case Step::Kind::LocalForward:
      {
        std::optional<u32> found;
        for (size_t i = s.search_from; i < m_local_labels.size() && !found; i++)
        {
          if (m_local_labels[i].first == s.label_number)
            found = m_local_labels[i].second - s.value;
        }
        if (!found)
          fail(s);
        stack.push_back(found.value_or(0));
        break;
      }

      case Step::Kind::Unary:
        stack.back() = s.op == AsmOp::Neg ? 0u - stack.back() : ~stack.back();
        break;

      case Step::Kind::Binary:
      {
        const u32 rhs = stack.back();
        stack.pop_back();
        if (s.op == AsmOp::Div && rhs == 0)
          fail(s);
        stack.back() = ApplyBinary32(s.op, stack.back(), rhs);
        break;
      }

      case Step::Kind::Relative:
        stack.back() -= s.value;
        break;
      }
    }
    return stack.empty() ? 0 : stack.back();
  }

  GekkoIR& m_ir;

  // Symbols
  std::set<std::string> m_names;
  std::map<std::string, u32, std::less<>> m_labels;
  std::map<std::string, u64, std::less<>> m_constants;
  // Local (numeric) labels in definition order: (number, address).
  std::vector<std::pair<u32, u32>> m_local_labels;

  // Instructions
  bool m_in_instruction = false;
  IrInstruction m_instr;
  size_t m_operand_col = 0;
  Program m_program;
  std::vector<Program> m_operand_programs;

  // Directives
  GekkoDirective m_directive = GekkoDirective::Byte;
  std::vector<u64> m_values;
  std::vector<double> m_floats;
  std::string_view m_string;
  std::string m_pending_var;
};
}  // namespace

FailureOr<GekkoIR> ParseToIR(std::string_view assembly, u32 base_virtual_address)
{
  GekkoIR ir;
  IrBuilder builder(ir, base_virtual_address);
  ParseWithPlugin(&builder, assembly);
  if (builder.Error())
    return std::move(*builder.Error());
  return ir;
}
}  // namespace Common::GekkoAssembler::detail
