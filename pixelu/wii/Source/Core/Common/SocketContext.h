// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef _WIN32
#include <cstddef>
#include <mutex>
#include <winsock2.h>
#endif

namespace Common
{
// Keeps Winsock initialised while at least one instance exists (no-op elsewhere). Hold one
// for as long as sockets are used.
class SocketContext
{
public:
  SocketContext();
  ~SocketContext();

  SocketContext(const SocketContext&) = delete;
  SocketContext(SocketContext&&) = delete;
  SocketContext& operator=(const SocketContext&) = delete;
  SocketContext& operator=(SocketContext&&) = delete;

private:
#ifdef _WIN32
  static std::mutex s_lock;
  static size_t s_num_objects;
  static WSADATA s_data;
#endif
};
}  // namespace Common
