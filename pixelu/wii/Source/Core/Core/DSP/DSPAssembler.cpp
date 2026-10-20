// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/DSP/DSPAssembler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/FileUtil.h"

#include "Core/DSP/DSPCore.h"
#include "Core/DSP/DSPDisassembler.h"
#include "Core/DSP/DSPTables.h"

namespace DSP
{
namespace
{
// Indexed by AssemblerError.
constexpr std::array<const char*, 24> ERROR_TEXT = {
    "",
    "Unknown Error",
    "Unknown opcode",
    "Not enough parameters",
    "Too many parameters",
    "Wrong parameter",
    "Expected parameter of type 'string'",
    "Expected parameter of type 'value'",
    "Expected parameter of type 'register'",
    "Expected parameter of type 'memory pointer'",
    "Expected parameter of type 'immediate'",
    "Incorrect binary value",
    "Incorrect hexadecimal value",
    "Incorrect decimal value",
    "Label already exists",
    "Label not defined",
    "No matching brackets",
    "This opcode cannot be extended",
    "Given extending params for non extensible opcode",
    "Wrong parameter: must be accumulator register",
    "Wrong parameter: must be mid accumulator register",
    "Invalid register",
    "Number out of range",
    "Program counter out of range",
};

// Splits a mutable C string into tokens the way the C library's strtok does: runs of
// delimiters are skipped, the delimiter ending a token is overwritten with a NUL, and
// an empty delimiter set yields the whole remainder. Restarting with a new string
// abandons the old one.
class Tokenizer
{
public:
  char* Next(char* start, std::string_view delimiters)
  {
    char* p = start != nullptr ? start : m_rest;
    if (p == nullptr)
      return nullptr;

    while (*p != '\0' && IsDelimiter(*p, delimiters))
      ++p;
    if (*p == '\0')
    {
      m_rest = p;
      return nullptr;
    }

    char* const token = p;
    while (*p != '\0' && !IsDelimiter(*p, delimiters))
      ++p;
    if (*p != '\0')
      *p++ = '\0';
    m_rest = p;
    return token;
  }

private:
  static bool IsDelimiter(char c, std::string_view delimiters)
  {
    return delimiters.find(c) != std::string_view::npos;
  }

  char* m_rest = nullptr;
};

char* SkipSpaces(char* text)
{
  while (*text == ' ')
    ++text;
  return text;
}

bool IsDecimalDigit(char c)
{
  return c >= '0' && c <= '9';
}

bool IsUpperOrUnderscore(char c)
{
  return (c >= 'A' && c <= 'Z') || c == '_';
}

// Label names (the text before a ':') are [A-Z_][0-9A-Z_]*. An empty one is
// accepted; it simply defines nothing.
bool IsLabelName(std::string_view name)
{
  if (!name.empty() && !IsUpperOrUnderscore(name.front()))
    return false;
  return std::ranges::all_of(name, [](char c) { return IsUpperOrUnderscore(c) || IsDecimalDigit(c); });
}

// The value of c as a digit of the given base, or -1.
int DigitValue(char c, u32 base)
{
  int value = -1;
  if (IsDecimalDigit(c))
    value = c - '0';
  else if (c >= 'a' && c <= 'f')
    value = c - 'a' + 10;
  else if (c >= 'A' && c <= 'F')
    value = c - 'A' + 10;
  return value >= 0 && static_cast<u32>(value) < base ? value : -1;
}

// Prepares one source line in place: strips // and /* */ comments (the latter may
// span lines, tracked by in_comment), cuts the line at ';' or a line break, turns
// tabs into spaces and upper-cases everything outside double quotes.
void NormalizeLine(char* line, size_t size, bool& in_comment)
{
  bool upper = true;
  for (size_t i = 0; i < size; ++i)
  {
    char c = line[i];
    const char next = i + 1 < size ? line[i + 1] : '\0';

    if (c == '/' && next == '/')
    {
      c = '\0';
    }
    else if (c == '/' && next == '*')
    {
      in_comment = !in_comment;
    }
    else if (c == '*' && next == '/' && in_comment)
    {
      in_comment = false;
      c = ' ';
      line[i + 1] = ' ';
    }

    if (in_comment && static_cast<unsigned char>(c) > ' ')
      c = ' ';

    switch (c)
    {
    case '\r':
    case '\n':
    case ';':
      c = '\0';
      break;
    case '\t':
      c = ' ';
      break;
    case '"':
      upper = !upper;
      break;
    default:
      break;
    }
    if (upper && c >= 'a' && c <= 'z')
      c = static_cast<char>(c - 'a' + 'A');

    line[i] = c;
    if (c == '\0')
      return;
  }
}

// The field mask with its trailing zero bits removed: the largest value the field holds.
int MaskShiftedDown(u16 mask)
{
  if (mask == 0)
    return 0;
  while ((mask & 1) == 0)
    mask = static_cast<u16>(mask >> 1);
  return mask;
}

enum class AccumulatorPart
{
  Whole,
  Middle,
  Low,
  Other,
};

AccumulatorPart ClassifyAccumulator(int reg)
{
  if (reg >= DSP_REG_ACC0_FULL && reg <= DSP_REG_ACC1_FULL)
    return AccumulatorPart::Whole;
  if (reg >= DSP_REG_ACM0 && reg <= DSP_REG_ACM1)
    return AccumulatorPart::Middle;
  if (reg >= DSP_REG_ACL0 && reg <= DSP_REG_ACL1)
    return AccumulatorPart::Low;
  return AccumulatorPart::Other;
}

// Stands in for operand slots past the end of a template's parameter list.
constexpr param2_t NO_FIELD{};
}  // namespace

DSPAssembler::DSPAssembler(const AssemblerSettings& settings) : m_settings(settings)
{
}

DSPAssembler::~DSPAssembler() = default;

bool DSPAssembler::Assemble(const std::string& text, std::vector<u16>& code,
                            std::vector<int>* line_numbers)
{
  if (line_numbers != nullptr)
    line_numbers->clear();

  StartPass(1);
  if (!RunPass(text, 1))
    return false;
  if (m_total_size <= 0)
    return false;

  // Words that no instruction writes keep whatever the buffer held.
  m_output_buffer.resize(static_cast<size_t>(m_total_size));

  StartPass(2);
  if (!RunPass(text, 2))
    return false;

  code = std::move(m_output_buffer);
  m_output_buffer = {};

  m_last_error_str = "(no errors)";
  m_last_error = AssemblerError::OK;
  return true;
}

// ---------------------------------------------------------------------------
// Messages

std::string DSPAssembler::DescribePosition() const
{
  std::string line = m_position.line_text;
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
    line.pop_back();

  std::string text = fmt::format("At line {}", m_position.line_num);
  if (m_position.include_path)
    text += fmt::format(" of included file {}", *m_position.include_path);
  if (m_position.opcode_type)
  {
    text += *m_position.opcode_type == OpcodeType::Primary ? ", main opcode" : ", extension opcode";
    if (m_position.operand_number)
      text += fmt::format(" parameter {}", *m_position.operand_number);
  }
  text += ":\n";
  text += line;
  return text;
}

void DSPAssembler::RecordError(AssemblerError error, const std::optional<std::string>& detail)
{
  if (!m_settings.force)
    m_failed = true;

  const char* const what = ERROR_TEXT[static_cast<size_t>(error)];
  m_last_error_str = detail ? fmt::format("{}\nERROR: {}: {}\n\n", DescribePosition(), what, *detail) :
                              fmt::format("{}\nERROR: {}\n\n", DescribePosition(), what);
  fmt::print(stderr, "{}", m_last_error_str);
  m_last_error = error;
}

void DSPAssembler::Fail(AssemblerError error)
{
  RecordError(error, std::nullopt);
}

template <typename... Args>
void DSPAssembler::Fail(AssemblerError error, fmt::format_string<Args...> format, Args&&... args)
{
  RecordError(error, fmt::format(format, std::forward<Args>(args)...));
}

template <typename... Args>
void DSPAssembler::Warn(fmt::format_string<Args...> format, Args&&... args)
{
  fmt::print(stderr, "{}\nWARNING: {}\n\n", DescribePosition(),
             fmt::format(format, std::forward<Args>(args)...));
}

// ---------------------------------------------------------------------------
// Expressions
//
// The evaluator works on text. Parenthesised groups are evaluated first and
// replaced by their decimal value; then the expression is split at the first
// '+', or failing that the first '-', '*', '/', '|' or '&' (in that order), and
// both sides are evaluated recursively, left side first. All arithmetic is on
// 32-bit unsigned values.

u32 DSPAssembler::EvaluateTerm(const std::string& term)
{
  const auto char_at = [&term](size_t i) { return i < term.size() ? term[i] : '\0'; };

  size_t pos = 0;
  bool negate = false;
  if (char_at(pos) == '#')
  {
    ++pos;
    negate = true;
  }
  if (char_at(pos) == '-')
  {
    ++pos;
    negate = true;
  }

  u32 value = 0;
  const auto read_digits = [&](size_t from, u32 base, AssemblerError error) {
    for (size_t i = from; i < term.size(); ++i)
    {
      value *= base;
      const int digit = DigitValue(term[i], base);
      if (digit >= 0)
        value += static_cast<u32>(digit);
      else
        Fail(error, "{}", term);
    }
  };

  const char first = char_at(pos);
  const char second = char_at(pos + 1);
  if (first == '0' && !IsDecimalDigit(second))
  {
    // "0X..." is hexadecimal and "0'..." binary; any other "0..." is just zero.
    if (second == 'X')
      read_digits(pos + 2, 16, AssemblerError::IncorrectHex);
    else if (second == '\'')
      read_digits(pos + 2, 2, AssemblerError::IncorrectBinary);
  }
  else if (IsDecimalDigit(first))
  {
    read_digits(pos, 10, AssemblerError::IncorrectDecimal);
  }
  else
  {
    // A label. Its value is used as is: a leading sign does not apply to it.
    if (const std::optional<u16> label_value =
            m_labels.GetLabelValue(std::string_view(term).substr(pos)))
    {
      return *label_value;
    }
    // Forward references are fine in the first pass.
    if (m_cur_pass == 2)
      Fail(AssemblerError::UnknownLabel, "{}", term);
  }

  return negate ? 0u - value : value;
}

// Finds the first parenthesised group. When there is one, expression is cut down
// to the text before it, inner receives the text inside and the text after it is
// returned. A ')' seen before any '(' is kept as part of the group's text.
std::optional<std::string> DSPAssembler::ExtractBracketGroup(std::string& expression,
                                                             std::string& inner)
{
  std::optional<size_t> open_at;
  int depth = 0;
  for (size_t i = 0; i < expression.size(); ++i)
  {
    const char c = expression[i];
    if (c == '(' && !open_at)
    {
      open_at = i;
      depth = 1;
      continue;
    }
    if (c == '(')
    {
      ++depth;
    }
    else if (c == ')')
    {
      if (--depth == 0)
      {
        std::string after = expression.substr(i + 1);
        expression.resize(*open_at);
        return after;
      }
    }
    else if (!open_at)
    {
      continue;
    }
    inner += c;
  }

  // An unclosed group still cuts the expression short.
  if (open_at)
    expression.resize(*open_at);
  if (depth != 0)
    Fail(AssemblerError::NoMatchingBrackets);
  return std::nullopt;
}

u32 DSPAssembler::EvaluateExpression(std::string expression)
{
  std::string inner;
  while (const std::optional<std::string> after = ExtractBracketGroup(expression, inner))
  {
    const u32 group_value = EvaluateExpression(inner);
    expression += fmt::format("{}", static_cast<s32>(group_value));
    expression += *after;
    inner.clear();
  }

  std::erase(expression, ' ');

  // A '-' that cannot be a subtraction is a sign; mark it so the split below skips it.
  for (size_t i = 0; i < expression.size(); ++i)
  {
    if (expression[i] != '-')
      continue;
    const char before = i == 0 ? '\0' : expression[i - 1];
    if (i == 0 || before == '/' || before == '%' || before == '*')
      expression[i] = '#';
  }

  for (const char op : {'+', '-', '*', '/', '|', '&'})
  {
    const size_t at = expression.find(op);
    if (at == std::string::npos)
      continue;

    const u32 lhs = EvaluateExpression(expression.substr(0, at));
    const u32 rhs = EvaluateExpression(expression.substr(at + 1));
    u32 result = 0;
    switch (op)
    {
    case '+':
      result = lhs + rhs;
      break;
    case '-':
      result = lhs - rhs;
      if (static_cast<s32>(result) < 0)
      {
        Warn("Number Underflow: {}", static_cast<s32>(result));
        // Folded into 17 bits so that no '-' appears in the text again.
        result = 0x10000 + (result & 0xffff);
      }
      break;
    case '*':
      result = lhs * rhs;
      break;
    case '/':
      result = rhs != 0 ? lhs / rhs : 0;
      break;
    case '|':
      result = lhs | rhs;
      break;
    default:
      result = lhs & rhs;
      break;
    }
    expression = fmt::format("{}", static_cast<s32>(result));
  }

  return EvaluateTerm(expression);
}

// ---------------------------------------------------------------------------
// Operands

// Reads up to MAX_OPERANDS comma separated operands from text (which is modified).
// The first character picks the kind: '"' string, '#' immediate, "@$" register
// pointer, '@' memory address, '$' register, anything else a value.
u32 DSPAssembler::ParseOperands(char* text, OperandList& operands)
{
  Tokenizer tokens;
  u32 count = 0;
  for (char* item = tokens.Next(SkipSpaces(text), ","); item != nullptr && count < MAX_OPERANDS;
       item = tokens.Next(nullptr, ","))
  {
    item = SkipSpaces(item);
    if (*item == '\0')
      break;

    Operand& operand = operands[count++];
    operand.kind = P_NONE;
    switch (item[0])
    {
    case '"':
    {
      // Re-tokenizing for the closing quote also ends the operand list unless text
      // follows the string within the same operand.
      const char* const quoted = tokens.Next(item, "\"");
      operand.text = quoted != nullptr ? quoted : "";
      operand.kind = P_STR;
      break;
    }
    case '#':
      operand.value = EvaluateExpression(item + 1);
      operand.kind = P_IMM;
      break;
    case '@':
      if (item[1] == '$')
      {
        operand.value = EvaluateExpression(item + 2);
        operand.kind = P_PRG;
      }
      else
      {
        operand.value = EvaluateExpression(item + 1);
        operand.kind = P_MEM;
      }
      break;
    case '$':
      operand.value = EvaluateExpression(item + 1);
      operand.kind = P_REG;
      break;
    default:
      operand.value = EvaluateExpression(item);
      operand.kind = P_VAL;
      break;
    }
  }
  return count;
}

const DSPOPCTemplate* DSPAssembler::LookUpOpcode(std::string name, size_t operand_count,
                                                 OpcodeType type)
{
  // CW ("constant word") and anything else starting with those letters.
  if (name.starts_with("CW"))
    return &cw;

  if (const auto alias = m_aliases.find(name); alias != m_aliases.end())
    name = alias->second;

  const DSPOPCTemplate* const op =
      type == OpcodeType::Primary ? FindOpInfoByName(name) : FindExtOpInfoByName(name);
  if (op == nullptr)
  {
    Fail(AssemblerError::UnknownOpcode);
    return nullptr;
  }

  if (operand_count < op->param_count)
    Fail(AssemblerError::NotEnoughParameters);
  else if (operand_count > op->param_count)
    Fail(AssemblerError::TooManyParameters);
  return op;
}

void DSPAssembler::CheckRegisterOperand(const param2_t& field, const Operand& operand)
{
  const int reg = static_cast<int>(operand.value);
  const u32 index = operand.value & 1;
  const AccumulatorPart part = ClassifyAccumulator(reg);

  switch (field.type)
  {
  case P_REG18:
  case P_REG19:
  case P_REG1A:
  case P_REG1C:
  {
    const int lowest = (field.type >> 8) & 0x1f;
    if (reg < lowest || reg > lowest + MaskShiftedDown(field.mask))
      Fail(AssemblerError::InvalidRegister);
    break;
  }

  case P_PRG:
    if (reg < DSP_REG_AR0 || reg > DSP_REG_AR3)
      Fail(AssemblerError::InvalidRegister);
    break;

  case P_ACC:
    if (part == AccumulatorPart::Middle)
      Warn("$ACM{0} register used instead of $ACC{0} register", index);
    else if (part == AccumulatorPart::Low)
      Warn("$ACL{0} register used instead of $ACC{0} register", index);
    else if (part != AccumulatorPart::Whole)
      Fail(AssemblerError::WrongParameterExpectedAccumulator);
    break;

  case P_ACCM:
    if (part == AccumulatorPart::Low)
      Warn("$ACL{0} register used instead of $ACCM{0} register", index);
    else if (part == AccumulatorPart::Whole)
      Warn("$ACC{0} register used instead of $ACM{0} register", index);
    else if (part != AccumulatorPart::Middle)
      Fail(AssemblerError::WrongParameterExpectedAccumulator);
    break;

  case P_ACCL:
    if (part == AccumulatorPart::Middle)
      Warn("$ACM{0} register used instead of $ACL{0} register", index);
    else if (part == AccumulatorPart::Whole)
      Warn("$ACC{0} register used instead of $ACL{0} register", index);
    else if (part != AccumulatorPart::Low)
      Fail(AssemblerError::WrongParameterExpectedAccumulator);
    break;

  default:
    break;
  }
}

// Range checks for numeric operands. Fields are treated as signed or unsigned
// depending on their width and kind, and the messages say which ranges would do.
void DSPAssembler::CheckNumberRange(const param2_t& field, const Operand& operand)
{
  const int largest = MaskShiftedDown(field.mask);
  const unsigned int negative_floor = 0xffff & ~static_cast<unsigned int>(largest >> 1);
  const u32 raw = operand.value;
  const int value = static_cast<int>(raw);
  constexpr AssemblerError error = AssemblerError::NumberOutOfRange;

  if (value < 0)
  {
    if (largest == 7)  // SBCLR/SBSET bit numbers
    {
      Fail(error, "Value must be from 0x0 to {:#x}, was {:#x}", largest, value);
    }
    else if (field.type == P_MEM)
    {
      if (largest < 256)
      {
        Fail(error, "Address value must be from {:#x} to {:#x}, was {:#x}", negative_floor,
             largest >> 1, value);
      }
      else
      {
        Fail(error, "Address value must be from 0x0 to {:#x}, was {:#x}", largest, value);
      }
    }
    else
    {
      const int lowest = -((largest >> 1) + 1);
      if (value < lowest && largest < 128)
      {
        Fail(error, "Value must be from {:#x} to {:#x}, was {:#x}", lowest, largest >> 1, value);
      }
      else if (value < lowest)
      {
        Fail(error, "Value must be from {:#x} to {:#x} or 0x0 to {:#x}, was {:#x}", lowest,
             largest >> 1, largest, value);
      }
    }
    return;
  }

  if (largest == 7)
  {
    if (raw > 7u)
      Fail(error, "Value must be from {:#x} to {:#x}, was {:#x}\n", negative_floor, largest, raw);
    return;
  }

  if (field.type == P_MEM)
  {
    // Short addresses are sign extended bytes, so 0xff80-0xffff are fine as well.
    const int limit = largest < 256 ? largest >> 1 : largest;
    const bool fits = raw <= static_cast<u32>(limit) || (raw >= negative_floor && raw <= 0xffff);
    if (fits)
      return;
    if (limit < 256)
    {
      Fail(error, "Address value must be from {:#x} to {:#x}, was {:04x}\n", negative_floor, limit,
           raw);
    }
    else
    {
      Fail(error, "Address value must be less than {:#x}, was {:04x}\n", limit + 1, raw);
    }
    return;
  }

  // Narrow fields (shift amounts and the like) are signed.
  const int limit = largest < 128 ? largest >> 1 : largest;
  if (raw <= static_cast<u32>(limit))
    return;
  if (limit < 64)
    Fail(error, "Value must be from {:#x} to {:#x}, was {:#x}\n", -(limit + 1), limit, raw);
  else
    Fail(error, "Value must be less than {:#x}, was {:#x}\n", limit + 1, raw);
}

void DSPAssembler::CheckOperands(const DSPOPCTemplate& op, const OperandList& operands,
                                 size_t count)
{
  for (size_t i = 0; i < count; ++i)
  {
    m_position.operand_number = i + 1;
    const param2_t& field = i < std::size(op.params) ? op.params[i] : NO_FIELD;
    const Operand& operand = operands[i];
    const bool is_register = (operand.kind & P_REG) != 0;

    if (field.type == operand.kind && !is_register)
    {
      if ((field.type & 3) != 0 && (operand.kind & 3) != 0)
        CheckNumberRange(field, operand);
      continue;
    }

    // Plain values may name data or instruction addresses.
    if (operand.kind == P_VAL && (field.type == P_ADDR_I || field.type == P_ADDR_D))
      continue;

    if ((field.type & P_REG) != 0 && is_register)
    {
      CheckRegisterOperand(field, operand);
      continue;
    }

    // The operand is of the wrong kind: say what was given, then give up on the rest.
    switch (operand.kind & (P_REG | 7))
    {
    case P_REG:
      Fail(AssemblerError::ExpectedParamReg);
      break;
    case P_MEM:
      Fail(AssemblerError::ExpectedParamMem);
      break;
    case P_VAL:
      Fail(AssemblerError::ExpectedParamVal);
      break;
    case P_IMM:
      Fail(AssemblerError::ExpectedParamImm);
      break;
    default:
      break;
    }
    Fail(AssemblerError::WrongParameter);
    break;
  }
  m_position.operand_number = std::nullopt;
}

// ORs the instruction and its operand fields into the output at the current address.
void DSPAssembler::Encode(const DSPOPCTemplate& op, const OperandList& operands, size_t count)
{
  const auto merge = [this](u32 index, u16 bits) {
    if (index < m_output_buffer.size())
      m_output_buffer[index] |= bits;
  };

  merge(m_cur_addr, op.opcode);
  for (size_t i = 0; i < count && i < std::size(op.params); ++i)
  {
    const param2_t& field = op.params[i];
    // The "other accumulator" operands are implied by their partner and have no bits.
    if (field.type == P_ACC_D || field.type == P_ACCM_D)
      continue;

    u16 bits = static_cast<u16>(operands[i].value);
    if (field.lshift > 0)
      bits = static_cast<u16>(bits << field.lshift);
    else
      bits = static_cast<u16>(bits >> -field.lshift);
    merge(m_cur_addr + field.loc, static_cast<u16>(bits & field.mask));
  }
}

// ---------------------------------------------------------------------------
// Passes

void DSPAssembler::StartPass(int pass)
{
  m_failed = false;
  if (pass == 1)
  {
    m_labels.Clear();
    m_labels.RegisterDefaults();
    m_aliases = {{"S15", "SET15"}, {"S16", "SET16"}, {"S40", "SET40"}};
  }
  m_cur_addr = 0;
  m_total_size = 0;
  m_cur_segment = Segment::Code;
  m_segment_addr.fill(0);
}

bool DSPAssembler::HandleDirective(const char* name, const OperandList& operands, int pass)
{
  const Operand& arg = operands[0];
  const std::string_view directive = name;

  if (directive == "INCLUDE")
  {
    if (arg.kind != P_STR)
    {
      Fail(AssemblerError::ExpectedParamStr);
      return true;
    }
    const SourcePosition including = m_position;
    m_position.include_path = m_include_dir.empty() ? arg.text : m_include_dir + '/' + arg.text;
    std::string included_text;
    File::ReadFileToString(*m_position.include_path, included_text);
    RunPass(included_text, pass);
    m_position = including;
    return true;
  }

  if (directive == "INCDIR")
  {
    if (arg.kind == P_STR)
      m_include_dir = arg.text;
    else
      Fail(AssemblerError::ExpectedParamStr);
    return true;
  }

  if (directive == "ORG")
  {
    if (arg.kind == P_VAL)
    {
      m_total_size = static_cast<int>(std::max(m_cur_addr, arg.value));
      m_cur_addr = arg.value;
    }
    else
    {
      Fail(AssemblerError::ExpectedParamVal);
    }
    return true;
  }

  if (directive == "WARNPC")
  {
    if (arg.kind != P_VAL)
      Fail(AssemblerError::ExpectedParamVal);
    else if (m_cur_addr > arg.value)
      Fail(AssemblerError::PCOutOfRange, "WARNPC at 0x{:04x}, expected 0x{:04x} or less",
           m_cur_addr, arg.value);
    return true;
  }

  if (directive == "SEGMENT")
  {
    if (arg.kind != P_STR)
    {
      Fail(AssemblerError::ExpectedParamStr);
      return true;
    }
    m_segment_addr[static_cast<size_t>(m_cur_segment)] = m_cur_addr;
    if (arg.text == "DATA")
      m_cur_segment = Segment::Data;
    if (arg.text == "CODE")
      m_cur_segment = Segment::Code;
    m_cur_addr = m_segment_addr[static_cast<size_t>(m_cur_segment)];
    return true;
  }

  return false;
}

bool DSPAssembler::RunPass(const std::string& text, int pass)
{
  std::istringstream source(text);
  std::array<char, LINE_BUFFER_SIZE> line{};
  bool in_comment = false;

  m_position.line_num = 0;
  m_cur_pass = pass;

  while (!m_failed && source.getline(line.data(), static_cast<std::streamsize>(line.size())))
  {
    m_position.line_text = line.data();
    ++m_position.line_num;
    NormalizeLine(line.data(), line.size(), in_comment);

    // "NAME:" at the very start defines a label.
    char* cursor = line.data();
    std::string label;
    const std::string_view cleaned = line.data();
    if (const size_t colon = cleaned.find(':');
        colon != std::string_view::npos && IsLabelName(cleaned.substr(0, colon)))
    {
      label = cleaned.substr(0, colon);
      cursor += colon + 1;
    }

    // MNEMONIC[ 'EXTENSION] operands[ : extension operands]
    Tokenizer words;
    char* mnemonic = words.Next(cursor, " ");
    const char* extension = nullptr;
    OperandList operands{};
    OperandList ext_operands{};
    u32 operand_count = 0;
    u32 ext_operand_count = 0;
    if (mnemonic != nullptr)
    {
      if (char* tick = std::strchr(mnemonic, '\''))
      {
        *tick = '\0';
        if (tick[1] != '\0')
          extension = tick + 1;
      }

      char* operand_text = words.Next(nullptr, "");
      char* ext_operand_text = nullptr;
      if (operand_text != nullptr)
      {
        if (char* colon = std::strchr(operand_text, ':'))
        {
          *colon = '\0';
          ext_operand_text = colon + 1;
        }
        operand_count = ParseOperands(operand_text, operands);
      }
      if (ext_operand_text != nullptr)
        ext_operand_count = ParseOperands(ext_operand_text, ext_operands);
    }

    if (!label.empty())
    {
      // "NAME: EQU value" binds a value; otherwise the label is the current address.
      u32 value = m_cur_addr;
      if (mnemonic != nullptr && std::strcmp(mnemonic, "EQU") == 0)
      {
        value = operands[0].value;
        mnemonic = nullptr;
      }
      if (pass == 1 && !m_labels.RegisterLabel(label, static_cast<u16>(value)))
        Fail(AssemblerError::LabelAlreadyExists);
    }

    if (mnemonic == nullptr || HandleDirective(mnemonic, operands, pass))
      continue;

    m_position.opcode_type = OpcodeType::Primary;
    const DSPOPCTemplate* op = LookUpOpcode(mnemonic, operand_count, OpcodeType::Primary);
    if (op == nullptr)
      op = &cw;
    const u32 size = op->size;
    CheckOperands(*op, operands, operand_count);

    const DSPOPCTemplate* ext_op = nullptr;
    m_position.opcode_type = OpcodeType::Extension;
    if (!op->extended)
    {
      if (extension != nullptr)
        Fail(AssemblerError::CantExtendOpcode);
      if (ext_operand_count != 0)
        Fail(AssemblerError::ExtensionParamsOnNonExtendableOpcode);
    }
    else if (extension != nullptr)
    {
      ext_op = LookUpOpcode(extension, ext_operand_count, OpcodeType::Extension);
      if (ext_op != nullptr)
        CheckOperands(*ext_op, ext_operands, ext_operand_count);
    }
    else if (ext_operand_count != 0)
    {
      Fail(AssemblerError::ExtensionParamsOnNonExtendableOpcode);
    }
    m_position.opcode_type = std::nullopt;

    if (pass == 2)
    {
      if (m_cur_addr < m_output_buffer.size())
        m_output_buffer[m_cur_addr] = 0;
      Encode(*op, operands, operand_count);
      if (ext_op != nullptr)
        Encode(*ext_op, ext_operands, ext_operand_count);
    }

    m_cur_addr += size;
    m_total_size += static_cast<int>(size);
  }

  return !m_failed;
}
}  // namespace DSP
