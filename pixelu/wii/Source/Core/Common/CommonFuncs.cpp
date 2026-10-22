// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/CommonFuncs.h"

#include <cerrno>
#include <cstring>

#ifdef _WIN32
#include <windows.h>

#include "Common/StringUtil.h"
#endif

namespace Common
{
namespace
{
constexpr std::size_t MESSAGE_CAPACITY = 256;

#ifndef _WIN32
// glibc's strerror_r may be the GNU flavour (returns char*) or the POSIX one (returns int,
// 0 on success) depending on feature macros; these overloads accept either result.
[[maybe_unused]] const char* StrerrorResult(const char* message, const char*)
{
  return message;
}
[[maybe_unused]] const char* StrerrorResult(int status, const char* buffer)
{
  return status == 0 ? buffer : "";
}
#endif
}  // namespace

const char* StrErrorWrapper(int error, char* buffer, std::size_t length)
{
#ifdef _WIN32
  return strerror_s(buffer, length, error) == 0 ? buffer : "";
#else
  return StrerrorResult(strerror_r(error, buffer, length), buffer);
#endif
}

std::string StrerrorString(int error)
{
  char buffer[MESSAGE_CAPACITY];
  return StrErrorWrapper(error, buffer, sizeof(buffer));
}

std::string LastStrerrorString()
{
  return StrerrorString(errno);
}

#ifdef _WIN32
std::string GetLastErrorString()
{
  return GetWin32ErrorString(GetLastError());
}

std::string GetWin32ErrorString(unsigned long error_code)
{
  wchar_t buffer[MESSAGE_CAPACITY] = {};
  FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, error_code,
                 MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), buffer,
                 static_cast<DWORD>(MESSAGE_CAPACITY), nullptr);
  return WStringToUTF8(buffer);
}

std::optional<std::wstring> GetModuleName(void* hInstance)
{
  // GetModuleFileNameW truncates (and returns the buffer size) when the buffer is too small, so
  // keep doubling until the whole path fits.
  std::wstring path(64, L'\0');
  for (;;)
  {
    const DWORD capacity = static_cast<DWORD>(path.size());
    const DWORD written =
        GetModuleFileNameW(static_cast<HMODULE>(hInstance), path.data(), capacity);
    if (written == 0)
      return std::nullopt;
    if (written < capacity)
    {
      path.resize(written);
      return path;
    }
    path.resize(path.size() * 2);
  }
}
#endif
}  // namespace Common
