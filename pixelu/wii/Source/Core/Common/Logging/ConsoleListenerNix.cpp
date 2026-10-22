// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Log lines on Linux go to standard error. When that is a terminal that understands ANSI escape
// codes (and NO_COLOR is not set, see no-color.org) each line is coloured by its level.

#include "Common/Logging/ConsoleListener.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

namespace
{
bool WantsColour()
{
  if (!isatty(STDERR_FILENO))
    return false;
  const char* no_colour = std::getenv("NO_COLOR");
  if (no_colour && no_colour[0] != '\0')
    return false;
  const char* term = std::getenv("TERM");
  return term && term[0] != '\0' && std::strcmp(term, "dumb") != 0;
}

const char* ColourFor(Common::Log::LogLevel level)
{
  switch (level)
  {
  case Common::Log::LogLevel::LNOTICE:
    return "\x1b[92m";  // bright green
  case Common::Log::LogLevel::LERROR:
    return "\x1b[91m";  // bright red
  case Common::Log::LogLevel::LWARNING:
    return "\x1b[93m";  // bright yellow
  case Common::Log::LogLevel::LINFO:
    return "\x1b[97m";  // bright white
  default:
    return "\x1b[37m";  // grey for debug and anything else
  }
}

bool s_colour = false;
}  // namespace

ConsoleListener::ConsoleListener()
{
  s_colour = WantsColour();
}

ConsoleListener::~ConsoleListener()
{
  std::fflush(stderr);
}

void ConsoleListener::Log(Common::Log::LogLevel level, const char* text)
{
  // The text already ends in a newline. One fputs per line keeps lines from different threads
  // whole (stdio locks the stream for each call).
  if (s_colour)
  {
    std::fprintf(stderr, "%s%s\x1b[0m", ColourFor(level), text);
  }
  else
  {
    std::fputs(text, stderr);
  }
}
