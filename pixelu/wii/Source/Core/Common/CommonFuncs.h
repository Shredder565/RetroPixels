// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <string>

#ifdef _WIN32
#include <optional>
#endif

#ifdef __APPLE__
#include "Common/CommonTypes.h"
#endif

#ifdef _WIN32
// POSIX names used throughout the code base, mapped to their MSVC CRT equivalents (64-bit file
// offsets and sizes where there is a choice).
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define unlink _unlink
#define vscprintf _vscprintf
#define fseeko _fseeki64
#define ftello _ftelli64
#define atoll _atoi64
#define stat _stat64
#define fstat _fstat64
#define fileno _fileno

extern "C" {
__declspec(dllimport) void __stdcall DebugBreak(void);
}

// Stops in the debugger (or terminates the process when none is attached).
#define Crash()                                                                                    \
  {                                                                                                \
    DebugBreak();                                                                                  \
  }
#else
#define Crash()                                                                                    \
  {                                                                                                \
    __builtin_trap();                                                                              \
  }
#endif

namespace Common
{
// Thread-safe strerror(): writes the message for `error` into `buffer` if needed and returns a
// pointer to it (which may be a static string instead). May change errno.
const char* StrErrorWrapper(int error, char* buffer, std::size_t length);

// Message text for an errno value / for the current errno. May change errno.
std::string StrerrorString(int error);
std::string LastStrerrorString();

#ifdef _WIN32
// Message text for GetLastError(). May change the last-error value.
std::string GetLastErrorString();

// Message text for a Win32 error code.
std::string GetWin32ErrorString(unsigned long error_code);

// Full path of the module (DLL or EXE) with the given handle, or nullopt on failure.
std::optional<std::wstring> GetModuleName(void* hInstance);
#endif

#ifdef __APPLE__
struct MacOSVersion
{
  s64 major;
  s64 minor;
  s64 patch;
};

MacOSVersion GetMacOSVersion();
#endif
}  // namespace Common
