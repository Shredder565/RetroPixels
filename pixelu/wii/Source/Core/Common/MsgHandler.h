// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <type_traits>
#include <utility>

#include <fmt/format.h>

#include "Common/Logging/Log.h"

// Message boxes ("alerts") shown to the user, and the translation hook for UI strings.
namespace Common
{
enum class MsgType
{
  Information,
  Question,
  Warning,
  Critical
};

// Shows a message box; returns the answer for yes/no questions (true for OK boxes).
using MsgAlertHandler = bool (*)(const char* caption, const char* text, bool yes_no, MsgType style);
// Returns the translation of an English UI string.
using StringTranslator = std::string (*)(const char* text);

void RegisterMsgAlertHandler(MsgAlertHandler handler);
void RegisterStringTranslator(StringTranslator translator);

[[nodiscard]] std::string GetStringT(const char* string);

bool MsgAlertFmtImpl(bool yes_no, MsgType style, Common::Log::LogType log_type, const char* file,
                     int line, fmt::string_view format, const fmt::format_args& args);

template <std::size_t NumFields, typename S, typename... Args>
bool MsgAlertFmt(bool yes_no, MsgType style, Common::Log::LogType log_type, const char* file,
                 int line, const S& format, const Args&... args)
{
  static_assert(NumFields == sizeof...(Args),
                "The format string's replacement fields and the arguments don't match up");
#if FMT_VERSION >= 110000 && FMT_VERSION < 120200
  const fmt::format_string<Args...> checked_format(format);
  return MsgAlertFmtImpl(yes_no, style, log_type, file, line, checked_format.get(),
                         fmt::make_format_args(args...));
#else
  return MsgAlertFmtImpl(yes_no, style, log_type, file, line, format,
                         fmt::make_format_args(args...));
#endif
}

// `format` (the English text) is only checked at compile time; the alert is built from
// `translated_format`, which must take the same arguments.
template <std::size_t NumFields, bool has_non_positional_args, typename S, typename... Args>
bool MsgAlertFmtT(bool yes_no, MsgType style, Common::Log::LogType log_type, const char* file,
                  int line, const S& format, fmt::string_view translated_format,
                  const Args&... args)
{
  static_assert(!has_non_positional_args,
                "Translatable strings need numbered fields ({0}, {1:x}, ...) instead of {}");
  static_assert(NumFields == sizeof...(Args),
                "The format string's replacement fields and the arguments don't match up");
#if FMT_VERSION >= 110000 && FMT_VERSION < 120200
  static_assert(std::is_base_of_v<fmt::detail::compile_string, S>);
#endif
  return MsgAlertFmtImpl(yes_no, style, log_type, file, line, translated_format,
                         fmt::make_format_args(args...));
}

void SetEnableAlert(bool enable);
void SetAbortOnPanicAlert(bool should_abort);

// fmt::format with a translated format string.
template <typename... Args>
std::string FmtFormatT(const char* string, Args&&... args)
{
  const std::string translated = Common::GetStringT(string);
  return fmt::format(fmt::runtime(translated), std::forward<Args>(args)...);
}
}  // namespace Common

#define GenericAlertFmt(yes_no, style, log_type, format, ...)                                      \
  Common::MsgAlertFmt<Common::CountFmtReplacementFields(format)>(                                  \
      yes_no, style, Common::Log::LogType::log_type, __FILE__, __LINE__,                           \
      FMT_STRING(format) __VA_OPT__(, ) __VA_ARGS__)

#define GenericAlertFmtT(yes_no, style, log_type, format, ...)                                     \
  Common::MsgAlertFmtT<Common::CountFmtReplacementFields(format),                                  \
                       Common::ContainsNonPositionalArguments(format)>(                            \
      yes_no, style, Common::Log::LogType::log_type, __FILE__, __LINE__, FMT_STRING(format),       \
      Common::GetStringT(format) __VA_OPT__(, ) __VA_ARGS__)

// Untranslated alerts.
#define SuccessAlertFmt(format, ...)                                                               \
  GenericAlertFmt(false, Common::MsgType::Information, MASTER_LOG,                                 \
                  format __VA_OPT__(, ) __VA_ARGS__)
#define PanicAlertFmt(format, ...)                                                                 \
  GenericAlertFmt(false, Common::MsgType::Warning, MASTER_LOG, format __VA_OPT__(, ) __VA_ARGS__)
#define PanicYesNoFmt(format, ...)                                                                 \
  GenericAlertFmt(true, Common::MsgType::Warning, MASTER_LOG, format __VA_OPT__(, ) __VA_ARGS__)
#define AskYesNoFmt(format, ...)                                                                   \
  GenericAlertFmt(true, Common::MsgType::Question, MASTER_LOG, format __VA_OPT__(, ) __VA_ARGS__)
#define CriticalAlertFmt(format, ...)                                                              \
  GenericAlertFmt(false, Common::MsgType::Critical, MASTER_LOG, format __VA_OPT__(, ) __VA_ARGS__)

// Translated alerts (the format string is looked up with GetStringT).
#define SuccessAlertFmtT(format, ...)                                                              \
  GenericAlertFmtT(false, Common::MsgType::Information, MASTER_LOG,                                \
                   format __VA_OPT__(, ) __VA_ARGS__)
#define PanicAlertFmtT(format, ...)                                                                \
  GenericAlertFmtT(false, Common::MsgType::Warning, MASTER_LOG, format __VA_OPT__(, ) __VA_ARGS__)
#define PanicYesNoFmtT(format, ...)                                                                \
  GenericAlertFmtT(true, Common::MsgType::Warning, MASTER_LOG, format __VA_OPT__(, ) __VA_ARGS__)
#define AskYesNoFmtT(format, ...)                                                                  \
  GenericAlertFmtT(true, Common::MsgType::Question, MASTER_LOG, format __VA_OPT__(, ) __VA_ARGS__)
#define CriticalAlertFmtT(format, ...)                                                             \
  GenericAlertFmtT(false, Common::MsgType::Critical, MASTER_LOG, format __VA_OPT__(, ) __VA_ARGS__)

// Yes/no warning logged to a chosen channel; used by the assertion macros.
#define PanicYesNoFmtAssert(log_type, format, ...)                                                 \
  GenericAlertFmt(true, Common::MsgType::Warning, log_type, format __VA_OPT__(, ) __VA_ARGS__)
