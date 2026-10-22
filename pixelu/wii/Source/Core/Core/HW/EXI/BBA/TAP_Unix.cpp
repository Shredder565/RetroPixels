// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// The Broadband Adapter's "TAP" backend on Linux: Ethernet frames go to and come from a TAP
// interface of the kernel's TUN/TAP driver (/dev/net/tun).
//
// The interface is called "Dolphin", the name GameCube BBA guides have always told Linux users
// to create, for example with
//     sudo ip tuntap add Dolphin mode tap user $USER && sudo ip link set Dolphin up
// (and then bridge it or give it an address). An existing persistent interface owned by the
// user can be attached without privileges; otherwise the kernel creates a temporary one, which
// needs CAP_NET_ADMIN.

#include "Core/HW/EXI/EXI_DeviceEthernet.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/StringUtil.h"

namespace ExpansionInterface
{
namespace
{
constexpr char TUN_DEVICE[] = "/dev/net/tun";
constexpr char TAP_NAME[] = "Dolphin";
// How often the reader looks at its stop flag while no frame arrives.
constexpr int READ_POLL_MS = 50;
}  // namespace

bool CEXIETHERNET::TAPNetworkInterface::Activate()
{
  if (IsActivated())
    return true;

  const int fd = open(TUN_DEVICE, O_RDWR | O_CLOEXEC);
  if (fd < 0)
  {
    PanicAlertFmtT("Couldn't open {0}: {1}. The broadband adapter's TAP mode needs the TUN/TAP "
                   "driver.",
                   TUN_DEVICE, std::strerror(errno));
    return false;
  }

  ifreq request{};
  // Whole Ethernet frames, without the 4-byte packet information header.
  request.ifr_flags = IFF_TAP | IFF_NO_PI;
  std::strncpy(request.ifr_name, TAP_NAME, IFNAMSIZ - 1);
  if (ioctl(fd, TUNSETIFF, &request) < 0)
  {
    const int error = errno;
    close(fd);
    PanicAlertFmtT("Couldn't attach to the TAP interface \"{0}\": {1}. Create it first, for "
                   "example with \"sudo ip tuntap add Dolphin mode tap user $USER\".",
                   TAP_NAME, std::strerror(error));
    return false;
  }

  m_fd = fd;
  INFO_LOG_FMT(SP1, "Attached to TAP interface {}", request.ifr_name);
  m_stop_reading.Clear();
  return RecvInit();
}

void CEXIETHERNET::TAPNetworkInterface::Deactivate()
{
  if (!IsActivated())
    return;

  m_receiving.Clear();
  m_stop_reading.Set();
  if (m_read_thread.joinable())
    m_read_thread.join();

  close(m_fd);
  m_fd = -1;
}

bool CEXIETHERNET::TAPNetworkInterface::IsActivated()
{
  return m_fd >= 0;
}

void CEXIETHERNET::TAPNetworkInterface::ReadLoop()
{
  while (!m_stop_reading.IsSet())
  {
    pollfd waiting{m_fd, POLLIN, 0};
    const int ready = poll(&waiting, 1, READ_POLL_MS);
    if (ready < 0)
    {
      if (errno == EINTR)
        continue;
      ERROR_LOG_FMT(SP1, "Waiting for TAP frames failed: {}", std::strerror(errno));
      break;
    }
    if (ready == 0 || !(waiting.revents & POLLIN))
      continue;

    const ssize_t length = read(m_fd, m_read_buffer.data(), m_read_buffer.size());
    if (length < 0)
    {
      if (errno != EAGAIN && errno != EINTR)
        ERROR_LOG_FMT(SP1, "Reading a TAP frame failed: {}", std::strerror(errno));
      continue;
    }

    DEBUG_LOG_FMT(SP1, "Received {} bytes:\n {}", length,
                  ArrayToString(m_read_buffer.data(), static_cast<u32>(length), 0x10));
    if (m_receiving.IsSet())
      m_eth_ref->ReceiveFrame(m_read_buffer.data(), static_cast<u32>(length));
  }
}

bool CEXIETHERNET::TAPNetworkInterface::SendFrame(const u8* frame, u32 size)
{
  DEBUG_LOG_FMT(SP1, "SendFrame {} bytes:\n{}", size, ArrayToString(frame, size, 0x10));

  // One write() is one frame on a TAP descriptor.
  const ssize_t written = write(m_fd, frame, size);
  if (written != static_cast<ssize_t>(size))
  {
    ERROR_LOG_FMT(SP1, "Sending a {}-byte frame to the TAP interface failed: {}", size,
                  written < 0 ? std::strerror(errno) : "short write");
    return false;
  }

  m_eth_ref->SendComplete();
  return true;
}

bool CEXIETHERNET::TAPNetworkInterface::RecvInit()
{
  m_read_thread = std::thread(&TAPNetworkInterface::ReadLoop, this);
  return true;
}

void CEXIETHERNET::TAPNetworkInterface::RecvStart()
{
  m_receiving.Set();
}

void CEXIETHERNET::TAPNetworkInterface::RecvStop()
{
  m_receiving.Clear();
}
}  // namespace ExpansionInterface
