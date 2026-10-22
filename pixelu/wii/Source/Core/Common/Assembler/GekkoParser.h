// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Common/Assembler/AssemblerShared.h"
#include "Common/Assembler/AssemblerTables.h"
#include "Common/Assembler/GekkoLexer.h"
#include "Common/CommonTypes.h"

namespace Common::GekkoAssembler::detail
{
class ParsePlugin;

// Everything a parse in progress owns. Plugins can look at the lexer to learn where the parser
// is, and may record an error to stop the parse.
struct ParseState
{
  ParseState(std::string_view input_str, ParsePlugin& plugin);

  bool HasToken(TokenType tp) const;
  // Consumes a token of type `tp`, or records an error if the next token is something else.
  void ParseToken(TokenType tp);
  // Records an error at the next token.
  void EmitErrorHere(std::string&& message);

  Lexer lexer;
  ParsePlugin& plugin;

  std::optional<AssemblerError> error;
  bool eof;
};

enum class AsmOp
{
  Or,
  Xor,
  And,
  Lsh,
  Rsh,
  Add,
  Sub,
  Mul,
  Div,
  Neg,
  Not
};

// Kinds of leaf values in expressions.
enum class Terminal
{
  Hex,
  Dec,
  Oct,
  Bin,
  Flt,
  Str,
  Id,
  GPR,
  FPR,
  SPR,
  CRField,
  Lt,
  Gt,
  Eq,
  So,
  Dot,
  NumLabFwd,
  NumLabBwd,
};

enum class ParenType
{
  Normal,   // ( ... )
  RelConv,  // ` ... `
};

// Receives a callback at each point of interest while the parser walks the input. The default
// implementations do nothing. Expressions are reported in postfix order: operands first, then
// the operator.
class ParsePlugin
{
public:
  ParsePlugin() : m_owner(nullptr) {}
  virtual ~ParsePlugin() = default;

  void SetOwner(ParseState* o) { m_owner = o; }
  void ForwardError(AssemblerError&& err) { m_owner_error = std::move(err); }
  std::optional<AssemblerError>& Error() { return m_owner_error; }

  // Runs once the whole input parsed without error.
  virtual void PostParseAction() {}

  // "Pre" runs before the construct's first token is consumed, "Post" after its last.
  virtual void OnDirectivePre(GekkoDirective) {}
  virtual void OnDirectivePost(GekkoDirective) {}
  virtual void OnInstructionPre(const ParseInfo&, bool) {}
  virtual void OnInstructionPost(const ParseInfo&, bool) {}
  virtual void OnOperandPre() {}
  virtual void OnOperandPost() {}
  virtual void OnResolvedExprPre() {}
  virtual void OnResolvedExprPost() {}

  // Runs after the operator's operands have been reported.
  virtual void OnOperator(AsmOp) {}

  // Token callbacks run while the token is still the lexer's next token.
  virtual void OnTerminal(Terminal, const AssemblerToken&) {}
  virtual void OnHiaddr(std::string_view) {}
  virtual void OnLoaddr(std::string_view) {}
  virtual void OnOpenParen(ParenType) {}
  virtual void OnCloseParen(ParenType) {}
  virtual void OnError() {}
  virtual void OnLabelDecl(std::string_view) {}
  virtual void OnNumericLabelDecl(std::string_view, u32) {}
  virtual void OnVarDecl(std::string_view) {}

protected:
  ParseState* m_owner;
  std::optional<AssemblerError> m_owner_error;
};

// Parses `input`, reporting to `plugin`. Any error ends up in plugin->Error().
void ParseWithPlugin(ParsePlugin* plugin, std::string_view input);
}  // namespace Common::GekkoAssembler::detail
