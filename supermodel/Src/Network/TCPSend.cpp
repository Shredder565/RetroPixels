// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "TCPSend.h"
#include "OSD/Logger.h"

TCPSend::TCPSend(std::string &ip, int port)
  : m_ip(ip), m_port(port), m_socket(nullptr)
{
  SDLNet_Init();
}

TCPSend::~TCPSend()
{
  if (m_socket)
    SDLNet_TCP_Close(m_socket);
  m_socket = nullptr;
  SDLNet_Quit();
}

bool TCPSend::Connected()
{
  return m_socket != nullptr;
}

bool TCPSend::Connect()
{
  IPaddress address;
  if (SDLNet_ResolveHost(&address, m_ip.c_str(), Uint16(m_port)) == 0)
    m_socket = SDLNet_TCP_Open(&address);
  return Connected();
}

bool TCPSend::Send(const void *data, int length)
{
  if (!Connected())
    return false;

  // Length first. An empty message is only the length (the receiver reads
  // it as "the link is going down").
  SDLNet_TCP_Send(m_socket, &length, sizeof(length));
  if (length == 0)
    return true;

  if (SDLNet_TCP_Send(m_socket, data, length) < length)
  {
    SDLNet_TCP_Close(m_socket);
    m_socket = nullptr;
  }
  return true;
}
