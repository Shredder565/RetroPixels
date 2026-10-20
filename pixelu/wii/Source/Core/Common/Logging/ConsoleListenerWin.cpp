// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/Logging/ConsoleListener.h"

#include <string>

#include <windows.h>

#include "Common/StringUtil.h"

ConsoleListener::ConsoleListener() = default;

ConsoleListener::~ConsoleListener() = default;

void ConsoleListener::Log([[maybe_unused]] Common::Log::LogLevel level, const char* text)
{
  const std::wstring wide = UTF8ToWString(text);
  OutputDebugStringW(wide.c_str());
}
