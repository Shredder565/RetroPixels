// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/CWDemangler.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

#include "Common/StringUtil.h"

namespace CWDemangler
{
namespace
{
// Mangled operator names (the Itanium-style two/three letter codes CodeWarrior also uses).
std::optional<std::string_view> OperatorName(std::string_view code)
{
  struct Entry
  {
    std::string_view code;
    std::string_view name;
  };
  static constexpr Entry table[] = {
      {"nw", "operator new"},  {"nwa", "operator new[]"}, {"dl", "operator delete"},
      {"dla", "operator delete[]"}, {"pl", "operator+"},   {"mi", "operator-"},
      {"ml", "operator*"},     {"dv", "operator/"},       {"md", "operator%"},
      {"er", "operator^"},     {"ad", "operator&"},       {"or", "operator|"},
      {"co", "operator~"},     {"nt", "operator!"},       {"as", "operator="},
      {"lt", "operator<"},     {"gt", "operator>"},       {"apl", "operator+="},
      {"ami", "operator-="},   {"amu", "operator*="},     {"adv", "operator/="},
      {"amd", "operator%="},   {"aer", "operator^="},     {"aad", "operator&="},
      {"aor", "operator|="},   {"ls", "operator<<"},      {"rs", "operator>>"},
      {"ars", "operator>>="},  {"als", "operator<<="},    {"eq", "operator=="},
      {"ne", "operator!="},    {"le", "operator<="},      {"ge", "operator>="},
      {"aa", "operator&&"},    {"oo", "operator||"},      {"pp", "operator++"},
      {"mm", "operator--"},    {"cm", "operator,"},       {"rm", "operator->*"},
      {"rf", "operator->"},    {"cl", "operator()"},      {"vc", "operator[]"},
      {"vt", "__vtable"},
  };
  for (const Entry& e : table)
  {
    if (e.code == code)
      return e.name;
  }
  return std::nullopt;
}

// Single-letter builtin type codes.
std::optional<std::string_view> BuiltinType(char code)
{
  switch (code)
  {
  case 'i':
    return "int";
  case 'b':
    return "bool";
  case 'c':
    return "char";
  case 's':
    return "short";
  case 'l':
    return "long";
  case 'x':
    return "long long";
  case 'f':
    return "float";
  case 'd':
    return "double";
  case 'w':
    return "wchar_t";
  case 'v':
    return "void";
  case 'e':
    return "...";
  case 'r':
    return "long double";
  case 'D':
    return "short double";
  default:
    return std::nullopt;
  }
}

struct Number
{
  std::size_t value;
  std::string_view rest;
};

// Leading decimal number.
std::optional<Number> ReadNumber(std::string_view str)
{
  if (str.empty())
    return std::nullopt;
  std::size_t value = 0;
  const char* const end = str.data() + str.size();
  const auto [ptr, ec] = std::from_chars(str.data(), end, value);
  if (ec != std::errc{})
    return std::nullopt;
  return Number{value, std::string_view(ptr, static_cast<std::size_t>(end - ptr))};
}

struct Qualifiers
{
  std::string pre;   // cv/sign words printed before the type
  std::string post;  // pointer/reference declarators printed after it
  std::string_view rest;
};

// Reads the run of P, R, C, V, U, S codes in front of a type. A pointer or reference that
// follows cv words binds them: "PC" reads as "* const" (a const pointer).
Qualifiers ReadQualifiers(std::string_view str)
{
  Qualifiers q;
  std::size_t used = 0;
  for (; used < str.size(); ++used)
  {
    const char c = str[used];
    if (c == 'P' || c == 'R')
    {
      const char* symbol = c == 'P' ? "*" : "&";
      if (q.pre.empty())
        q.post.insert(0, symbol);
      else
        q.post.insert(0, std::string(symbol) + " " + std::string(StripTrailingWhitespace(q.pre)));
      q.pre.clear();
    }
    else if (c == 'C')
      q.pre += "const ";
    else if (c == 'V')
      q.pre += "volatile ";
    else if (c == 'U')
      q.pre += "unsigned ";
    else if (c == 'S')
      q.pre += "signed ";
    else
      break;
  }
  q.post = std::string(StripTrailingWhitespace(q.post));
  q.rest = str.substr(used);
  return q;
}

// Position of the "__" that separates a symbol's name from its signature: the first one that
// is not inside a template argument list (and, for "__op" conversion operators, not inside the
// target type).
std::optional<std::size_t> FindSplit(std::string_view str, bool special, DemangleOptions options)
{
  std::size_t from = 0;
  if (special && str.starts_with("op"))
  {
    const auto type = demangle_arg(str.substr(2), options);
    if (!type)
      return std::nullopt;
    from = str.size() - type->rest.size();
  }

  int depth = 0;
  for (std::size_t i = from; i < str.size(); ++i)
  {
    const char c = str[i];
    if (c == '<')
      ++depth;
    else if (c == '>')
      --depth;
    else if (c == '_' && depth == 0 && i + 1 < str.size() && str[i + 1] == '_')
      return i;
  }
  return std::nullopt;
}

DemangleArgResult Done(std::string pre, std::string post, std::string_view rest)
{
  return DemangleArgResult{std::move(pre), std::move(post), rest};
}

// Function type after 'F' (or member function type after "M<class>F"). `cv` and `declarator`
// are what was read in front of it; `member_of` is the printed class for members.
std::optional<DemangleArgResult> ReadFunctionType(std::string_view str, std::string cv,
                                                  std::string declarator, bool is_member,
                                                  DemangleOptions options)
{
  bool const_member = false;
  if (is_member)
  {
    // The hidden 'this' and the second implicit parameter of member function pointers.
    if (str.starts_with("PCvPCv"))
    {
      const_member = true;
      str.remove_prefix(6);
    }
    else if (str.starts_with("PCvPv"))
    {
      str.remove_prefix(5);
    }
    else
    {
      return std::nullopt;
    }
  }
  else if (declarator.starts_with('*'))
  {
    // Pointer to function: the '*' moves inside the parentheses.
    declarator = std::string(StripLeadingWhitespace(std::string_view(declarator).substr(1)));
    cv = "*" + cv;
  }
  else
  {
    return std::nullopt;
  }

  const auto params = demangle_function_args(str, options);
  if (!params || !params->rest.starts_with('_'))
    return std::nullopt;

  const auto ret = demangle_arg(params->rest.substr(1), options);
  if (!ret)
    return std::nullopt;

  std::string pre = ret->arg_pre + " (" + cv + declarator;
  std::string post = ")(" + params->args + ")" + (const_member ? " const" : "") + ret->arg_post;
  return Done(std::move(pre), std::move(post), ret->rest);
}
}  // namespace

std::optional<DemangleTemplateArgsResult> demangle_template_args(std::string_view str,
                                                                 DemangleOptions options)
{
  const std::size_t open = str.find('<');
  if (open == std::string_view::npos)
    return DemangleTemplateArgsResult{str, ""};

  const std::size_t close = str.rfind('>');
  if (close == std::string_view::npos || close < open)
    return std::nullopt;

  std::string printed = "<";
  std::string_view inner = str.substr(open + 1, close - open - 1);
  while (!inner.empty())
  {
    const auto arg = demangle_arg(inner, options);
    if (!arg)
      return std::nullopt;
    printed += arg->arg_pre;
    printed += arg->arg_post;
    if (arg->rest.empty())
      break;
    // Skip the separator.
    printed += ", ";
    inner = arg->rest.substr(1);
  }
  printed += ">";
  return DemangleTemplateArgsResult{str.substr(0, open), std::move(printed)};
}

std::optional<DemangleNameResult> demangle_name(std::string_view str, DemangleOptions options)
{
  const auto length = ReadNumber(str);
  if (!length || length->rest.size() < length->value)
    return std::nullopt;

  const std::string_view mangled = length->rest.substr(0, length->value);
  const auto templ = demangle_template_args(mangled, options);
  if (!templ)
    return std::nullopt;

  std::string base(templ->name);
  std::string full = base + templ->args;
  return DemangleNameResult{std::move(base), std::move(full),
                            length->rest.substr(length->value)};
}

std::optional<DemangleNameResult> demangle_qualified_name(std::string_view str,
                                                          DemangleOptions options)
{
  if (!str.starts_with('Q'))
    return demangle_name(str, options);

  // The nesting depth is one digit (the compiler keeps at most nine levels).
  if (str.size() < 3 || str[1] < '0' || str[1] > '9')
    return std::nullopt;
  const int levels = str[1] - '0';
  str.remove_prefix(2);

  DemangleNameResult result;
  for (int level = 0; level < levels; ++level)
  {
    auto part = demangle_name(str, options);
    if (!part)
      return std::nullopt;
    if (level != 0)
      result.full += "::";
    result.full += part->full;
    result.class_name = std::move(part->class_name);
    str = part->rest;
  }
  result.rest = str;
  return result;
}

std::optional<DemangleArgResult> demangle_arg(std::string_view str, DemangleOptions options)
{
  // Negative integer template argument.
  if (str.starts_with('-'))
  {
    const auto number = ReadNumber(str.substr(1));
    if (!number)
      return std::nullopt;
    return Done("-" + std::to_string(number->value), "", number->rest);
  }

  Qualifiers q = ReadQualifiers(str);
  std::string_view body = q.rest;

  if (!body.empty() && std::isdigit(static_cast<unsigned char>(body[0])))
  {
    const auto number = ReadNumber(body);
    if (!number)
      return std::nullopt;

    // A number alone (up to ',' or the end) is an integer template argument, or with the
    // Metrowerks extensions one of the extension types.
    if (number->rest.empty() || number->rest.starts_with(','))
    {
      if (options.mw_extensions && (number->value == 1 || number->value == 2))
      {
        const char* ext = number->value == 1 ? "__int128" : "__vec2x32float__";
        return Done(q.pre + ext, q.post, number->rest);
      }
      return Done(q.pre + std::to_string(number->value) + q.post, "", number->rest);
    }

    // Otherwise it is the length prefix of a class name.
    const auto name = demangle_name(body, options);
    if (!name)
      return std::nullopt;
    return Done(q.pre + name->full + q.post, "", name->rest);
  }

  if (body.starts_with('Q'))
  {
    const auto name = demangle_qualified_name(body, options);
    if (!name)
      return std::nullopt;
    return Done(q.pre + name->full + q.post, "", name->rest);
  }

  if (body.starts_with('M'))
  {
    // Pointer to member function: M<class>F...
    const auto owner = demangle_qualified_name(body.substr(1), options);
    if (!owner || !owner->rest.starts_with('F'))
      return std::nullopt;
    return ReadFunctionType(owner->rest.substr(1), owner->full + "::*" + q.pre, q.post, true,
                            options);
  }

  if (body.starts_with('F'))
    return ReadFunctionType(body.substr(1), q.pre, q.post, false, options);

  if (body.starts_with('A'))
  {
    // Array: A<count>_<element type>
    const auto count = ReadNumber(body.substr(1));
    if (!count || !count->rest.starts_with('_'))
      return std::nullopt;
    const auto element = demangle_arg(count->rest.substr(1), options);
    if (!element)
      return std::nullopt;
    const std::string declarator = q.post.empty() ? std::string() : "(" + q.post + ")";
    return Done(q.pre + element->arg_pre + declarator,
                "[" + std::to_string(count->value) + "]" + element->arg_post, element->rest);
  }

  if (body.empty())
    return std::nullopt;

  if (body[0] == '_')
    return Done(q.pre, "", body);

  const auto builtin = BuiltinType(body[0]);
  if (!builtin)
    return std::nullopt;
  return Done(q.pre + std::string(*builtin) + q.post, "", body.substr(1));
}

std::optional<DemangleFunctionArgsResult> demangle_function_args(std::string_view str,
                                                                 DemangleOptions options)
{
  std::string printed;
  while (!str.empty())
  {
    // Separators only go between non-empty pieces.
    if (!printed.empty())
      printed += ", ";

    const auto arg = demangle_arg(str, options);
    if (!arg)
      return std::nullopt;
    printed += arg->arg_pre;
    printed += arg->arg_post;
    str = arg->rest;

    if (str.starts_with('_') || str.starts_with(','))
      break;
  }
  return DemangleFunctionArgsResult{std::move(printed), str};
}

std::optional<std::string> demangle_special_function(std::string_view str,
                                                     std::string_view class_name,
                                                     DemangleOptions options)
{
  // Conversion operator: "op" followed by the target type.
  if (str.starts_with("op"))
  {
    const auto type = demangle_arg(str.substr(2), options);
    if (!type)
      return std::nullopt;
    return "operator " + type->arg_pre + type->arg_post;
  }

  const auto templ = demangle_template_args(str, options);
  if (!templ)
    return std::nullopt;

  if (templ->name == "dt")
    return "~" + std::string(class_name) + templ->args;
  if (templ->name == "ct")
    return std::string(class_name) + templ->args;

  // Operator codes are only recognised on their own: the lookup uses the whole input (up to
  // a NUL), so a templated operator falls through to the generic form below.
  const std::string_view whole = str.substr(0, str.find('\0'));
  if (const auto op = OperatorName(whole))
    return std::string(*op) + templ->args;

  return "__" + std::string(templ->name) + templ->args;
}

std::optional<std::string> demangle(std::string_view str, DemangleOptions options)
{
  if (std::ranges::any_of(str, [](char c) { return static_cast<unsigned char>(c) > 127; }))
    return std::nullopt;

  std::string static_var;

  // Wii compilers: "@GUARD@<symbol>@<var>" / "@LOCAL@<symbol>@<var>" for function statics.
  const bool is_guard = str.starts_with("@GUARD@");
  if (is_guard || str.starts_with("@LOCAL@"))
  {
    str.remove_prefix(7);
    const std::size_t at = str.rfind('@');
    if (at == std::string_view::npos)
      return std::nullopt;
    static_var = std::string(str.substr(at + 1));
    if (is_guard)
      static_var += " guard";
    str = str.substr(0, at);
  }

  const bool special = str.starts_with("__");
  if (special)
    str.remove_prefix(2);

  const auto split_at = FindSplit(str, special, options);
  if (!split_at)
    return std::nullopt;
  std::size_t split = *split_at;
  // A name may itself end in underscores: the separator is the last "__" of the run.
  while (split + 2 < str.size() && str[split + 2] == '_')
    ++split;

  const std::string_view base = str.substr(0, split);
  std::string_view signature = str.substr(split);

  std::string fn_name;
  if (!special)
  {
    const auto templ = demangle_template_args(base, options);
    if (!templ)
      return std::nullopt;
    fn_name = std::string(templ->name) + templ->args;
  }
  else if (base == "init")
  {
    // "__init__<name>__<signature>": the static initialiser keeps "init__<name>".
    const std::size_t next = signature.substr(2).find("__");
    if (next == std::string_view::npos)
      return std::nullopt;
    fn_name = std::string(str.substr(0, next + 6));
    signature.remove_prefix(next + 2);
  }
  else
  {
    fn_name = std::string(base);
  }

  // GameCube compilers: "<var>$localstatic<n>$<function>" for function statics.
  if (const std::size_t dollar = fn_name.find('$'); dollar != std::string::npos)
  {
    const std::size_t second = fn_name.find('$', dollar + 1);
    if (second == std::string::npos)
      return std::nullopt;
    const std::string var = fn_name.substr(0, dollar);
    const std::string kind = fn_name.substr(dollar + 1, second - dollar - 1);
    if (!kind.starts_with("localstatic"))
      return std::nullopt;
    // The guard of an initialised static carries no variable name.
    static_var = var == "init" ? kind + " guard" : var;
    fn_name = fn_name.substr(second + 1);
  }

  std::string_view rest = signature.substr(2);

  std::string scope;
  std::string class_name;
  if (!rest.starts_with('F'))
  {
    auto owner = demangle_qualified_name(rest, options);
    if (!owner)
      return std::nullopt;
    class_name = std::move(owner->class_name);
    scope = std::move(owner->full);
    rest = owner->rest;
  }

  if (special)
  {
    auto name = demangle_special_function(fn_name, class_name, options);
    if (!name)
      return std::nullopt;
    fn_name = std::move(*name);
  }

  const bool const_method = rest.starts_with('C');
  if (const_method)
    rest.remove_prefix(1);

  if (rest.starts_with('F'))
  {
    const auto params = demangle_function_args(rest.substr(1), options);
    if (!params)
      return std::nullopt;
    if (options.omit_empty_parameters && params->args == "void")
      fn_name += "()";
    else
      fn_name += "(" + params->args + ")";
    rest = params->rest;
  }

  std::string ret_pre;
  std::string ret_post;
  if (rest.starts_with('_'))
  {
    const auto ret = demangle_arg(rest.substr(1), options);
    if (!ret)
      return std::nullopt;
    ret_pre = ret->arg_pre;
    ret_post = ret->arg_post;
    rest = ret->rest;
  }

  if (!rest.empty())
    return std::nullopt;

  if (const_method)
    fn_name += " const";
  if (!scope.empty())
    fn_name = scope + "::" + fn_name;
  if (!ret_pre.empty())
    fn_name = ret_pre + " " + fn_name + ret_post;
  if (!static_var.empty())
    fn_name += "::" + static_var;
  return fn_name;
}
}  // namespace CWDemangler
