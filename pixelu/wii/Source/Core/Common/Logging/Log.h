// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>

#include <fmt/format.h>

#include "Common/FormatUtil.h"

namespace Common::Log
{
// Log channels. Each one can be switched on and off in the log configuration; the names the
// configuration uses are set up in LogManager.cpp.
enum class LogType : int
{
  ACHIEVEMENTS,
  ACTIONREPLAY,
  AUDIO,
  AUDIO_INTERFACE,
  BOOT,
  COMMANDPROCESSOR,
  COMMON,
  CONSOLE,
  CONTROLLERINTERFACE,
  CORE,
  DISCIO,
  DSPHLE,
  DSPLLE,
  DSP_MAIL,
  DSPINTERFACE,
  DVDINTERFACE,
  AMMEDIABOARD,
  AMMEDIABOARD_NET,
  DYNA_REC,
  EXPANSIONINTERFACE,
  FILEMON,
  FRAMEDUMP,
  GDB_STUB,
  GPFIFO,
  HOST_GPU,
  HSP,
  IOS,
  IOS_DI,
  IOS_ES,
  IOS_FS,
  IOS_NET,
  IOS_SD,
  IOS_SSL,
  IOS_STM,
  IOS_USB,
  IOS_WC24,
  IOS_WFS,
  IOS_WIIMOTE,
  MASTER_LOG,
  MEMMAP,
  MEMCARD_MANAGER,
  NETPLAY,
  OSHLE,
  OSREPORT,
  OSREPORT_HLE,
  PIXELENGINE,
  PROCESSORINTERFACE,
  POWERPC,
  SERIALINTERFACE,
  SERIALINTERFACE_AMBB,
  SERIALINTERFACE_CARD,
  SERIALINTERFACE_JVSIO,
  SP1,
  SYMBOLS,
  VIDEO,
  VIDEOINTERFACE,
  WII_IPC,
  WIIMOTE,

  NUMBER_OF_LOGS  // keep last
};

constexpr LogType LAST_LOG_TYPE = LogType(int(LogType::NUMBER_OF_LOGS) - 1);

// Severity, from most to least important. The numbers are stored in the logger configuration.
enum class LogLevel : int
{
  LNOTICE = 1,   // important messages that are not errors (startup, OSReport)
  LERROR = 2,    // errors
  LWARNING = 3,  // suspicious situations
  LINFO = 4,     // general information
  LDEBUG = 5,    // detailed tracing, may be slow
};

// Messages above this level are compiled out entirely.
#if defined(_DEBUG) || defined(DEBUGFAST)
constexpr auto MAX_EFFECTIVE_LOGLEVEL = LogLevel::LDEBUG;
#else
constexpr auto MAX_EFFECTIVE_LOGLEVEL = LogLevel::LINFO;
#endif

// One letter per level, indexed by the level's number.
static const char LOG_LEVEL_TO_CHAR[7] = "-NEWID";

void GenericLogFmtImpl(LogLevel level, LogType type, const char* file, int line,
                       fmt::string_view format, const fmt::format_args& args);

template <std::size_t NumFields, typename S, typename... Args>
void GenericLogFmt(LogLevel level, LogType type, const char* file, int line, const S& format,
                   const Args&... args)
{
  static_assert(NumFields == sizeof...(Args),
                "The format string's replacement fields and the arguments don't match up");
#if FMT_VERSION >= 110000 && FMT_VERSION < 120200
  // These fmt versions only convert FMT_STRING objects to a string view through format_string.
  const fmt::format_string<Args...> checked_format(format);
  GenericLogFmtImpl(level, type, file, line, checked_format.get(), fmt::make_format_args(args...));
#else
  GenericLogFmtImpl(level, type, file, line, format, fmt::make_format_args(args...));
#endif
}
}  // namespace Common::Log

// Logging macros: <LEVEL>_LOG_FMT(LOG_TYPE, "format {}", args...).

#define GENERIC_LOG_FMT(t, v, format, ...)                                                         \
  do                                                                                               \
  {                                                                                                \
    if ((v) <= Common::Log::MAX_EFFECTIVE_LOGLEVEL)                                                \
    {                                                                                              \
      constexpr std::size_t GENERIC_LOG_FMT_FIELDS = Common::CountFmtReplacementFields(format);    \
      Common::Log::GenericLogFmt<GENERIC_LOG_FMT_FIELDS>(                                          \
          v, t, __FILE__, __LINE__, FMT_STRING(format) __VA_OPT__(, ) __VA_ARGS__);                \
    }                                                                                              \
  } while (0)

#define ERROR_LOG_FMT(t, ...)                                                                      \
  GENERIC_LOG_FMT(Common::Log::LogType::t, Common::Log::LogLevel::LERROR __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG_FMT(t, ...)                                                                       \
  GENERIC_LOG_FMT(Common::Log::LogType::t,                                                         \
                  Common::Log::LogLevel::LWARNING __VA_OPT__(, ) __VA_ARGS__)
#define NOTICE_LOG_FMT(t, ...)                                                                     \
  GENERIC_LOG_FMT(Common::Log::LogType::t,                                                         \
                  Common::Log::LogLevel::LNOTICE __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG_FMT(t, ...)                                                                       \
  GENERIC_LOG_FMT(Common::Log::LogType::t, Common::Log::LogLevel::LINFO __VA_OPT__(, ) __VA_ARGS__)
#define DEBUG_LOG_FMT(t, ...)                                                                      \
  GENERIC_LOG_FMT(Common::Log::LogType::t, Common::Log::LogLevel::LDEBUG __VA_OPT__(, ) __VA_ARGS__)
