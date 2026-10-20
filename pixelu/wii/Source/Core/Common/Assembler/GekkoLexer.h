// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <cstddef>
#include <deque>
#include <optional>
#include <string_view>
#include <vector>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail
{
// Appends the bytes a quoted string literal (including its quotes) stands for, with C-style
// escapes resolved.
void ConvertStringLiteral(std::string_view literal, std::vector<u8>* out_vec);

enum class TokenType
{
  Invalid,
  Identifier,
  StringLit,
  HexadecimalLit,
  DecimalLit,
  OctalLit,
  BinaryLit,
  FloatLit,
  GPR,
  FPR,
  CRField,
  SPR,
  Lt,
  Gt,
  Eq,
  So,
  NumLabFwd,
  NumLabBwd,
  // Separates statements.
  Eol,
  Eof,

  Dot,
  Colon,
  Comma,
  Lparen,
  Rparen,
  Pipe,
  Caret,
  Ampersand,
  Lsh,
  Rsh,
  Plus,
  Minus,
  Star,
  Slash,
  Tilde,
  Grave,
  At,

  OperatorBegin = Dot,
  LastToken = At,
};

std::string_view TokenTypeToStr(TokenType);

struct AssemblerToken
{
  TokenType token_type;
  std::string_view token_val;
  // For Invalid tokens: why, and which part of token_val is at fault.
  std::string_view invalid_reason;
  Interval invalid_region;

  std::string_view TypeStr() const;
  // token_val, or a placeholder for tokens that have no visible text.
  std::string_view ValStr() const;

  // The token's numeric value. Defined for u8, u16, u32, u64 (integers, registers, labels;
  // wrapping on overflow) and float, double (float literals).
  template <typename T>
  std::optional<T> EvalToken() const;
};

struct CursorPosition
{
  size_t index = 0;
  size_t line = 0;
  size_t col = 0;
};

// Tokenizer with unbounded lookahead. Tokens are produced lazily; how identifiers are split
// depends on the current match rule, and changing the rule re-reads any buffered lookahead.
class Lexer
{
public:
  enum class IdentifierMatchRule
  {
    Typical,
    Mnemonic,   // may also contain '+', '-' and '.' (branch hints, record forms)
    Directive,  // may start with a digit (".4byte")
  };

  explicit Lexer(std::string_view str) : m_src(str) {}

  // Position of the next unconsumed token.
  size_t LineNumber() const;
  size_t ColNumber() const;
  // The full source line holding the next token, including its newline if it has one.
  std::string_view CurrentLine() const;

  void SetIdentifierMatchRule(IdentifierMatchRule rule);

  const Tagged<CursorPosition, AssemblerToken>& LookaheadTagRef(size_t num_fwd) const;
  AssemblerToken Lookahead() const;
  const AssemblerToken& LookaheadRef() const;
  TokenType LookaheadType() const;
  // Reads the next token as a floating point literal; only directives need that.
  AssemblerToken LookaheadFloat() const;
  void Eat();
  void EatAndReset();

  template <size_t N>
  void LookaheadN(std::array<AssemblerToken, N>* tokens_out) const
  {
    Fill(N);
    for (size_t i = 0; i < N; i++)
      (*tokens_out)[i] = ValueOf(m_ahead[i]);
  }

  template <size_t N>
  void LookaheadTaggedN(std::array<Tagged<CursorPosition, AssemblerToken>, N>* tokens_out) const
  {
    Fill(N);
    for (size_t i = 0; i < N; i++)
      (*tokens_out)[i] = m_ahead[i];
  }

  template <size_t N>
  void EatN()
  {
    for (size_t i = 0; i < N; i++)
      Eat();
  }

private:
  // Makes sure at least `n` tokens are buffered.
  void Fill(size_t n) const;
  void Rewind() const;
  AssemblerToken Scan() const;

  std::string_view m_src;
  // Where scanning resumes: just past the last scanned token and the blanks after it.
  mutable CursorPosition m_pos;
  mutable std::deque<Tagged<CursorPosition, AssemblerToken>> m_ahead;
  IdentifierMatchRule m_rule = IdentifierMatchRule::Typical;
};
}  // namespace Common::GekkoAssembler::detail
