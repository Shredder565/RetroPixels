// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "InputCommon/ControllerInterface/CoreDevice.h"

// The mapping expression language: tokens, the parser entry points, the expression tree
// interface and the environment that binds control names to device inputs and outputs.
namespace ciface::ExpressionParser
{
// Token kinds. The numeric order matters: binary operators form one contiguous block (with the
// operators that can be combined with '=' at its start), see Token::IsBinaryOperator().
enum TokenType
{
  TOK_WHITESPACE,
  TOK_INVALID,
  TOK_EOF,
  TOK_LPAREN,
  TOK_RPAREN,
  TOK_NOT,
  TOK_CONTROL,
  TOK_LITERAL,
  TOK_VARIABLE,
  TOK_BAREWORD,
  TOK_COMMENT,
  TOK_HOTKEY,
  TOK_QUESTION,
  TOK_COLON,
  TOK_BINARY_OPS_BEGIN,
  TOK_COMPOUND_ASSIGN_OPS_BEGIN = TOK_BINARY_OPS_BEGIN,
  TOK_AND = TOK_BINARY_OPS_BEGIN,
  TOK_OR,
  TOK_XOR,
  TOK_ADD,
  TOK_SUB,
  TOK_MUL,
  TOK_DIV,
  TOK_MOD,
  TOK_COMPOUND_ASSIGN_OPS_END,
  TOK_ASSIGN = TOK_COMPOUND_ASSIGN_OPS_END,
  TOK_LTHAN,
  TOK_GTHAN,
  TOK_COMMA,
  TOK_BINARY_OPS_END,
};

class Token
{
public:
  TokenType type;
  // Text of names, literals and variables; for a compound assignment ("+=" and so on) the
  // operator character.
  std::string data;

  // Where the token was found in the source text (used for syntax highlighting).
  std::size_t string_position = 0;
  std::size_t string_length = 0;

  explicit Token(TokenType type_);
  Token(TokenType type_, std::string data_);

  bool IsBinaryOperator() const;
};

enum class ParseStatus
{
  Successful,
  // The text did not parse. A fallback expression (the whole text as one control name) may
  // still be usable.
  SyntaxError,
  // Nothing but whitespace: the control reads its default value.
  EmptyExpression,
};

class Lexer
{
public:
  std::string expr;
  std::string::iterator it;

  explicit Lexer(std::string expr_);

  // Appends every token up to and including TOK_EOF. Stops after the first TOK_INVALID and
  // reports SyntaxError in that case.
  ParseStatus Tokenize(std::vector<Token>& tokens);

private:
  Token ScanOne();
  std::string TakeWordCharacters();
  Token TakeQuoted(TokenType type, char closing);
  Token TakeNumber(char first);
};

class ControlQualifier
{
public:
  bool has_device = false;
  Core::DeviceQualifier device_qualifier;
  // Name of the input or output (the parser does not tell them apart).
  std::string control_name;

  ControlQualifier() = default;

  operator std::string() const
  {
    if (!has_device)
      return control_name;
    return device_qualifier.ToString() + ":" + control_name;
  }

  // "Device:Control" names a device explicitly; the last ':' separates the two parts.
  void FromString(const std::string& str)
  {
    const std::size_t split = str.rfind(':');
    has_device = split != std::string::npos;
    device_qualifier.FromString(has_device ? str.substr(0, split) : std::string());
    control_name = has_device ? str.substr(split + 1) : str;
  }
};

class ControlEnvironment
{
public:
  using VariableContainer = std::map<std::string, std::shared_ptr<ControlState>>;

  ControlEnvironment(const Core::DeviceContainer& container_, const Core::DeviceQualifier& default_,
                     VariableContainer& vars)
      : m_vars(vars), m_devices(container_), m_default(default_)
  {
  }

  std::shared_ptr<Core::Device> FindDevice(const ControlQualifier& qualifier) const;
  Core::Device::Input* FindInput(const ControlQualifier& qualifier) const;
  Core::Device::Output* FindOutput(const ControlQualifier& qualifier) const;

  // Returns the variable with this name, creating it (with value 0) if needed.
  std::shared_ptr<ControlState> GetVariablePtr(const std::string& name);

  // Drops variables that no expression refers to any more.
  void CleanUnusedVariables();

private:
  VariableContainer& m_vars;
  const Core::DeviceContainer& m_devices;
  const Core::DeviceQualifier& m_default;
};

class Expression
{
public:
  virtual ~Expression() = default;
  virtual ControlState GetValue() = 0;
  virtual void SetValue(ControlState state) = 0;
  virtual int CountNumControls() const = 0;
  virtual void UpdateReferences(ControlEnvironment& finder) = 0;

  // Runs the expression's side effects and returns the expression an assignment should write
  // to (the expression itself unless it is an assignment, a comma or a conditional).
  virtual Expression* GetLValue();
};

class ParseResult
{
public:
  static ParseResult MakeEmptyResult();
  static ParseResult MakeSuccessfulResult(std::unique_ptr<Expression>&& expr);
  static ParseResult MakeErrorResult(Token token, std::string description);

  ParseStatus status = ParseStatus::EmptyExpression;
  std::unique_ptr<Expression> expr;

  // Set for syntax errors: the offending token and a translated description.
  std::optional<Token> token;
  std::optional<std::string> description;

private:
  ParseResult() = default;
};

ParseResult ParseExpression(const std::string& expr);
ParseResult ParseTokens(const std::vector<Token>& tokens);

}  // namespace ciface::ExpressionParser
