// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Common/SocketContext.h"

#ifdef _WIN32
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#endif

namespace Common
{
#ifdef _WIN32
std::mutex SocketContext::s_lock;
size_t SocketContext::s_num_objects = 0;
WSADATA SocketContext::s_data;

SocketContext::SocketContext()
{
  std::lock_guard lock(s_lock);
  if (s_num_objects++ != 0)
    return;

  // WSAStartup returns its error code directly (WSAGetLastError is not valid here).
  const int error = WSAStartup(MAKEWORD(2, 2), &s_data);
  if (error != 0)
  {
    ERROR_LOG_FMT(COMMON, "WSAStartup failed with error {}: {}", error,
                  Common::DecodeNetworkError(error));
    return;
  }
  INFO_LOG_FMT(COMMON, "Winsock started: version {}.{}, highest {}.{}", LOBYTE(s_data.wVersion),
               HIBYTE(s_data.wVersion), LOBYTE(s_data.wHighVersion), HIBYTE(s_data.wHighVersion));
}

SocketContext::~SocketContext()
{
  std::lock_guard lock(s_lock);
  if (--s_num_objects == 0)
    WSACleanup();
}
#else
SocketContext::SocketContext() = default;
SocketContext::~SocketContext() = default;
#endif
}  // namespace Common
