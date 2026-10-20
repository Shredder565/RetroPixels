// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceEthernet.h"

#include <string>
#include <utility>

#include "Common/Logging/Log.h"

namespace ExpansionInterface
{
// The broadband adapter on a tapserver connection: Ethernet frames pass through unchanged.

CEXIETHERNET::TAPServerNetworkInterface::TAPServerNetworkInterface(CEXIETHERNET* eth_ref,
                                                                   const std::string& destination)
    : NetworkInterface(eth_ref),
      m_connection(
          destination, [this](std::string&& frame) { OnFrame(std::move(frame)); }, BBA_RECV_SIZE)
{
}

bool CEXIETHERNET::TAPServerNetworkInterface::Activate()
{
  return m_connection.Activate();
}

void CEXIETHERNET::TAPServerNetworkInterface::Deactivate()
{
  m_connection.Deactivate();
}

bool CEXIETHERNET::TAPServerNetworkInterface::IsActivated()
{
  return m_connection.IsActivated();
}

bool CEXIETHERNET::TAPServerNetworkInterface::RecvInit()
{
  return m_connection.RecvInit();
}

void CEXIETHERNET::TAPServerNetworkInterface::RecvStart()
{
  m_connection.RecvStart();
}

void CEXIETHERNET::TAPServerNetworkInterface::RecvStop()
{
  m_connection.RecvStop();
}

bool CEXIETHERNET::TAPServerNetworkInterface::SendFrame(const u8* frame, u32 size)
{
  if (!m_connection.SendFrame(frame, size))
    return false;
  m_eth_ref->SendComplete();
  return true;
}

void CEXIETHERNET::TAPServerNetworkInterface::OnFrame(std::string&& frame)
{
  if (frame.size() > BBA_RECV_SIZE)
  {
    ERROR_LOG_FMT(SP1, "Received BBA frame of size {}, which is larger than maximum size {}",
                  frame.size(), BBA_RECV_SIZE);
    return;
  }
  m_eth_ref->ReceiveFrame(reinterpret_cast<const u8*>(frame.data()),
                          static_cast<u32>(frame.size()));
}

}  // namespace ExpansionInterface
