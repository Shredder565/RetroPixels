// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
using socklen_t = int;
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "Common/CommonTypes.h"

namespace Common
{
class PCAP;
}

// Dumps the emulated console's network traffic (the plain text of SSL connections, socket data
// and broadband adapter frames) for debugging.
namespace Core
{
enum class NetworkCaptureType
{
  None,
  Raw,
  PCAP,
};

class NetworkCaptureLogger
{
public:
  NetworkCaptureLogger();
  NetworkCaptureLogger(const NetworkCaptureLogger&) = delete;
  NetworkCaptureLogger(NetworkCaptureLogger&&) = delete;
  NetworkCaptureLogger& operator=(const NetworkCaptureLogger&) = delete;
  NetworkCaptureLogger& operator=(NetworkCaptureLogger&&) = delete;
  virtual ~NetworkCaptureLogger();

  virtual void OnNewSocket(s32 socket) = 0;

  virtual void LogSSLRead(const void* data, std::size_t length, s32 socket) = 0;
  virtual void LogSSLWrite(const void* data, std::size_t length, s32 socket) = 0;

  // `from` / `to` is the peer for unconnected sockets, or null to ask the socket.
  virtual void LogRead(const void* data, std::size_t length, s32 socket, sockaddr* from) = 0;
  virtual void LogWrite(const void* data, std::size_t length, s32 socket, sockaddr* to) = 0;

  virtual void LogBBA(const void* data, std::size_t length) = 0;

  virtual NetworkCaptureType GetCaptureType() const = 0;
};

// Logs nothing.
class DummyNetworkCaptureLogger : public NetworkCaptureLogger
{
public:
  void OnNewSocket(s32 socket) override;

  void LogSSLRead(const void* data, std::size_t length, s32 socket) override;
  void LogSSLWrite(const void* data, std::size_t length, s32 socket) override;

  void LogRead(const void* data, std::size_t length, s32 socket, sockaddr* from) override;
  void LogWrite(const void* data, std::size_t length, s32 socket, sockaddr* to) override;

  void LogBBA(const void* data, std::size_t length) override;

  NetworkCaptureType GetCaptureType() const override;
};

// Appends SSL plain text to "<game id>_read.bin" / "<game id>_write.bin".
class BinarySSLCaptureLogger final : public DummyNetworkCaptureLogger
{
public:
  void LogSSLRead(const void* data, std::size_t length, s32 socket) override;
  void LogSSLWrite(const void* data, std::size_t length, s32 socket) override;

  NetworkCaptureType GetCaptureType() const override;
};

// Writes everything to a .pcap file, wrapping socket data in made-up Ethernet/IPv4/TCP or UDP
// headers so packet analysers can read it.
class PCAPSSLCaptureLogger final : public NetworkCaptureLogger
{
public:
  PCAPSSLCaptureLogger();
  ~PCAPSSLCaptureLogger() override;

  void OnNewSocket(s32 socket) override;

  void LogSSLRead(const void* data, std::size_t length, s32 socket) override;
  void LogSSLWrite(const void* data, std::size_t length, s32 socket) override;

  void LogRead(const void* data, std::size_t length, s32 socket, sockaddr* from) override;
  void LogWrite(const void* data, std::size_t length, s32 socket, sockaddr* to) override;

  void LogBBA(const void* data, std::size_t length) override;

  NetworkCaptureType GetCaptureType() const override;

private:
  enum class Direction
  {
    Incoming,
    Outgoing,
  };

  void LogSocketData(Direction direction, const void* data, std::size_t length, s32 socket,
                     sockaddr* peer);
  void WritePacket(Direction direction, const u8* data, u16 length, s32 socket,
                   const sockaddr_in& source, const sockaddr_in& destination);

  std::unique_ptr<Common::PCAP> m_pcap;
  std::mutex m_bba_mutex;
  // Made-up TCP sequence numbers, per socket and direction.
  std::map<s32, u32> m_incoming_sequence;
  std::map<s32, u32> m_outgoing_sequence;
};
}  // namespace Core
