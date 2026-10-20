// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include <cstring>

#include <libipc/ipc.h>

#include "Common/Logging/Log.h"
#include "Core/HW/EXI/EXI_DeviceEthernet.h"

namespace ExpansionInterface
{
// The broadband adapter on a cpp-ipc channel shared with other local processes (for example
// other emulator instances). Every message on the channel is one Ethernet frame.

namespace
{
constexpr char CHANNEL_NAME[] = "dolphin-emu-bba-ipc";
constexpr u64 SEND_TIMEOUT_MS = 3000;
constexpr u64 RECEIVE_POLL_MS = 50;
}  // namespace

bool CEXIETHERNET::IPCBBAInterface::Activate()
{
  // Unlike the other backends, a second activation is refused.
  if (m_active)
    return false;

  m_channel.connect(CHANNEL_NAME, ipc::sender | ipc::receiver);
  if (!m_channel.valid())
    return false;

  m_receiving.Clear();
  m_stop_reading.Clear();
  m_active = true;
  return RecvInit();
}

void CEXIETHERNET::IPCBBAInterface::Deactivate()
{
  m_receiving.Clear();
  m_stop_reading.Set();
  if (m_read_thread.joinable())
    m_read_thread.join();

  m_channel.disconnect();
  m_active = false;
}

bool CEXIETHERNET::IPCBBAInterface::IsActivated()
{
  return m_active;
}

bool CEXIETHERNET::IPCBBAInterface::SendFrame(const u8* const frame, const u32 size)
{
  if (!m_active)
    return false;

  if (!m_channel.send(frame, size, SEND_TIMEOUT_MS))
  {
    ERROR_LOG_FMT(SP1, "Failed to send frame");
    return false;
  }

  m_eth_ref->SendComplete();
  return true;
}

bool CEXIETHERNET::IPCBBAInterface::RecvInit()
{
  m_read_thread = std::thread(&CEXIETHERNET::IPCBBAInterface::ReadLoop, this);
  return true;
}

void CEXIETHERNET::IPCBBAInterface::RecvStart()
{
  m_receiving.Set();
}

void CEXIETHERNET::IPCBBAInterface::RecvStop()
{
  m_receiving.Clear();
}

void CEXIETHERNET::IPCBBAInterface::ReadLoop()
{
  while (!m_stop_reading.IsSet())
  {
    const ipc::buff_t message{m_channel.recv(RECEIVE_POLL_MS)};
    if (message.empty() || !m_receiving.IsSet())
      continue;

    if (message.size() > BBA_RECV_SIZE)
    {
      ERROR_LOG_FMT(SP1, "Dropping IPC frame of {} bytes (larger than {})", message.size(),
                    BBA_RECV_SIZE);
      continue;
    }
    m_eth_ref->ReceiveFrame(static_cast<const u8*>(message.data()),
                            static_cast<u32>(message.size()));
  }
}

}  // namespace ExpansionInterface
