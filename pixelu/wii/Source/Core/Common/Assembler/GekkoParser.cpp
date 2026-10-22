// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Assembler/GekkoParser.h"

#include <array>
#include <optional>
#include <span>

#include <fmt/format.h>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/AssemblerTables.h"
#include "Common/Assembler/GekkoLexer.h"
#include "Common/Assert.h"

namespace Common::GekkoAssembler::detail
{
namespace
{
// Binary operators, loosest first. All are left associative.
struct BinaryOp
{
  TokenType token;
  int level;
  AsmOp op;
};

constexpr BinaryOp kBinaryOps[] = {
    {TokenType::Pipe, 0, AsmOp::Or},  {TokenType::Caret, 1, AsmOp::Xor},
    {TokenType::Ampersand, 2, AsmOp::And}, {TokenType::Lsh, 3, AsmOp::Lsh},
    {TokenType::Rsh, 3, AsmOp::Rsh},  {TokenType::Plus, 4, AsmOp::Add},
    {TokenType::Minus, 4, AsmOp::Sub}, {TokenType::Star, 5, AsmOp::Mul},
    {TokenType::Slash, 5, AsmOp::Div},
};

const BinaryOp* FindBinaryOp(TokenType t)
{
  for (const BinaryOp& b : kBinaryOps)
  {
    if (b.token == t)
      return &b;
  }
  return nullptr;
}

std::optional<Terminal> NumberTerminal(TokenType t)
{
  switch (t)
  {
  case TokenType::HexadecimalLit:
    return Terminal::Hex;
  case TokenType::DecimalLit:
    return Terminal::Dec;
  case TokenType::OctalLit:
    return Terminal::Oct;
  case TokenType::BinaryLit:
    return Terminal::Bin;
  default:
    return std::nullopt;
  }
}

std::optional<Terminal> BuiltinTerminal(TokenType t)
{
  switch (t)
  {
  case TokenType::GPR:
    return Terminal::GPR;
  case TokenType::FPR:
    return Terminal::FPR;
  case TokenType::SPR:
    return Terminal::SPR;
  case TokenType::CRField:
    return Terminal::CRField;
  case TokenType::Lt:
    return Terminal::Lt;
  case TokenType::Gt:
    return Terminal::Gt;
  case TokenType::Eq:
    return Terminal::Eq;
  case TokenType::So:
    return Terminal::So;
  default:
    return std::nullopt;
  }
}

// Tokens an expression can start with.
bool StartsExpression(TokenType t)
{
  switch (t)
  {
  case TokenType::Minus:
  case TokenType::Tilde:
  case TokenType::Lparen:
  case TokenType::Grave:
  case TokenType::Identifier:
  case TokenType::Dot:
  case TokenType::NumLabFwd:
  case TokenType::NumLabBwd:
    return true;
  default:
    return NumberTerminal(t).has_value() || BuiltinTerminal(t).has_value();
  }
}

// What the operand list of each ParseAlg looks like.
enum class Piece
{
  Operand,
  Comma,
  OptionalComma,  // a comma and the rest, or nothing more
  Open,
  Close,
};

std::span<const Piece> Layout(ParseAlg alg)
{
  using P = Piece;
  static constexpr P op1[] = {P::Operand};
  static constexpr P op2[] = {P::Operand, P::Comma, P::Operand};
  static constexpr P op3[] = {P::Operand, P::Comma, P::Operand, P::Comma, P::Operand};
  static constexpr P op4[] = {P::Operand, P::Comma, P::Operand, P::Comma,
                              P::Operand, P::Comma, P::Operand};
  static constexpr P op5[] = {P::Operand, P::Comma, P::Operand, P::Comma, P::Operand,
                              P::Comma,   P::Operand, P::Comma, P::Operand};
  static constexpr P op1or2[] = {P::Operand, P::OptionalComma, P::Operand};
  static constexpr P op2or3[] = {P::Operand, P::Comma, P::Operand, P::OptionalComma, P::Operand};
  static constexpr P off[] = {P::Operand, P::Comma, P::Operand, P::Open, P::Operand, P::Close};
  static constexpr P off_op2[] = {P::Operand, P::Comma, P::Operand, P::Open,  P::Operand,
                                  P::Close,   P::Comma, P::Operand, P::Comma, P::Operand};
  switch (alg)
  {
  case ParseAlg::Op1:
    return op1;
  case ParseAlg::Op2:
    return op2;
  case ParseAlg::Op3:
    return op3;
  case ParseAlg::Op4:
    return op4;
  case ParseAlg::Op5:
    return op5;
  case ParseAlg::Op1Or2:
    return op1or2;
  case ParseAlg::Op2Or3:
    return op2or3;
  case ParseAlg::Op1Off1:
    return off;
  case ParseAlg::Op1Off1Op2:
    return off_op2;
  default:
    return {};
  }
}

class Parser
{
public:
  explicit Parser(ParseState& state) : s(state), lex(state.lexer), cb(state.plugin) {}

  void Program()
  {
    if (lex.LookaheadType() == TokenType::Eof)
    {
      s.eof = true;
      return;
    }
    if (!Statement())
      return;

    while (!s.eof && !s.error)
    {
      const AssemblerToken next = lex.Lookahead();
      if (next.token_type == TokenType::Eof)
      {
        s.eof = true;
      }
      else if (next.token_type == TokenType::Eol)
      {
        lex.Eat();
        if (!Statement())
          return;
      }
      else
      {
        Fail(fmt::format("Unexpected token '{}' where line should have ended", next.ValStr()));
      }
    }
  }

private:
  bool Failed() const { return s.error.has_value(); }
  void Fail(std::string message) { s.EmitErrorHere(std::move(message)); }

  // An optional label, then an optional instruction or directive. False on error.
  bool Statement()
  {
    Label();
    if (Failed())
      return false;
    if (s.HasToken(TokenType::Dot))
    {
      s.ParseToken(TokenType::Dot);
      Directive();
    }
    else
    {
      Instruction();
    }
    return !Failed();
  }

  void Label()
  {
    std::array<AssemblerToken, 2> t;
    lex.LookaheadN(&t);
    if (t[1].token_type != TokenType::Colon)
      return;

    if (t[0].token_type == TokenType::Identifier)
    {
      cb.OnLabelDecl(t[0].token_val);
    }
    else if (t[0].token_type == TokenType::DecimalLit)
    {
      const std::optional<u32> number = t[0].EvalToken<u32>();
      if (!number)
        return;
      cb.OnNumericLabelDecl(t[0].token_val, *number);
    }
    else
    {
      return;
    }
    if (!Failed())
      lex.EatN<2>();
  }

  // --- instructions -------------------------------------------------------------------------

  void Instruction()
  {
    lex.SetIdentifierMatchRule(Lexer::IdentifierMatchRule::Mnemonic);
    const AssemblerToken mnemonic = lex.Lookahead();
    if (mnemonic.token_type != TokenType::Identifier)
    {
      // Nothing (or not an instruction) on this line; let the caller deal with it.
      lex.SetIdentifierMatchRule(Lexer::IdentifierMatchRule::Typical);
      return;
    }

    bool extended = false;
    const ParseInfo* info = FindMnemonic(mnemonic.token_val, &extended);
    if (info == nullptr)
    {
      Fail(fmt::format("Unknown or unsupported mnemonic '{}'", mnemonic.ValStr()));
      return;
    }

    cb.OnInstructionPre(*info, extended);
    lex.EatAndReset();
    Operands(info->parse_algorithm);
    if (Failed())
      return;
    cb.OnInstructionPost(*info, extended);
  }

  void Operands(ParseAlg alg)
  {
    if (alg == ParseAlg::NoneOrOp1)
    {
      if (StartsExpression(lex.LookaheadType()))
        Operand();
      return;
    }

    for (const Piece piece : Layout(alg))
    {
      switch (piece)
      {
      case Piece::Operand:
        Operand();
        break;
      case Piece::Comma:
        s.ParseToken(TokenType::Comma);
        break;
      case Piece::Open:
        s.ParseToken(TokenType::Lparen);
        break;
      case Piece::Close:
        s.ParseToken(TokenType::Rparen);
        break;
      case Piece::OptionalComma:
        if (!s.HasToken(TokenType::Comma))
          return;
        s.ParseToken(TokenType::Comma);
        break;
      }
      if (Failed())
        return;
    }
  }

  void Operand()
  {
    cb.OnOperandPre();
    Expression(0);
    if (!Failed())
      cb.OnOperandPost();
  }

  // --- directives ---------------------------------------------------------------------------

  void Directive()
  {
    lex.SetIdentifierMatchRule(Lexer::IdentifierMatchRule::Directive);
    const AssemblerToken name = lex.Lookahead();
    if (name.token_type != TokenType::Identifier)
    {
      Fail(fmt::format("Unexpected token '{}' in directive type", name.ValStr()));
      return;
    }
    const GekkoDirective* directive = FindDirective(name.token_val);
    if (directive == nullptr)
    {
      Fail(fmt::format("Unknown assembler directive '{}'", name.ValStr()));
      return;
    }

    cb.OnDirectivePre(*directive);
    lex.EatAndReset();
    switch (*directive)
    {
    case GekkoDirective::Byte:
    case GekkoDirective::_2byte:
    case GekkoDirective::_4byte:
    case GekkoDirective::_8byte:
      CommaList([this] { ResolvedExpression(); });
      break;
    case GekkoDirective::Float:
    case GekkoDirective::Double:
      CommaList([this] { FloatValue(); });
      break;
    case GekkoDirective::Locate:
    case GekkoDirective::Zeros:
    case GekkoDirective::Skip:
      ResolvedExpression();
      break;
    case GekkoDirective::PadAlign:
    case GekkoDirective::Align:
      Number();
      break;
    case GekkoDirective::DefVar:
      VariableDefinition();
      break;
    case GekkoDirective::Ascii:
    case GekkoDirective::Asciz:
      StringValue();
      break;
    }
    if (!Failed())
      cb.OnDirectivePost(*directive);
  }

  template <typename F>
  void CommaList(F item)
  {
    item();
    while (!Failed() && s.HasToken(TokenType::Comma))
    {
      lex.Eat();
      item();
    }
  }

  void ResolvedExpression()
  {
    cb.OnResolvedExprPre();
    Expression(0);
    if (!Failed())
      cb.OnResolvedExprPost();
  }

  void FloatValue()
  {
    const AssemblerToken tok = lex.LookaheadFloat();
    if (tok.token_type != TokenType::FloatLit)
    {
      Fail("Invalid floating point literal");
      return;
    }
    cb.OnTerminal(Terminal::Flt, tok);
    lex.Eat();
  }

  void VariableDefinition()
  {
    const AssemblerToken name = lex.Lookahead();
    if (name.token_type != TokenType::Identifier)
    {
      Fail(fmt::format("Expected an identifier, but found '{}'", name.ValStr()));
      return;
    }
    cb.OnVarDecl(name.token_val);
    if (Failed())
      return;
    lex.Eat();
    s.ParseToken(TokenType::Comma);
    if (Failed())
      return;
    ResolvedExpression();
  }

  void StringValue()
  {
    const AssemblerToken tok = lex.Lookahead();
    if (tok.token_type != TokenType::StringLit)
    {
      Fail(fmt::format("Expected a string literal, but found '{}'", tok.ValStr()));
      return;
    }
    cb.OnTerminal(Terminal::Str, tok);
    lex.Eat();
  }

  // --- expressions --------------------------------------------------------------------------

  // Parses operators of at least `min_level` binding strength (precedence climbing).
  void Expression(int min_level)
  {
    Unary();
    if (Failed())
      return;
    while (const BinaryOp* op = FindBinaryOp(lex.LookaheadType()))
    {
      if (op->level < min_level)
        return;
      lex.Eat();
      Expression(op->level + 1);
      if (Failed())
        return;
      cb.OnOperator(op->op);
      if (Failed())
        return;
    }
  }

  void Unary()
  {
    const TokenType t = lex.LookaheadType();
    if (t != TokenType::Minus && t != TokenType::Tilde)
    {
      Primary();
      return;
    }
    lex.Eat();
    Unary();
    if (!Failed())
      cb.OnOperator(t == TokenType::Minus ? AsmOp::Neg : AsmOp::Not);
  }

  void Primary()
  {
    if (s.HasToken(TokenType::Lparen))
    {
      cb.OnOpenParen(ParenType::Normal);
      if (Failed())
        return;
      Bracketed(TokenType::Rparen, ParenType::Normal);
    }
    else if (s.HasToken(TokenType::Grave))
    {
      cb.OnOpenParen(ParenType::RelConv);
      Bracketed(TokenType::Grave, ParenType::RelConv);
    }
    else
    {
      Leaf();
    }
  }

  void Bracketed(TokenType closer, ParenType kind)
  {
    lex.Eat();
    Expression(0);
    if (Failed())
      return;
    if (s.HasToken(closer))
      cb.OnCloseParen(kind);
    s.ParseToken(closer);
  }

  void Leaf()
  {
    const AssemblerToken& tok = lex.LookaheadRef();
    const TokenType t = tok.token_type;
    if (NumberTerminal(t))
    {
      Number();
    }
    else if (t == TokenType::Identifier)
    {
      Symbol();
    }
    else if (const std::optional<Terminal> builtin = BuiltinTerminal(t))
    {
      cb.OnTerminal(*builtin, lex.Lookahead());
      if (!Failed())
        lex.Eat();
    }
    else if (t == TokenType::NumLabFwd || t == TokenType::NumLabBwd)
    {
      cb.OnTerminal(t == TokenType::NumLabFwd ? Terminal::NumLabFwd : Terminal::NumLabBwd,
                    lex.Lookahead());
      lex.Eat();
    }
    else if (t == TokenType::Dot)
    {
      cb.OnTerminal(Terminal::Dot, lex.Lookahead());
      if (!Failed())
        lex.Eat();
    }
    else
    {
      Fail(fmt::format("Unexpected token '{}' in expression", tok.ValStr()));
    }
  }

  // A plain integer literal (also the argument of .align and .padalign).
  void Number()
  {
    const AssemblerToken tok = lex.Lookahead();
    const std::optional<Terminal> kind = NumberTerminal(tok.token_type);
    if (!kind)
    {
      Fail(fmt::format("Invalid {} with value '{}'", tok.TypeStr(), tok.ValStr()));
      return;
    }
    cb.OnTerminal(*kind, tok);
    if (!Failed())
      lex.Eat();
  }

  // An identifier, possibly with an @ha or @l suffix selecting half of its address.
  void Symbol()
  {
    std::array<AssemblerToken, 3> t;
    lex.LookaheadN(&t);
    if (t[1].token_type == TokenType::At && (t[2].token_val == "ha" || t[2].token_val == "l"))
    {
      if (t[2].token_val == "ha")
        cb.OnHiaddr(t[0].token_val);
      else
        cb.OnLoaddr(t[0].token_val);
      if (!Failed())
        lex.EatN<3>();
      return;
    }

    cb.OnTerminal(Terminal::Id, t[0]);
    if (!Failed())
      lex.Eat();
  }

  ParseState& s;
  Lexer& lex;
  ParsePlugin& cb;
};
}  // namespace

ParseState::ParseState(std::string_view input_str, ParsePlugin& p)
    : lexer(input_str), plugin(p), eof(false)
{
}

bool ParseState::HasToken(TokenType tp) const
{
  return lexer.LookaheadType() == tp;
}

void ParseState::ParseToken(TokenType tp)
{
  const AssemblerToken tok = lexer.LookaheadRef();
  if (tok.token_type == tp)
    lexer.Eat();
  else
    EmitErrorHere(fmt::format("Expected '{}' but found '{}'", TokenTypeToStr(tp), tok.ValStr()));
}

void ParseState::EmitErrorHere(std::string&& message)
{
  const AssemblerToken tok = lexer.Lookahead();
  AssemblerError err{std::move(message), lexer.CurrentLine(), lexer.LineNumber(),
                     lexer.ColNumber(), tok.token_val.size()};
  if (tok.token_type == TokenType::Invalid)
  {
    // The lexer knows better what is wrong with a malformed token.
    err.message = std::string(tok.invalid_reason);
    err.col += tok.invalid_region.begin;
    err.len = tok.invalid_region.len;
  }
  error = std::move(err);
}

void ParseWithPlugin(ParsePlugin* plugin, std::string_view input)
{
  ParseState state(input, *plugin);
  plugin->SetOwner(&state);

  Parser(state).Program();
  if (!state.error)
    plugin->PostParseAction();
  if (state.error)
  {
    plugin->OnError();
    plugin->ForwardError(std::move(*state.error));
  }

  plugin->SetOwner(nullptr);
}
}  // namespace Common::GekkoAssembler::detail
