// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Assembler/GekkoLexer.h"

#include <array>
#include <cstdlib>
#include <string>

#include "Common/Assembler/AssemblerTables.h"

namespace Common::GekkoAssembler::detail
{
namespace
{
// Character classes. Only ASCII counts; bytes >= 0x80 are never letters, digits or blanks.
constexpr bool IsDigit(char c)
{
  return c >= '0' && c <= '9';
}
constexpr bool IsLetter(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
constexpr bool IsHex(char c)
{
  return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
constexpr bool IsOctal(char c)
{
  return c >= '0' && c <= '7';
}
constexpr bool IsBinary(char c)
{
  return c == '0' || c == '1';
}
// Whitespace other than the newline, which is a token of its own.
constexpr bool IsBlank(char c)
{
  return c == ' ' || c == '\t' || c == '\v' || c == '\f' || c == '\r';
}
constexpr char Lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
constexpr u32 HexValue(char c)
{
  if (IsDigit(c))
    return static_cast<u32>(c - '0');
  return static_cast<u32>(Lower(c) - 'a' + 10);
}

bool SameText(std::string_view a, std::string_view b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++)
  {
    if (Lower(a[i]) != Lower(b[i]))
      return false;
  }
  return true;
}

// A read position in the source. Reading past the end yields NUL, as does an embedded NUL byte;
// either one ends the token stream.
struct Cursor
{
  std::string_view src;
  CursorPosition pos;

  char Peek() const { return pos.index < src.size() ? src[pos.index] : '\0'; }

  void Step()
  {
    if (pos.index >= src.size())
      return;
    if (src[pos.index] == '\n')
    {
      pos.line++;
      pos.col = 0;
    }
    else
    {
      pos.col++;
    }
    pos.index++;
  }

  // Blanks, then at most one comment running to the end of the line.
  void SkipBlank()
  {
    while (IsBlank(Peek()))
      Step();
    if (Peek() == '#')
    {
      while (Peek() != '\n' && Peek() != '\0')
        Step();
    }
  }

  template <typename Pred>
  void StepWhile(Pred pred)
  {
    while (pred(Peek()))
      Step();
  }
};

// Matches the rest of a string literal after its opening quote. Returns the failure reason, if
// any; the cursor is left after the last character that could be part of the literal.
std::optional<std::string_view> MatchStringBody(Cursor& c)
{
  enum class State
  {
    Text,
    Escape,
    HexStart,
  };
  State state = State::Text;
  constexpr std::string_view unterminated = "Invalid string: No terminating \"";

  for (char ch = c.Peek(); ch != '\0'; ch = c.Peek())
  {
    switch (state)
    {
    case State::Text:
      c.Step();
      if (ch == '\n')
        return unterminated;
      if (ch == '"')
        return std::nullopt;
      if (ch == '\\')
        state = State::Escape;
      break;

    case State::Escape:
      c.Step();
      if (ch == '\n')
        return unterminated;
      state = ch == 'x' ? State::HexStart : State::Text;
      break;

    case State::HexStart:
      if (!IsHex(ch))
        return "Invalid string: bad hex escape";
      c.Step();
      state = State::Text;
      break;
    }
  }
  return state == State::HexStart ? "Invalid string: bad hex escape" : unterminated;
}

// Matches [+-]?(\d+(\.\d+)?|\.\d+)(e[+-]?\d+)? as far as possible.
std::optional<std::string_view> MatchFloat(Cursor& c)
{
  constexpr std::string_view no_value = "Invalid float: No numeric value";
  constexpr std::string_view no_fraction = "Invalid float: No numeric value after decimal point";
  constexpr std::string_view no_exponent =
      "Invalid float: No numeric value following exponent signifier";

  const auto digits = [&c] {
    bool any = false;
    while (IsDigit(c.Peek()))
    {
      c.Step();
      any = true;
    }
    return any;
  };

  if (c.Peek() == '+' || c.Peek() == '-')
    c.Step();

  if (c.Peek() == '.')
  {
    c.Step();
    if (!digits())
      return no_fraction;
  }
  else
  {
    if (!digits())
      return no_value;
    if (c.Peek() == '.')
    {
      c.Step();
      if (!digits())
        return no_fraction;
    }
  }

  if (c.Peek() == 'e')
  {
    c.Step();
    if (c.Peek() == '+' || c.Peek() == '-')
      c.Step();
    if (!digits())
      return no_exponent;
  }
  return std::nullopt;
}

// Characters that form a token on their own.
struct Punctuator
{
  char ch;
  TokenType type;
};
constexpr Punctuator kPunctuators[] = {
    {'.', TokenType::Dot},   {':', TokenType::Colon},     {',', TokenType::Comma},
    {'(', TokenType::Lparen}, {')', TokenType::Rparen},   {'|', TokenType::Pipe},
    {'^', TokenType::Caret}, {'&', TokenType::Ampersand}, {'+', TokenType::Plus},
    {'-', TokenType::Minus}, {'*', TokenType::Star},      {'/', TokenType::Slash},
    {'~', TokenType::Tilde}, {'`', TokenType::Grave},     {'@', TokenType::At},
    {'\n', TokenType::Eol},  {'\0', TokenType::Eof},
};

TokenType PunctuationType(char ch)
{
  for (const Punctuator& p : kPunctuators)
  {
    if (p.ch == ch)
      return p.type;
  }
  return TokenType::Invalid;
}

// "0".."31" without leading zeros.
bool IsRegisterNumber(std::string_view s)
{
  if (s.size() == 1)
    return IsDigit(s[0]);
  if (s.size() != 2 || !IsDigit(s[0]) || !IsDigit(s[1]))
    return false;
  return s[0] == '1' || s[0] == '2' || (s[0] == '3' && s[1] <= '1');
}

TokenType ClassifyWord(std::string_view word)
{
  const char first = Lower(word[0]);
  if (first == 'r' && IsRegisterNumber(word.substr(1)))
    return TokenType::GPR;
  if (SameText(word, "sp") || SameText(word, "rtoc"))
    return TokenType::GPR;
  if (first == 'f' && IsRegisterNumber(word.substr(1)))
    return TokenType::FPR;
  if (word.size() == 3 && SameText(word.substr(0, 2), "cr") && word[2] >= '0' && word[2] <= '7')
    return TokenType::CRField;

  static constexpr std::pair<std::string_view, TokenType> cr_bits[] = {
      {"lt", TokenType::Lt}, {"gt", TokenType::Gt}, {"eq", TokenType::Eq}, {"so", TokenType::So}};
  for (const auto& [name, type] : cr_bits)
  {
    if (SameText(word, name))
      return type;
  }
  if (FindSprName(word))
    return TokenType::SPR;
  return TokenType::Identifier;
}

template <typename T>
T Accumulate(std::string_view digits, u32 base)
{
  T value = 0;
  for (const char c : digits)
    value = static_cast<T>(value * base + HexValue(c));
  return value;
}

// Tokens whose value is "skip a fixed prefix, read the rest as digits in some base".
struct DigitForm
{
  TokenType type;
  size_t prefix;
  u32 base;
};
constexpr DigitForm kDigitForms[] = {
    {TokenType::DecimalLit, 0, 10}, {TokenType::HexadecimalLit, 2, 16},
    {TokenType::OctalLit, 1, 8},    {TokenType::BinaryLit, 2, 2},
    {TokenType::FPR, 1, 10},        {TokenType::CRField, 2, 10},
};

template <typename T>
std::optional<T> IntegerValue(TokenType type, std::string_view text)
{
  for (const DigitForm& form : kDigitForms)
  {
    if (form.type == type)
      return Accumulate<T>(text.substr(form.prefix), form.base);
  }

  if (type == TokenType::GPR)
  {
    // Two GPRs also have ABI names.
    if (SameText(text, "sp"))
      return T{1};
    if (SameText(text, "rtoc"))
      return T{2};
    return Accumulate<T>(text.substr(1), 10);
  }
  if (type == TokenType::SPR)
    return static_cast<T>(FindSprName(text).value_or(0));

  // lt, gt, eq, so are declared in bit order, so the bit number is the offset from lt.
  const int cr_bit = static_cast<int>(type) - static_cast<int>(TokenType::Lt);
  if (cr_bit >= 0 && cr_bit <= static_cast<int>(TokenType::So) - static_cast<int>(TokenType::Lt))
    return static_cast<T>(cr_bit);

  if (type == TokenType::NumLabFwd || type == TokenType::NumLabBwd)
  {
    // Everything but the trailing 'f' or 'b'.
    return Accumulate<T>(text.substr(0, text.size() - 1), 10);
  }
  return std::nullopt;
}
}  // namespace

void ConvertStringLiteral(std::string_view literal, std::vector<u8>* out_vec)
{
  // Everything between the quotes. The lexer has already checked the escapes are well formed.
  const std::string_view body = literal.substr(1, literal.size() - 2);
  size_t i = 0;
  const auto at = [&body](size_t n) { return n < body.size() ? body[n] : '"'; };

  while (i < body.size())
  {
    const char c = body[i++];
    if (c != '\\')
    {
      out_vec->push_back(static_cast<u8>(c));
      continue;
    }

    const char e = at(i);
    if (IsOctal(e))
    {
      u8 value = 0;
      while (IsOctal(at(i)))
        value = static_cast<u8>(value * 8 + (at(i++) - '0'));
      out_vec->push_back(value);
    }
    else if (e == 'x')
    {
      i++;
      u8 value = 0;
      while (IsHex(at(i)))
        value = static_cast<u8>(value * 16 + HexValue(at(i++)));
      out_vec->push_back(value);
    }
    else
    {
      i++;
      char out;
      switch (e)
      {
      case 'a':
        out = '\a';
        break;
      case 'b':
        out = '\b';
        break;
      case 'f':
        out = '\f';
        break;
      case 'n':
        out = '\n';
        break;
      case 'r':
        out = '\r';
        break;
      case 't':
        out = '\t';
        break;
      case 'v':
        out = '\v';
        break;
      default:
        // \', \", \?, \\ and any other character stand for themselves.
        out = e;
        break;
      }
      out_vec->push_back(static_cast<u8>(out));
    }
  }
}

namespace
{
// Display names, indexed by TokenType. Numeric labels have no name of their own.
using TypeNameTable = std::array<std::string_view, static_cast<size_t>(TokenType::LastToken) + 1>;
constexpr TypeNameTable kTypeNames = [] {
  TypeNameTable n{};
  const auto set = [&n](TokenType t, std::string_view s) { n[static_cast<size_t>(t)] = s; };
  set(TokenType::Invalid, "Invalid");
  set(TokenType::Identifier, "Identifier");
  set(TokenType::StringLit, "String Literal");
  set(TokenType::HexadecimalLit, "Hexadecimal Literal");
  set(TokenType::DecimalLit, "Decimal Literal");
  set(TokenType::OctalLit, "Octal Literal");
  set(TokenType::BinaryLit, "Binary Literal");
  set(TokenType::FloatLit, "Float Literal");
  set(TokenType::GPR, "GPR");
  set(TokenType::FPR, "FPR");
  set(TokenType::CRField, "CR Field");
  set(TokenType::SPR, "SPR");
  for (TokenType bit : {TokenType::Lt, TokenType::Gt, TokenType::Eq, TokenType::So})
    set(bit, "CR Bit");
  set(TokenType::Eol, "End of Line");
  set(TokenType::Eof, "End of File");
  set(TokenType::Lsh, "<<");
  set(TokenType::Rsh, ">>");
  // Every single-character operator is shown as its own character.
  for (const Punctuator& p : kPunctuators)
  {
    if (static_cast<int>(p.type) >= static_cast<int>(TokenType::OperatorBegin))
      set(p.type, std::string_view(&p.ch, 1));
  }
  return n;
}();
}  // namespace

std::string_view TokenTypeToStr(TokenType tp)
{
  const auto i = static_cast<size_t>(tp);
  return i < kTypeNames.size() ? kTypeNames[i] : std::string_view{};
}

std::string_view AssemblerToken::TypeStr() const
{
  return TokenTypeToStr(token_type);
}

std::string_view AssemblerToken::ValStr() const
{
  if (token_type == TokenType::Eol)
    return "<EOL>";
  if (token_type == TokenType::Eof)
    return "<EOF>";
  return token_val;
}

// Out-of-range float literals become infinity or zero rather than failing.
template <>
std::optional<float> AssemblerToken::EvalToken() const
{
  if (token_type != TokenType::FloatLit)
    return std::nullopt;
  const std::string text(token_val);
  return std::strtof(text.c_str(), nullptr);
}

template <>
std::optional<double> AssemblerToken::EvalToken() const
{
  if (token_type != TokenType::FloatLit)
    return std::nullopt;
  const std::string text(token_val);
  return std::strtod(text.c_str(), nullptr);
}

template <>
std::optional<u8> AssemblerToken::EvalToken() const
{
  return IntegerValue<u8>(token_type, token_val);
}

template <>
std::optional<u16> AssemblerToken::EvalToken() const
{
  return IntegerValue<u16>(token_type, token_val);
}

template <>
std::optional<u32> AssemblerToken::EvalToken() const
{
  return IntegerValue<u32>(token_type, token_val);
}

template <>
std::optional<u64> AssemblerToken::EvalToken() const
{
  return IntegerValue<u64>(token_type, token_val);
}

size_t Lexer::LineNumber() const
{
  return m_ahead.empty() ? m_pos.line : TagOf(m_ahead.front()).line;
}

size_t Lexer::ColNumber() const
{
  return m_ahead.empty() ? m_pos.col : TagOf(m_ahead.front()).col;
}

std::string_view Lexer::CurrentLine() const
{
  const size_t at = m_ahead.empty() ? m_pos.index : TagOf(m_ahead.front()).index;
  const size_t start = at == 0 ? 0 : m_src.rfind('\n', at - 1) + 1;  // npos + 1 == 0
  const size_t newline = m_src.find('\n', start);
  const size_t end = newline == std::string_view::npos ? m_src.size() : newline + 1;
  return m_src.substr(start, end - start);
}

void Lexer::SetIdentifierMatchRule(IdentifierMatchRule rule)
{
  Rewind();
  m_rule = rule;
}

void Lexer::Rewind() const
{
  if (m_ahead.empty())
    return;
  m_pos = TagOf(m_ahead.front());
  m_ahead.clear();
}

void Lexer::Fill(size_t n) const
{
  while (m_ahead.size() < n)
  {
    const CursorPosition at = m_pos;
    AssemblerToken tok = Scan();
    m_ahead.emplace_back(at, tok);
  }
}

const Tagged<CursorPosition, AssemblerToken>& Lexer::LookaheadTagRef(size_t num_fwd) const
{
  Fill(num_fwd + 1);
  return m_ahead[num_fwd];
}

AssemblerToken Lexer::Lookahead() const
{
  return LookaheadRef();
}

const AssemblerToken& Lexer::LookaheadRef() const
{
  Fill(1);
  return ValueOf(m_ahead.front());
}

TokenType Lexer::LookaheadType() const
{
  return LookaheadRef().token_type;
}

AssemblerToken Lexer::LookaheadFloat() const
{
  Rewind();
  Cursor c{m_src, m_pos};
  c.SkipBlank();
  const CursorPosition start = c.pos;

  std::optional<std::string_view> failure = MatchFloat(c);
  if (c.pos.index == start.index)
    c.Step();  // always take at least one character so the error has something to point at

  const std::string_view text = m_src.substr(start.index, c.pos.index - start.index);
  AssemblerToken tok;
  if (failure)
    tok = {TokenType::Invalid, text, *failure, Interval{0, text.size()}};
  else
    tok = {TokenType::FloatLit, text, "", Interval{0, 0}};

  // Unlike other tokens, blanks after a float are left for the next token to skip.
  m_pos = c.pos;
  m_ahead.emplace_back(start, tok);
  return tok;
}

void Lexer::Eat()
{
  if (m_ahead.empty())
    Scan();
  else
    m_ahead.pop_front();
}

void Lexer::EatAndReset()
{
  Eat();
  SetIdentifierMatchRule(IdentifierMatchRule::Typical);
}

AssemblerToken Lexer::Scan() const
{
  Cursor c{m_src, m_pos};
  c.SkipBlank();
  const CursorPosition start = c.pos;

  const char head = c.Peek();
  c.Step();

  TokenType type;
  std::string_view reason;
  Interval region{0, 0};

  const bool word_start = IsLetter(head) || head == '_' ||
                          (m_rule == IdentifierMatchRule::Directive && IsDigit(head));
  if (word_start)
  {
    const bool mnemonic = m_rule == IdentifierMatchRule::Mnemonic;
    c.StepWhile([mnemonic](char ch) {
      return IsLetter(ch) || IsDigit(ch) || ch == '_' ||
             (mnemonic && (ch == '+' || ch == '-' || ch == '.'));
    });
    type = ClassifyWord(m_src.substr(start.index, c.pos.index - start.index));
  }
  else if (head == '"')
  {
    type = TokenType::StringLit;
    if (const auto failure = MatchStringBody(c))
    {
      type = TokenType::Invalid;
      reason = *failure;
      region = Interval{0, c.pos.index - start.index};
    }
  }
  else if (head == '0' && c.Peek() == 'x')
  {
    c.Step();
    c.StepWhile(IsHex);
    type = TokenType::HexadecimalLit;
  }
  else if (head == '0' && c.Peek() == 'b')
  {
    c.Step();
    if (IsBinary(c.Peek()))
    {
      c.StepWhile(IsBinary);
      type = TokenType::BinaryLit;
    }
    else
    {
      type = TokenType::NumLabBwd;  // "0b" is a reference to local label 0
    }
  }
  else if (head == '0' && IsOctal(c.Peek()))
  {
    c.StepWhile(IsOctal);
    type = TokenType::OctalLit;
  }
  else if (head == '0' && c.Peek() == 'f')
  {
    c.Step();
    type = TokenType::NumLabFwd;
  }
  else if (head == '0')
  {
    type = TokenType::DecimalLit;
  }
  else if (IsDigit(head))
  {
    c.StepWhile(IsDigit);
    type = TokenType::DecimalLit;
    if (c.Peek() == 'f' || c.Peek() == 'b')
    {
      type = c.Peek() == 'f' ? TokenType::NumLabFwd : TokenType::NumLabBwd;
      c.Step();
    }
  }
  else if ((head == '<' || head == '>') && c.Peek() == head)
  {
    c.Step();
    type = head == '<' ? TokenType::Lsh : TokenType::Rsh;
  }
  else
  {
    type = PunctuationType(head);
    if (head == '<' || head == '>')
      type = TokenType::Invalid;
    if (type == TokenType::Invalid)
    {
      reason = "Unrecognized character";
      region = Interval{0, 1};
    }
  }

  AssemblerToken tok{type, m_src.substr(start.index, c.pos.index - start.index), reason, region};
  c.SkipBlank();
  m_pos = c.pos;
  return tok;
}
}  // namespace Common::GekkoAssembler::detail
