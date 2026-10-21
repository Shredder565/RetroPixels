// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/Logging/LogManager.h"

// Sends log lines to the debugger output (OutputDebugString) on Windows, and to standard error
// (coloured on a terminal) on Linux.
class ConsoleListener : public Common::Log::LogListener
{
public:
  ConsoleListener();
  ~ConsoleListener() override;

  void Log(Common::Log::LogLevel level, const char* text) override;
};
