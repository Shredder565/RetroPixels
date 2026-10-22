// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Common.h"
#include "Common/CommonFuncs.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

// Runtime checks. A failed check asks the user whether to carry on; answering "no" breaks into
// the debugger.
//
// ASSERT_MSG(log_type, condition, format, args...) puts a formatted explanation in the dialog.
// The DEBUG_ variants only check anything when debug-level logging is compiled in.

#define ASSERT_MSG(_t_, _a_, _fmt_, ...)                                                           \
  do                                                                                               \
  {                                                                                                \
    if (!(_a_)) [[unlikely]]                                                                       \
    {                                                                                              \
      if (!PanicYesNoFmtAssert(_t_,                                                                \
                               _fmt_ "\n\nFailed check: {}\nSource: {} line {}\nFunction: {}\n\n"  \
                                     "Do you want to continue anyway?",                            \
                               __VA_ARGS__ __VA_OPT__(, ) #_a_, __FILE__, __LINE__, __func__))     \
      {                                                                                            \
        Crash();                                                                                   \
      }                                                                                            \
    }                                                                                              \
  } while (0)

#define ASSERT(_a_)                                                                                \
  do                                                                                               \
  {                                                                                                \
    if (!(_a_)) [[unlikely]]                                                                       \
    {                                                                                              \
      if (!PanicYesNoFmt("Failed check: {}\nSource: {} line {}\nFunction: {}\n\n"                  \
                         "Do you want to continue anyway?",                                        \
                         #_a_, __FILE__, __LINE__, __func__))                                      \
      {                                                                                            \
        Crash();                                                                                   \
      }                                                                                            \
    }                                                                                              \
  } while (0)

#define DEBUG_ASSERT_MSG(_t_, _a_, _fmt_, ...)                                                     \
  do                                                                                               \
  {                                                                                                \
    if constexpr (Common::Log::MAX_EFFECTIVE_LOGLEVEL >= Common::Log::LogLevel::LDEBUG)            \
    {                                                                                              \
      ASSERT_MSG(_t_, _a_, _fmt_ __VA_OPT__(, ) __VA_ARGS__);                                      \
    }                                                                                              \
  } while (0)

#define DEBUG_ASSERT(_a_)                                                                          \
  do                                                                                               \
  {                                                                                                \
    if constexpr (Common::Log::MAX_EFFECTIVE_LOGLEVEL >= Common::Log::LogLevel::LDEBUG)            \
    {                                                                                              \
      ASSERT(_a_);                                                                                 \
    }                                                                                              \
  } while (0)
