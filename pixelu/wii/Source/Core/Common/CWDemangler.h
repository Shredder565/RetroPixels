// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

// Demangler for symbol names produced by Metrowerks CodeWarrior (the GameCube/Wii SDK
// compiler), e.g. "__ct__Q24nw4r2ut10LinkListFv" -> "nw4r::ut::LinkList::LinkList()".
// Each parse function consumes a prefix of its input and returns the text it produced together
// with the unconsumed rest, or std::nullopt when the input does not follow the scheme.

namespace CWDemangler
{
struct DemangleOptions
{
  // Print "f(void)" as "f()".
  bool omit_empty_parameters = true;
  // Read the Metrowerks extension type codes 1 and 2 (__int128, __vec2x32float__). Off by
  // default: those codes clash with numeric template arguments.
  bool mw_extensions = false;
};

struct DemangleTemplateArgsResult
{
  std::string_view name;
  std::string args;
};

struct DemangleNameResult
{
  std::string class_name;
  std::string full;
  std::string_view rest;
};

// A type is printed as arg_pre + <declarator> + arg_post; both are needed for function and
// array types, which wrap around the name.
struct DemangleArgResult
{
  std::string arg_pre;
  std::string arg_post;
  std::string_view rest;
};

struct DemangleFunctionArgsResult
{
  std::string args;
  std::string_view rest;
};

// "name<args>" -> name and "<printed args>". A string without '<' has empty args.
std::optional<DemangleTemplateArgsResult> demangle_template_args(std::string_view str,
                                                                 DemangleOptions options);
// "<length><name>"
std::optional<DemangleNameResult> demangle_name(std::string_view str, DemangleOptions options);
// "Q<count><name>..." or a single "<length><name>"
std::optional<DemangleNameResult> demangle_qualified_name(std::string_view str,
                                                          DemangleOptions options);
// One type (or numeric template argument).
std::optional<DemangleArgResult> demangle_arg(std::string_view str, DemangleOptions options);
// A parameter list, up to a '_' (return type follows) or ',' (end of template argument).
std::optional<DemangleFunctionArgsResult> demangle_function_args(std::string_view str,
                                                                 DemangleOptions options);
// Constructors, destructors, operators and other "__"-prefixed member names.
std::optional<std::string> demangle_special_function(std::string_view str,
                                                     std::string_view class_name,
                                                     DemangleOptions options);
// A whole symbol; std::nullopt when it is not a mangled name.
std::optional<std::string> demangle(std::string_view str, DemangleOptions options);
}  // namespace CWDemangler
