// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/MsgHandler.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>

#include "Common/StringUtil.h"
#endif

#include "Common/Logging/Log.h"

namespace Common
{
namespace
{
// Used until the UI installs its own handler.
bool FallbackAlertHandler(const char* caption, const char* text, bool yes_no, MsgType style)
{
#ifdef _WIN32
  UINT flags = yes_no ? MB_YESNO : MB_OK;
  switch (style)
  {
  case MsgType::Question:
    flags |= MB_ICONQUESTION;
    break;
  case MsgType::Warning:
    flags |= MB_ICONWARNING;
    break;
  default:
    flags |= MB_ICONINFORMATION;
    break;
  }
  const std::wstring wide_text = UTF8ToWString(text);
  const std::wstring wide_caption = UTF8ToWString(caption);
  return MessageBoxW(nullptr, wide_text.c_str(), wide_caption.c_str(), flags) == IDYES;
#else
  (void)caption;
  (void)style;
  (void)yes_no;
  std::fprintf(stderr, "%s\n", text);
  return false;  // nobody can answer, so every question gets "no"
#endif
}

std::string IdentityTranslator(const char* text)
{
  return text;
}

MsgAlertHandler s_alert_handler = FallbackAlertHandler;
StringTranslator s_translator = IdentityTranslator;
bool s_alerts_enabled = true;
bool s_abort_on_panic = false;

// The caption strings are translation keys; they must stay exactly as they are.
const char* CaptionFor(MsgType style)
{
  static const std::string information = GetStringT("Information");
  static const std::string question = GetStringT("Question");
  static const std::string warning = GetStringT("Warning");
  static const std::string critical = GetStringT("Critical");

  switch (style)
  {
  case MsgType::Information:
    return information.c_str();
  case MsgType::Question:
    return question.c_str();
  case MsgType::Warning:
    return warning.c_str();
  case MsgType::Critical:
    return critical.c_str();
  }
  return "Unhandled caption";
}

Log::LogLevel LevelFor(MsgType style)
{
  if (style == MsgType::Critical)
    return Log::LogLevel::LERROR;
  if (style == MsgType::Warning)
    return Log::LogLevel::LWARNING;
  return Log::LogLevel::LINFO;
}

bool PresentAlert(const std::string& text, bool yes_no, Log::LogType log_type, MsgType style,
                  const char* file, int line)
{
  const char* caption = CaptionFor(style);

  // Log with the caller's location rather than this file's.
  Log::GenericLogFmt<2>(LevelFor(style), log_type, file, line, FMT_STRING("{}: {}"), caption,
                        text);

  if (s_abort_on_panic && style == MsgType::Warning)
    std::abort();

  // Disabling alerts only silences information and warnings; questions and critical errors
  // are always shown.
  const bool must_show = style == MsgType::Question || style == MsgType::Critical;
  if (s_alert_handler == nullptr || !(s_alerts_enabled || must_show))
    return true;

  return s_alert_handler(caption, text.c_str(), yes_no, style);
}
}  // namespace

void RegisterMsgAlertHandler(MsgAlertHandler handler)
{
  s_alert_handler = handler;
}

void RegisterStringTranslator(StringTranslator translator)
{
  s_translator = translator;
}

void SetEnableAlert(bool enable)
{
  s_alerts_enabled = enable;
}

void SetAbortOnPanicAlert(bool should_abort)
{
  s_abort_on_panic = should_abort;
}

std::string GetStringT(const char* string)
{
  return s_translator(string);
}

bool MsgAlertFmtImpl(bool yes_no, MsgType style, Common::Log::LogType log_type, const char* file,
                     int line, fmt::string_view format, const fmt::format_args& args)
{
  const std::string text = fmt::vformat(format, args);
  return PresentAlert(text, yes_no, log_type, style, file, line);
}
}  // namespace Common
