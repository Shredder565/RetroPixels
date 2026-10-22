// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/TimeUtil.h"

#include <cerrno>
#include <cstring>

#include "Common/Logging/Log.h"

namespace Common
{
std::optional<std::tm> LocalTime(std::time_t time)
{
  std::tm result{};
#ifdef _WIN32
  const int status = localtime_s(&result, &time);
  const bool ok = status == 0;
  const int error = status;
#else
  const bool ok = localtime_r(&time, &result) != nullptr;
  const int error = errno;
#endif
  if (ok)
    return result;

  ERROR_LOG_FMT(COMMON, "Failed to convert time to local time: {}", std::strerror(error));
  return std::nullopt;
}
}  // namespace Common
