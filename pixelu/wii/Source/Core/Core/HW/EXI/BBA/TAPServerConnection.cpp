// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include <cstdlib>
#include <cstring>
#include <optional>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2ipdef.h>
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/StringUtil.h"
#include "Core/HW/EXI/BBA/TAPServerConnection.h"
#include "SFML/Network/IpAddress.hpp"

namespace ExpansionInterface
{
namespace
{
#ifdef _WIN32
using IoSize = int;
using IoLength = int;
using AddressLength = int;
SOCKET Handle(int fd)
{
  return static_cast<SOCKET>(fd);
}
void CloseSocket(int fd)
{
  closesocket(Handle(fd));
}
#else
using IoSize = ssize_t;
using IoLength = std::size_t;
using AddressLength = socklen_t;
int Handle(int fd)
{
  return fd;
}
void CloseSocket(int fd)
{
  close(fd);
}
#endif

// Fills in the address for "host:port" (or a UNIX socket path). Returns its length, or 0.
int ResolveDestination(const std::string& destination, sockaddr_storage* storage)
{
  if (destination.empty())
  {
    ERROR_LOG_FMT(SP1, "Cannot connect: destination is empty\n");
    return 0;
  }

  if (destination.front() == '/')
  {
#ifdef _WIN32
    ERROR_LOG_FMT(SP1, "UNIX sockets are not supported on Windows\n");
    return 0;
#else
    auto* const unix_address = reinterpret_cast<sockaddr_un*>(storage);
    if (destination.size() >= sizeof(unix_address->sun_path))
    {
      ERROR_LOG_FMT(SP1, "Socket path is too long; unable to create tapserver connection\n");
      return 0;
    }
    unix_address->sun_family = AF_UNIX;
    std::memcpy(unix_address->sun_path, destination.c_str(), destination.size() + 1);
    return static_cast<int>(sizeof(*unix_address));
#endif
  }

  const std::size_t colon = destination.find(':');
  if (colon == std::string::npos)
  {
    ERROR_LOG_FMT(SP1, "Destination IP address does not include port\n");
    return 0;
  }

  const std::optional<sf::IpAddress> host = sf::IpAddress::resolve(destination.substr(0, colon));
  if (!host)
  {
    ERROR_LOG_FMT(SP1, "Destination IP address is not valid\n");
    return 0;
  }

  const int port = std::atoi(destination.c_str() + colon + 1);
  if (port < 1 || port > 65535)
  {
    ERROR_LOG_FMT(SP1, "Destination port is not valid\n");
    return 0;
  }

  auto* const inet_address = reinterpret_cast<sockaddr_in*>(storage);
  inet_address->sin_family = AF_INET;
  inet_address->sin_addr.s_addr = htonl(host->toInteger());
  inet_address->sin_port = htons(static_cast<u16>(port));
  return static_cast<int>(sizeof(*inet_address));
}

int OpenConnection(const std::string& destination)
{
  sockaddr_storage address{};
  const int address_length = ResolveDestination(destination, &address);
  if (address_length == 0)
    return -1;

  const int protocol = address.ss_family == AF_INET ? IPPROTO_TCP : 0;
  const int fd = static_cast<int>(socket(address.ss_family, SOCK_STREAM, protocol));
  if (fd == -1)
  {
    ERROR_LOG_FMT(SP1, "Couldn't create socket; unable to create tapserver connection\n");
    return -1;
  }
  Common::SetPlatformSocketOptions(fd);

  if (connect(Handle(fd), reinterpret_cast<const sockaddr*>(&address),
              static_cast<AddressLength>(address_length)) == -1)
  {
    INFO_LOG_FMT(SP1, "Couldn't connect socket ({}), unable to create tapserver connection\n",
                 Common::StrNetworkError());
    CloseSocket(fd);
    return -1;
  }
  return fd;
}
}  // namespace

TAPServerConnection::TAPServerConnection(std::string destination, RecvCallback recv_cb,
                                         std::size_t max_frame_size)
    : m_destination(std::move(destination)), m_on_frame(std::move(recv_cb)),
      m_max_frame_size(max_frame_size)
{
}

bool TAPServerConnection::Activate()
{
  if (IsActivated())
    return true;

  m_fd = OpenConnection(m_destination);
  return m_fd >= 0 && RecvInit();
}

void TAPServerConnection::Deactivate()
{
  m_receiving.Clear();
  m_stop_reading.Set();
  if (m_read_thread.joinable())
    m_read_thread.join();
  m_stop_reading.Clear();

  if (m_fd >= 0)
    CloseSocket(m_fd);
  m_fd = -1;
}

bool TAPServerConnection::IsActivated()
{
  return m_fd >= 0;
}

bool TAPServerConnection::RecvInit()
{
  m_read_thread = std::thread(&TAPServerConnection::ReadLoop, this);
  return true;
}

void TAPServerConnection::RecvStart()
{
  m_receiving.Set();
}

void TAPServerConnection::RecvStop()
{
  m_receiving.Clear();
}

bool TAPServerConnection::SendWithLength(const char* data, std::size_t size, const char* caller)
{
  const char length[2] = {static_cast<char>(size & 0xff), static_cast<char>((size >> 8) & 0xff)};
  if (send(Handle(m_fd), length, 2, Common::SEND_FLAGS) != 2)
  {
    ERROR_LOG_FMT(SP1, "{}(): could not write size field", caller);
    return false;
  }

  const IoSize sent = send(Handle(m_fd), data, static_cast<IoLength>(size), Common::SEND_FLAGS);
  if (sent < 0 || static_cast<std::size_t>(sent) != size)
  {
    ERROR_LOG_FMT(SP1, "{}(): expected to write {} bytes, instead wrote {}", caller, size, sent);
    return false;
  }
  return true;
}

bool TAPServerConnection::SendAndRemoveAllHDLCFrames(std::string* send_buf)
{
  constexpr char FLAG = 0x7E;
  while (!send_buf->empty())
  {
    const std::size_t opening = send_buf->find(FLAG);
    if (opening == std::string::npos)
      break;
    const std::size_t closing = send_buf->find(FLAG, opening + 1);
    if (closing == std::string::npos)
      break;

    // Both flag bytes go with the frame.
    const std::size_t frame_end = closing + 1;
    if (!SendWithLength(send_buf->data() + opening, frame_end - opening,
                        "SendAndRemoveAllHDLCFrames"))
    {
      return false;
    }
    send_buf->erase(0, frame_end);
  }
  return true;
}

bool TAPServerConnection::SendFrame(const u8* frame, u32 size)
{
  INFO_LOG_FMT(SP1, "SendFrame {}\n{}", size, ArrayToString(frame, size, 0x10));
  return SendWithLength(reinterpret_cast<const char*>(frame), size, "SendFrame");
}

void TAPServerConnection::ReadLoop()
{
  // Where the reader is within the stream: the two length bytes (possibly split across reads),
  // then the frame body. Frames are read even while receiving is off, so the server never
  // blocks, but are only delivered if receiving was on when their length arrived.
  enum class Expect
  {
    Length,
    LengthHigh,
    Body,
  };
  Expect expect = Expect::Length;
  std::size_t frame_length = 0;
  std::size_t received = 0;
  bool deliver = false;
  std::string frame;

  const auto begin_body = [&] {
    if (frame_length == 0)
    {
      // Nothing to read; an empty frame is dropped.
      expect = Expect::Length;
      return;
    }
    frame.assign(frame_length, '\0');
    received = 0;
    deliver = frame_length <= m_max_frame_size && m_receiving.IsSet();
    if (frame_length > m_max_frame_size)
      ERROR_LOG_FMT(SP1, "Packet is too large ({} bytes); dropping it", frame_length);
    expect = Expect::Body;
  };

  while (!m_stop_reading.IsSet())
  {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(Handle(m_fd), &readable);
    timeval timeout{0, 50000};
    const int ready = select(m_fd + 1, &readable, nullptr, nullptr, &timeout);
    if (ready < 0)
    {
      ERROR_LOG_FMT(SP1, "Can't poll tapserver fd: {}", Common::StrNetworkError());
      continue;
    }
    if (ready == 0)
      continue;

    if (expect == Expect::Length)
    {
      unsigned char length[2];
      const IoSize got = recv(Handle(m_fd), reinterpret_cast<char*>(length), 2, 0);
      if (got == 2)
      {
        frame_length = length[0] | (std::size_t{length[1]} << 8);
        begin_body();
      }
      else if (got == 1)
      {
        frame_length = length[0];
        expect = Expect::LengthHigh;
      }
      else
      {
        ERROR_LOG_FMT(SP1, "Failed to read size field from destination: {}",
                      Common::StrNetworkError());
      }
    }
    else if (expect == Expect::LengthHigh)
    {
      unsigned char high = 0;
      if (recv(Handle(m_fd), reinterpret_cast<char*>(&high), 1, 0) != 1)
      {
        ERROR_LOG_FMT(SP1, "Failed to read split size field from destination: {}",
                      Common::StrNetworkError());
        continue;
      }
      frame_length |= std::size_t{high} << 8;
      begin_body();
    }
    else
    {
      const IoSize got = recv(Handle(m_fd), frame.data() + received,
                              static_cast<IoLength>(frame_length - received), 0);
      if (got <= 0)
      {
        ERROR_LOG_FMT(SP1, "Failed to read data from destination: {}", Common::StrNetworkError());
        continue;
      }
      received += static_cast<std::size_t>(got);
      if (received == frame_length)
      {
        if (deliver)
          m_on_frame(std::move(frame));
        frame.clear();
        expect = Expect::Length;
      }
    }
  }
}

}  // namespace ExpansionInterface
