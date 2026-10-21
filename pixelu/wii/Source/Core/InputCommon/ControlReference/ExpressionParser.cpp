// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "InputCommon/ControlReference/ExpressionParser.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

#include "InputCommon/ControlReference/FunctionExpression.h"

namespace ciface::ExpressionParser
{
using Core::Device;

namespace
{
// ---------------------------------------------------------------------------------------------
// Character classes (plain ASCII, like the "C" locale).

bool IsAsciiLetter(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAsciiDigit(char c)
{
  return c >= '0' && c <= '9';
}

bool IsWordCharacter(char c)
{
  return IsAsciiLetter(c) || IsAsciiDigit(c) || c == '_';
}

// The single-character operators. Everything else yields TOK_INVALID.
struct OperatorCharacter
{
  char character;
  TokenType type;
};
constexpr std::array<OperatorCharacter, 12> OPERATOR_CHARACTERS = {{
    {'&', TOK_AND}, {'|', TOK_OR}, {'^', TOK_XOR}, {'+', TOK_ADD},
    {'-', TOK_SUB}, {'*', TOK_MUL}, {'/', TOK_DIV}, {'%', TOK_MOD},
    {'=', TOK_ASSIGN}, {'<', TOK_LTHAN}, {'>', TOK_GTHAN}, {',', TOK_COMMA},
}};

TokenType OperatorForCharacter(char c)
{
  for (const OperatorCharacter& entry : OPERATOR_CHARACTERS)
  {
    if (entry.character == c)
      return entry.type;
  }
  return TOK_INVALID;
}

// "a op= b" exists for the arithmetic and logic operators only.
bool HasCompoundForm(TokenType type)
{
  return type >= TOK_COMPOUND_ASSIGN_OPS_BEGIN && type < TOK_COMPOUND_ASSIGN_OPS_END;
}

// A number is one or more digits, optionally followed by '.' and one or more digits.
bool IsWellFormedNumber(const std::string& text)
{
  std::size_t pos = 0;
  const auto skip_digits = [&] {
    const std::size_t start = pos;
    while (pos < text.size() && IsAsciiDigit(text[pos]))
      ++pos;
    return pos != start;
  };

  if (!skip_digits())
    return false;
  if (pos == text.size())
    return true;
  if (text[pos] != '.')
    return false;
  ++pos;
  return skip_digits() && pos == text.size();
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// Tokens and the lexer

Token::Token(TokenType type_) : type(type_)
{
}

Token::Token(TokenType type_, std::string data_) : type(type_), data(std::move(data_))
{
}

bool Token::IsBinaryOperator() const
{
  return type >= TOK_BINARY_OPS_BEGIN && type < TOK_BINARY_OPS_END;
}

Lexer::Lexer(std::string expr_) : expr(std::move(expr_)), it(expr.begin())
{
}

std::string Lexer::TakeWordCharacters()
{
  const auto start = it;
  while (it != expr.end() && IsWordCharacter(*it))
    ++it;
  return std::string(start, it);
}

Token Lexer::TakeQuoted(TokenType type, char closing)
{
  // Quoted text may not span lines.
  const auto start = it;
  while (it != expr.end() && *it != closing && *it != '\n')
    ++it;

  if (it == expr.end() || *it != closing)
    return Token(TOK_INVALID);

  std::string text(start, it);
  ++it;
  return Token(type, std::move(text));
}

Token Lexer::TakeNumber(char first)
{
  std::string text(1, first);
  while (it != expr.end() && (IsAsciiDigit(*it) || *it == '.'))
    text.push_back(*it++);

  if (!IsWellFormedNumber(text))
    return Token(TOK_INVALID);
  return Token(TOK_LITERAL, std::move(text));
}

Token Lexer::ScanOne()
{
  if (it == expr.end())
    return Token(TOK_EOF);

  const char c = *it;
  ++it;
  const bool more = it != expr.end();

  // Block comments: everything from "/*" to the next "*/".
  if (c == '/' && more && *it == '*')
  {
    ++it;
    const std::size_t close = expr.find("*/", static_cast<std::size_t>(it - expr.begin()));
    if (close == std::string::npos)
      return Token(TOK_INVALID);
    it = expr.begin() + static_cast<std::ptrdiff_t>(close + 2);
    return Token(TOK_COMMENT);
  }

  if (const TokenType op = OperatorForCharacter(c); op != TOK_INVALID)
  {
    if (more && *it == '=' && HasCompoundForm(op))
    {
      ++it;
      return Token(TOK_ASSIGN, std::string(1, c));
    }
    return Token(op);
  }

  switch (c)
  {
  case ' ':
  case '\t':
  case '\r':
  case '\n':
    return Token(TOK_WHITESPACE);
  case '(':
    return Token(TOK_LPAREN);
  case ')':
    return Token(TOK_RPAREN);
  case '!':
    return Token(TOK_NOT);
  case '@':
    return Token(TOK_HOTKEY);
  case '?':
    return Token(TOK_QUESTION);
  case ':':
    return Token(TOK_COLON);
  case '`':
    return TakeQuoted(TOK_CONTROL, '`');
  case '\'':
    return TakeQuoted(TOK_LITERAL, '\'');
  case '$':
    return Token(TOK_VARIABLE, TakeWordCharacters());
  default:
    break;
  }

  if (IsAsciiLetter(c))
    return Token(TOK_BAREWORD, std::string(1, c) + TakeWordCharacters());
  if (IsAsciiDigit(c))
    return TakeNumber(c);
  return Token(TOK_INVALID);
}

ParseStatus Lexer::Tokenize(std::vector<Token>& tokens)
{
  for (;;)
  {
    const auto start = it;
    Token& token = tokens.emplace_back(ScanOne());
    token.string_position = static_cast<std::size_t>(start - expr.begin());
    token.string_length = static_cast<std::size_t>(it - start);

    if (token.type == TOK_INVALID)
      return ParseStatus::SyntaxError;
    if (token.type == TOK_EOF)
      return ParseStatus::Successful;
  }
}

Expression* Expression::GetLValue()
{
  return this;
}

// ---------------------------------------------------------------------------------------------
// Expression tree nodes

namespace
{
class ControlExpression;

// Hotkey suppression. While "@(Ctrl+X)" is active, plain "X" mappings elsewhere must not fire.
// The registry counts, for each (final input, modifier input) pair, how many active hotkeys
// claim it. An input listed as the final input of any pair reads as 0 from ControlExpression.
class SuppressionRegistry
{
public:
  using Key = std::pair<Device::Input*, Device::Input*>;

  bool IsClaimed(const Device::Input* final_input) const
  {
    const auto first = m_claims.lower_bound(Key{const_cast<Device::Input*>(final_input), nullptr});
    return first != m_claims.end() && first->first.first == final_input;
  }

  // True when some pair for this input has a modifier that is not (a parent or child of) one of
  // the given modifiers.
  template <typename Modifiers>
  bool IsClaimedByOthers(const Device::Input* final_input, const Modifiers& own_modifiers) const
  {
    const auto related = [](const Device::Input* a, const Device::Input* b) {
      if (a == nullptr || b == nullptr)
        return false;
      return a == b || a->IsChild(b) || b->IsChild(a);
    };

    for (auto pos = m_claims.lower_bound(Key{const_cast<Device::Input*>(final_input), nullptr});
         pos != m_claims.end() && pos->first.first == final_input; ++pos)
    {
      const Device::Input* const claimer = pos->first.second;
      const bool is_own = std::ranges::any_of(own_modifiers, [&](const auto& modifier) {
        return related(modifier->GetInput(), claimer);
      });
      if (!is_own)
        return true;
    }
    return false;
  }

  void Claim(Device::Input* final_input, Device::Input* modifier)
  {
    if (final_input != nullptr && modifier != nullptr)
      ++m_claims[Key{final_input, modifier}];
  }

  void Release(Device::Input* final_input, Device::Input* modifier)
  {
    const auto pos = m_claims.find(Key{final_input, modifier});
    if (pos == m_claims.end())
      return;
    if (--pos->second == 0)
      m_claims.erase(pos);
  }

private:
  std::map<Key, u16> m_claims;
};

SuppressionRegistry s_suppressions;

// A reference to one device input and/or output.
class ControlExpression final : public Expression
{
public:
  explicit ControlExpression(ControlQualifier qualifier) : m_qualifier(std::move(qualifier)) {}

  ControlState GetValue() override
  {
    if (s_suppressions.IsClaimed(m_input))
      return 0;
    return GetValueIgnoringSuppression();
  }

  // Negative readings (two opposite directions held) are cut off at 0. Values above 1 are
  // passed on, for unbounded inputs such as motion sensors.
  ControlState GetValueIgnoringSuppression() const
  {
    if (m_input == nullptr)
      return 0.0;
    return std::max(0.0, m_input->GetState());
  }

  void SetValue(ControlState value) override
  {
    if (m_output != nullptr)
      m_output->SetState(value);
  }

  int CountNumControls() const override
  {
    return (m_input != nullptr || m_output != nullptr) ? 1 : 0;
  }

  void UpdateReferences(ControlEnvironment& env) override
  {
    // Holding the device keeps the raw input/output pointers valid.
    m_device = env.FindDevice(m_qualifier);
    m_input = env.FindInput(m_qualifier);
    m_output = env.FindOutput(m_qualifier);
  }

  Device::Input* GetInput() const { return m_input; }

private:
  ControlQualifier m_qualifier;
  std::shared_ptr<Device> m_device;
  Device::Input* m_input = nullptr;
  Device::Output* m_output = nullptr;
};

ControlState ApplyBinaryOperator(TokenType op, ControlState a, ControlState b)
{
  switch (op)
  {
  case TOK_ADD:
    return a + b;
  case TOK_SUB:
    return a - b;
  case TOK_MUL:
    return a * b;
  case TOK_DIV:
  {
    // Division by zero gives 0 instead of an infinity.
    const ControlState quotient = a / b;
    return std::isinf(quotient) ? 0.0 : quotient;
  }
  case TOK_MOD:
  {
    const ControlState remainder = std::fmod(a, b);
    return std::isnan(remainder) ? 0.0 : remainder;
  }
  case TOK_AND:
    return std::min(a, b);
  case TOK_OR:
    return std::max(a, b);
  case TOK_XOR:
    return std::max(std::min(1 - a, b), std::min(a, 1 - b));
  case TOK_LTHAN:
    return a < b;
  case TOK_GTHAN:
    return a > b;
  default:
    assert(false);
    return 0;
  }
}

// "a op b", plus assignment ("a = b") and sequencing ("a, b").
class BinaryExpression : public Expression
{
public:
  BinaryExpression(TokenType op, std::unique_ptr<Expression>&& left,
                   std::unique_ptr<Expression>&& right)
      : m_op(op), m_left(std::move(left)), m_right(std::move(right))
  {
  }

  ControlState GetValue() override
  {
    if (m_op == TOK_ASSIGN || m_op == TOK_COMMA)
      return GetLValue()->GetValue();

    // The left operand is always evaluated first.
    const ControlState a = m_left->GetValue();
    const ControlState b = m_right->GetValue();
    return ApplyBinaryOperator(m_op, a, b);
  }

  Expression* GetLValue() override
  {
    if (m_op == TOK_ASSIGN)
    {
      Expression* const target = m_left->GetLValue();
      target->SetValue(m_right->GetValue());
      return target;
    }
    if (m_op == TOK_COMMA)
    {
      m_left->GetValue();
      return m_right->GetLValue();
    }
    return this;
  }

  // Writing to "a op b" writes the value to both sides.
  void SetValue(ControlState value) override
  {
    m_left->SetValue(value);
    m_right->SetValue(value);
  }

  int CountNumControls() const override
  {
    return m_left->CountNumControls() + m_right->CountNumControls();
  }

  void UpdateReferences(ControlEnvironment& env) override
  {
    m_left->UpdateReferences(env);
    m_right->UpdateReferences(env);
  }

protected:
  const TokenType m_op;
  const std::unique_ptr<Expression> m_left;
  const std::unique_ptr<Expression> m_right;
};

// "a op= b": a = a op b, with the left side's lvalue resolved once.
class CompoundAssignmentExpression final : public BinaryExpression
{
public:
  using BinaryExpression::BinaryExpression;

  ControlState GetValue() override { return GetLValue()->GetValue(); }

  Expression* GetLValue() override
  {
    Expression* const target = m_left->GetLValue();
    const ControlState current = target->GetValue();
    const ControlState operand = m_right->GetValue();
    target->SetValue(ApplyBinaryOperator(m_op, current, operand));
    return target;
  }
};

class LiteralExpression final : public Expression
{
public:
  explicit LiteralExpression(ControlState value) : m_value(value) {}

  ControlState GetValue() override { return m_value; }
  void SetValue(ControlState) override {}
  // A literal counts as bound so that a literal-only expression is not replaced by the
  // bareword fallback.
  int CountNumControls() const override { return 1; }
  void UpdateReferences(ControlEnvironment&) override {}

private:
  const ControlState m_value;
};

// "$name": a value shared by every expression of the same controller.
class VariableExpression final : public Expression
{
public:
  explicit VariableExpression(std::string name) : m_name(std::move(name)) {}

  ControlState GetValue() override { return m_storage ? *m_storage : 0; }

  void SetValue(ControlState value) override
  {
    if (m_storage)
      *m_storage = value;
  }

  int CountNumControls() const override { return 1; }

  void UpdateReferences(ControlEnvironment& env) override
  {
    m_storage = env.GetVariablePtr(m_name);
  }

private:
  const std::string m_name;
  std::shared_ptr<ControlState> m_storage;
};

// "@(Mod1+Mod2+Key)": passes Key through while all modifiers are held, and suppresses plain
// mappings of Key meanwhile.
class HotkeyExpression final : public Expression
{
public:
  using InputList = std::vector<std::unique_ptr<ControlExpression>>;

  explicit HotkeyExpression(InputList inputs) : m_modifiers(std::move(inputs))
  {
    m_key = std::move(m_modifiers.back());
    m_modifiers.pop_back();
  }

  ~HotkeyExpression() override { Disengage(); }

  ControlState GetValue() override
  {
    // Stops at the first modifier that is not held.
    const bool modifiers_held =
        std::ranges::all_of(m_modifiers, [](const std::unique_ptr<ControlExpression>& modifier) {
          return modifier->GetValue() > CONDITION_THRESHOLD;
        });

    const ControlState key_value = m_key->GetValueIgnoringSuppression();

    if (!modifiers_held)
    {
      Disengage();
      // Pressing the modifiers while the key is already down must not trigger the hotkey.
      m_waiting_for_release = key_value > CONDITION_THRESHOLD;
      return 0;
    }

    // Our own modifiers (or related ones such as L/R Ctrl for Ctrl) never block us, so a hotkey
    // with more modifiers can still fire.
    const bool claimed_elsewhere = s_suppressions.IsClaimedByOthers(m_key->GetInput(), m_modifiers);

    if (key_value <= CONDITION_THRESHOLD)
      m_waiting_for_release = false;
    if (claimed_elsewhere)
      m_waiting_for_release = true;
    if (m_waiting_for_release)
      return 0;

    if (!m_engaged)
      Engage();
    return key_value;
  }

  void SetValue(ControlState) override {}

  int CountNumControls() const override
  {
    int count = 0;
    for (const auto& modifier : m_modifiers)
      count += modifier->CountNumControls();
    return count + m_key->CountNumControls();
  }

  void UpdateReferences(ControlEnvironment& env) override
  {
    for (const auto& modifier : m_modifiers)
      modifier->UpdateReferences(env);
    m_key->UpdateReferences(env);

    // An engaged hotkey re-registers its claims here: the claims for the current inputs are
    // added and then the previous registration is dropped, which looks the pairs up by the
    // current inputs as well. The two steps cancel out, so the registry keeps the pairs that were
    // claimed before the update.
    if (m_engaged)
    {
      ClaimAll();
      ReleaseAll();
    }
  }

private:
  void ClaimAll()
  {
    for (const auto& modifier : m_modifiers)
      s_suppressions.Claim(m_key->GetInput(), modifier->GetInput());
  }

  void ReleaseAll()
  {
    for (const auto& modifier : m_modifiers)
      s_suppressions.Release(m_key->GetInput(), modifier->GetInput());
  }

  void Engage()
  {
    ClaimAll();
    m_engaged = true;
  }

  void Disengage()
  {
    if (!m_engaged)
      return;
    m_engaged = false;
    ReleaseAll();
  }

  InputList m_modifiers;
  std::unique_ptr<ControlExpression> m_key;
  bool m_engaged = false;
  bool m_waiting_for_release = false;
};

// Reads from its first child when that child is bound to something, otherwise from the second.
// ParseExpression() uses it to try the whole text as a single control name (old-style
// mappings such as "Button A") before the parsed expression. A keyboard default device and a
// one-digit expression therefore bind to the digit key, although a NumericSetting simplifies
// such text back to a number first.
class CoalesceExpression final : public Expression
{
public:
  CoalesceExpression(std::unique_ptr<Expression>&& preferred,
                     std::unique_ptr<Expression>&& fallback)
      : m_preferred(std::move(preferred)), m_fallback(std::move(fallback))
  {
  }

  ControlState GetValue() override { return Active().GetValue(); }
  void SetValue(ControlState value) override { Active().SetValue(value); }
  int CountNumControls() const override { return Active().CountNumControls(); }

  void UpdateReferences(ControlEnvironment& env) override
  {
    m_preferred->UpdateReferences(env);
    m_fallback->UpdateReferences(env);
  }

private:
  Expression& Active() const
  {
    return m_preferred->CountNumControls() > 0 ? *m_preferred : *m_fallback;
  }

  std::unique_ptr<Expression> m_preferred;
  std::unique_ptr<Expression> m_fallback;
};
}  // namespace

// ---------------------------------------------------------------------------------------------
// Control environment

std::shared_ptr<Device> ControlEnvironment::FindDevice(const ControlQualifier& qualifier) const
{
  return m_devices.FindDevice(qualifier.has_device ? qualifier.device_qualifier : m_default);
}

Device::Input* ControlEnvironment::FindInput(const ControlQualifier& qualifier) const
{
  const std::shared_ptr<Device> device = FindDevice(qualifier);
  return device ? device->FindInput(qualifier.control_name) : nullptr;
}

Device::Output* ControlEnvironment::FindOutput(const ControlQualifier& qualifier) const
{
  const std::shared_ptr<Device> device = FindDevice(qualifier);
  return device ? device->FindOutput(qualifier.control_name) : nullptr;
}

std::shared_ptr<ControlState> ControlEnvironment::GetVariablePtr(const std::string& name)
{
  // The lexer never produces an empty variable name, but refuse one anyway.
  if (name.empty())
    return nullptr;

  std::shared_ptr<ControlState>& slot = m_vars[name];
  if (slot == nullptr)
    slot = std::make_shared<ControlState>();
  return slot;
}

void ControlEnvironment::CleanUnusedVariables()
{
  // The map's own reference does not count.
  std::erase_if(m_vars, [](const auto& entry) { return entry.second.use_count() <= 1; });
}

// ---------------------------------------------------------------------------------------------
// Parse results

ParseResult ParseResult::MakeEmptyResult()
{
  ParseResult empty;
  empty.status = ParseStatus::EmptyExpression;
  return empty;
}

ParseResult ParseResult::MakeSuccessfulResult(std::unique_ptr<Expression>&& expr)
{
  ParseResult success;
  success.expr = std::move(expr);
  success.status = ParseStatus::Successful;
  return success;
}

ParseResult ParseResult::MakeErrorResult(Token token, std::string description)
{
  ParseResult failure;
  failure.description = std::move(description);
  failure.token = std::move(token);
  failure.status = ParseStatus::SyntaxError;
  return failure;
}

// ---------------------------------------------------------------------------------------------
// Parser: precedence climbing over the token list.

namespace
{
// Lower numbers bind tighter. Assignment and the conditional operator group to the right.
constexpr int LOOSEST = 999;

// Operators that share a strength are listed together.
struct Binding
{
  TokenType type;
  int strength;
};
constexpr std::array<Binding, 13> BINDINGS = {{
    {TOK_MUL, 1}, {TOK_DIV, 1}, {TOK_MOD, 1},
    {TOK_ADD, 2}, {TOK_SUB, 2},
    {TOK_LTHAN, 3}, {TOK_GTHAN, 3},
    {TOK_AND, 4},
    {TOK_XOR, 5},
    {TOK_OR, 6},
    {TOK_ASSIGN, 7}, {TOK_QUESTION, 7},
    {TOK_COMMA, 8},
}};

constexpr int BindingStrength(TokenType type)
{
  for (const Binding& binding : BINDINGS)
  {
    if (binding.type == type)
      return binding.strength;
  }
  return LOOSEST;
}

class Parser
{
public:
  explicit Parser(const std::vector<Token>& tokens) : m_pos(tokens.begin()) {}

  ParseResult ParseAll()
  {
    ParseResult result = ParseExpressionUpTo(LOOSEST);
    if (result.status != ParseStatus::Successful)
      return result;

    const Token& next = Current();
    if (next.type != TOK_EOF)
      return ParseResult::MakeErrorResult(next, Common::GetStringT("Expected end of expression."));
    return result;
  }

private:
  // The next meaningful token (comments and whitespace are skipped for good).
  const Token& Current()
  {
    while (m_pos->type == TOK_WHITESPACE || m_pos->type == TOK_COMMENT)
      ++m_pos;
    return *m_pos;
  }

  // Consumes and returns the next meaningful token. TOK_EOF is never consumed.
  Token Take()
  {
    Token token = Current();
    if (token.type != TOK_EOF)
      ++m_pos;
    return token;
  }

  static ParseResult Error(const Token& token, std::string message)
  {
    return ParseResult::MakeErrorResult(token, std::move(message));
  }

  // Parses operands joined by operators that bind tighter than `limit` (or as tight, for the
  // right-grouping ones).
  ParseResult ParseExpressionUpTo(int limit)
  {
    ParseResult first = ParsePrimary(Take());
    if (first.status == ParseStatus::SyntaxError)
      return first;
    std::unique_ptr<Expression> tree = std::move(first.expr);

    for (;;)
    {
      const Token next = Current();
      const int strength = BindingStrength(next.type);

      if (next.IsBinaryOperator() &&
          (strength < limit || (next.type == TOK_ASSIGN && strength == limit)))
      {
        Take();
        ParseResult right = ParseExpressionUpTo(strength);
        if (right.status == ParseStatus::SyntaxError)
          return right;

        if (next.type == TOK_ASSIGN && !next.data.empty())
        {
          tree = std::make_unique<CompoundAssignmentExpression>(
              OperatorForCharacter(next.data.front()), std::move(tree), std::move(right.expr));
        }
        else
        {
          tree = std::make_unique<BinaryExpression>(next.type, std::move(tree),
                                                    std::move(right.expr));
        }
        continue;
      }

      if (next.type == TOK_QUESTION && strength <= limit)
      {
        Take();
        ParseResult when_true = ParseExpressionUpTo(strength);
        if (when_true.status != ParseStatus::Successful)
          return when_true;

        const Token separator = Take();
        if (separator.type != TOK_COLON)
          return Error(separator, Common::GetStringT("Expected colon."));

        ParseResult when_false = ParseExpressionUpTo(strength);
        if (when_false.status != ParseStatus::Successful)
          return when_false;

        // "c ? a : b" is the same as if(c, a, b).
        std::unique_ptr<FunctionExpression> conditional = MakeFunctionExpression("if");
        std::vector<std::unique_ptr<Expression>> operands;
        operands.push_back(std::move(tree));
        operands.push_back(std::move(when_true.expr));
        operands.push_back(std::move(when_false.expr));
        conditional->SetArguments(std::move(operands));
        tree = std::move(conditional);
        continue;
      }

      return ParseResult::MakeSuccessfulResult(std::move(tree));
    }
  }

  ParseResult ParsePrimary(const Token& token)
  {
    switch (token.type)
    {
    case TOK_BAREWORD:
      if (std::unique_ptr<FunctionExpression> function = MakeFunctionExpression(token.data))
        return ParseCall(token.data, std::move(function), token);
      // Not a function name: the word names a control of the default device.
      return MakeControl(token.data);

    case TOK_CONTROL:
      return MakeControl(token.data);

    case TOK_LITERAL:
    {
      ControlState value{};
      if (!TryParse(token.data, &value))
        return Error(token, Common::GetStringT("Invalid literal."));
      return ParseResult::MakeSuccessfulResult(std::make_unique<LiteralExpression>(value));
    }

    case TOK_VARIABLE:
      if (token.data.empty())
        return Error(token, Common::GetStringT("Expected variable name."));
      return ParseResult::MakeSuccessfulResult(std::make_unique<VariableExpression>(token.data));

    case TOK_LPAREN:
    {
      ParseResult inner = ParseExpressionUpTo(LOOSEST);
      if (inner.status != ParseStatus::Successful)
        return inner;
      const Token closing = Take();
      if (closing.type != TOK_RPAREN)
        return Error(closing, Common::GetStringT("Expected closing paren."));
      return inner;
    }

    case TOK_HOTKEY:
      return ParseHotkey();

    // Prefix operators are spelled as one-argument functions.
    case TOK_NOT:
      return ParseCall("not", MakeFunctionExpression("not"), token);
    case TOK_SUB:
      return ParseCall("minus", MakeFunctionExpression("minus"), token);
    case TOK_ADD:
      return ParseCall("plus", MakeFunctionExpression("plus"), token);

    default:
      return Error(token, Common::GetStringT("Expected start of expression."));
    }
  }

  static ParseResult MakeControl(const std::string& name)
  {
    ControlQualifier qualifier;
    qualifier.FromString(name);
    return ParseResult::MakeSuccessfulResult(std::make_unique<ControlExpression>(qualifier));
  }

  // Arguments are either a parenthesised, comma-separated list or a single primary expression
  // without parentheses ("!A", "-x").
  ParseResult ParseCall(const std::string& name, std::unique_ptr<FunctionExpression> function,
                        const Token& name_token)
  {
    std::vector<std::unique_ptr<Expression>> arguments;

    if (Current().type != TOK_LPAREN)
    {
      ParseResult only = ParsePrimary(Take());
      if (only.status != ParseStatus::Successful)
        return only;
      arguments.push_back(std::move(only.expr));
    }
    else
    {
      Take();
      if (Current().type == TOK_RPAREN)
      {
        Take();
      }
      else
      {
        for (;;)
        {
          ParseResult argument = ParseExpressionUpTo(BindingStrength(TOK_COMMA));
          if (argument.status != ParseStatus::Successful)
            return argument;
          arguments.push_back(std::move(argument.expr));

          const Token after = Take();
          if (after.type == TOK_RPAREN)
            break;
          if (after.type != TOK_COMMA)
            return Error(after, Common::GetStringT("Expected closing paren."));
        }
      }
    }

    function->SetArguments(std::move(arguments));
    const FunctionExpression::ArgumentValidation check = function->ValidateArguments();
    if (const auto* expected = std::get_if<FunctionExpression::ExpectedArguments>(&check))
    {
      const std::string signature = name + '(' + expected->text + ')';
      return ParseResult::MakeErrorResult(
          name_token, Common::FmtFormatT("Expected arguments: {0}", signature));
    }
    return ParseResult::MakeSuccessfulResult(std::move(function));
  }

  ParseResult ParseHotkey()
  {
    const Token opening = Take();
    if (opening.type != TOK_LPAREN)
      return Error(opening, Common::GetStringT("Expected opening paren."));

    HotkeyExpression::InputList inputs;
    for (;;)
    {
      const Token name = Take();
      if (name.type != TOK_CONTROL && name.type != TOK_BAREWORD)
        return Error(name, Common::GetStringT("Expected name of input."));

      ControlQualifier qualifier;
      qualifier.FromString(name.data);
      inputs.push_back(std::make_unique<ControlExpression>(std::move(qualifier)));

      const Token after = Take();
      if (after.type == TOK_RPAREN)
        break;
      if (after.type != TOK_ADD)
        return Error(after, Common::GetStringT("Expected + or closing paren."));
    }

    return ParseResult::MakeSuccessfulResult(std::make_unique<HotkeyExpression>(std::move(inputs)));
  }

  std::vector<Token>::const_iterator m_pos;
};
}  // namespace

ParseResult ParseTokens(const std::vector<Token>& tokens)
{
  return Parser(tokens).ParseAll();
}

ParseResult ParseExpression(const std::string& expr)
{
  if (StripWhitespace(expr).empty())
    return ParseResult::MakeEmptyResult();

  // The whole text as one control name (no device prefix), for old-style mappings.
  ControlQualifier whole_text;
  whole_text.control_name = expr;
  auto as_control_name = std::make_unique<ControlExpression>(std::move(whole_text));

  std::vector<Token> tokens;
  ParseResult result = Lexer(expr).Tokenize(tokens) == ParseStatus::Successful ?
                           ParseTokens(tokens) :
                           ParseResult::MakeErrorResult(Token(TOK_INVALID),
                                                        Common::GetStringT("Tokenizing failed."));

  if (result.status == ParseStatus::Successful)
  {
    result.expr =
        std::make_unique<CoalesceExpression>(std::move(as_control_name), std::move(result.expr));
  }
  else
  {
    // A syntax error still leaves the control-name reading usable.
    result.expr = std::move(as_control_name);
  }
  return result;
}

}  // namespace ciface::ExpressionParser
