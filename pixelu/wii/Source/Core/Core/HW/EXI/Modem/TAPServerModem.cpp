// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/EXI/EXI_DeviceModem.h"

#include <string>
#include <utility>

namespace ExpansionInterface
{
// The modem adapter on a tapserver connection. The modem's data stream is PPP in HDLC framing;
// each complete HDLC frame travels as one tapserver frame.

CEXIModem::TAPServerNetworkInterface::TAPServerNetworkInterface(CEXIModem* modem_ref,
                                                                const std::string& destination)
    : NetworkInterface(modem_ref),
      m_tapserver_if(
          destination, [this](std::string&& data) { HandleReceivedFrame(std::move(data)); },
          MODEM_RECV_SIZE)
{
}

bool CEXIModem::TAPServerNetworkInterface::Activate()
{
  return m_tapserver_if.Activate();
}

void CEXIModem::TAPServerNetworkInterface::Deactivate()
{
  m_tapserver_if.Deactivate();
}

bool CEXIModem::TAPServerNetworkInterface::IsActivated()
{
  return m_tapserver_if.IsActivated();
}

bool CEXIModem::TAPServerNetworkInterface::SendAndRemoveAllHDLCFrames(std::string* send_buffer)
{
  const std::size_t pending_before = send_buffer->size();
  if (!m_tapserver_if.SendAndRemoveAllHDLCFrames(send_buffer))
    return false;
  // Tell the modem when at least one frame went out.
  if (send_buffer->size() < pending_before)
    m_modem_ref->SendComplete();
  return true;
}

bool CEXIModem::TAPServerNetworkInterface::RecvInit()
{
  return m_tapserver_if.RecvInit();
}

void CEXIModem::TAPServerNetworkInterface::RecvStart()
{
  m_tapserver_if.RecvStart();
}

void CEXIModem::TAPServerNetworkInterface::RecvStop()
{
  m_tapserver_if.RecvStop();
}

void CEXIModem::TAPServerNetworkInterface::HandleReceivedFrame(std::string&& data)
{
  m_modem_ref->AddToReceiveBuffer(std::move(data));
}

}  // namespace ExpansionInterface
