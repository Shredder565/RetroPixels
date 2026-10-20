// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <string>
#include <thread>

#include "Common/CommonTypes.h"
#include "Common/Flag.h"
#include "Common/SocketContext.h"

namespace ExpansionInterface
{
// A connection to a tapserver (https://github.com/fuzziqersoftware/tapserver). Both directions
// carry frames as a 16-bit little-endian length followed by that many bytes. The destination is
// "host:port", or a UNIX socket path starting with '/' where those exist.
class TAPServerConnection
{
public:
  using RecvCallback = std::function<void(std::string&&)>;

  TAPServerConnection(std::string destination, RecvCallback recv_cb, std::size_t max_frame_size);

  bool Activate();
  void Deactivate();
  bool IsActivated();
  bool RecvInit();
  void RecvStart();
  void RecvStop();
  // Sends every complete HDLC frame (0x7E ... 0x7E) at the start of the buffer and removes it,
  // together with anything before it. Used by the modem adapter.
  bool SendAndRemoveAllHDLCFrames(std::string* send_buf);
  bool SendFrame(const u8* frame, u32 size);

private:
  bool SendWithLength(const char* data, std::size_t size, const char* caller);
  void ReadLoop();

  const std::string m_destination;
  const RecvCallback m_on_frame;
  const std::size_t m_max_frame_size;
  Common::SocketContext m_socket_context;

  int m_fd = -1;
  std::thread m_read_thread;
  Common::Flag m_receiving;
  Common::Flag m_stop_reading;
};

}  // namespace ExpansionInterface
