// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "NetOutputs.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace
{
  const unsigned kMaxClients = 10;   // size of the socket set (plus the server)
}

CNetOutputs::CNetOutputs()
{
}

CNetOutputs::~CNetOutputs()
{
  m_running = false;
  if (m_acceptThread.joinable())
    m_acceptThread.join();
  {
    std::lock_guard<std::recursive_mutex> lock(m_lock);
    while (!m_clients.empty())
      Drop(m_clients.front());
  }
  if (m_server)
    SDLNet_TCP_Close(m_server);
  m_server = nullptr;
  if (m_set)
    SDLNet_FreeSocketSet(m_set);
  m_set = nullptr;
  SDLNet_Quit();
}

std::string CNetOutputs::Line(const std::string &name, const std::string &value) const
{
  return name + " = " + value + m_lineEnd;
}

bool CNetOutputs::Initialize()
{
  SDLNet_Init();
  IPaddress address;
  bool ok = false;
  if (SDLNet_ResolveHost(&address, nullptr, Uint16(m_tcpPort)) == -1)
    ErrorLog("ResolveHost error: %s", SDLNet_GetError());
  else if (!(m_server = SDLNet_TCP_Open(&address)))
    ErrorLog("TCP_Open error: %s", SDLNet_GetError());
  else
    ok = true;
  if (!ok)
  {
    ErrorLog("Unable to create TCP server socket.");
    return false;
  }

  m_set = SDLNet_AllocSocketSet(kMaxClients + 1);
  SDLNet_TCP_AddSocket(m_set, m_server);

  m_running = true;
  m_acceptThread = std::thread(&CNetOutputs::AcceptLoop, this);
  return true;
}

void CNetOutputs::AcceptLoop()
{
  while (m_running)
  {
    if (TCPsocket client = SDLNet_TCP_Accept(m_server))
    {
      if (IPaddress *peer = SDLNet_TCP_GetPeerAddress(client))
        InfoLog("Client connected from %s:%d", SDLNet_ResolveIP(peer), peer->port);
      else
        ErrorLog("Failed to get client IP address: %s", SDLNet_GetError());
      Welcome(client);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
}

void CNetOutputs::Attached()
{
  // "mame_start = <game>" and "tcp = <port>" to everyone on the network
  bool ok = false;
  if (UDPsocket udp = SDLNet_UDP_Open(0))
  {
    IPaddress everyone;
    if (SDLNet_ResolveHost(&everyone, "255.255.255.255", Uint16(m_udpPort)) == -1)
      ErrorLog("ResolveHost error: %s", SDLNet_GetError());
    else
    {
      ok = true;
      const std::string text = Line("mame_start", GetGame().name) + Line("tcp", std::to_string(m_tcpPort));
      UDPpacket *packet = SDLNet_AllocPacket(512);
      packet->len = int(std::min<size_t>(text.size(), 512));
      memcpy(packet->data, text.data(), size_t(packet->len));
      packet->address = everyone;
      if (SDLNet_UDP_Send(udp, -1, packet) == 0)
        ErrorLog("UDP_Send error: %s", SDLNet_GetError());
      else
        DebugLog("Broadcast sent!");
      SDLNet_FreePacket(packet);
    }
    SDLNet_UDP_Close(udp);
  }
  else
    ErrorLog("UDP_Open error: %s", SDLNet_GetError());

  if (!ok)
    ErrorLog("Unable to notify with udp broadcast.");
}

bool CNetOutputs::Write(TCPsocket client, const std::string &text)
{
  const int sent = SDLNet_TCP_Send(client, text.data(), int(text.size()));
  DebugLog("Sent to client: %s", text.c_str());
  DebugLog("Bytes sent: %d", sent);
  DebugLog("Expected bytes: %zu", text.size());
  if (sent < int(text.size()))
  {
    ErrorLog("Failed to send TCP data to client: %s (tried to send %zu bytes, sent %d bytes).", SDLNet_GetError(), text.size(), sent);
    return false;
  }
  return true;
}

// Every output that has been set so far, with its value.
bool CNetOutputs::SendAll(TCPsocket client)
{
  for (unsigned i = 0; i < NUM_OUTPUTS; i++)
  {
    EOutputs output = EOutputs(i);
    if (HasValue(output) && !Write(client, Line(GetOutputName(output), std::to_string(GetValue(output)))))
      return false;
  }
  return true;
}

void CNetOutputs::Welcome(TCPsocket client)
{
  std::lock_guard<std::recursive_mutex> lock(m_lock);
  if (std::find(m_clients.begin(), m_clients.end(), client) != m_clients.end())
  {
    SendAll(client);
    return;
  }
  SDLNet_TCP_AddSocket(m_set, client);
  m_clients.push_back(client);
  if (!Write(client, Line("mame_start", GetGame().name)))
  {
    ErrorLog("Failed to send game id string to client. Unregistering client.");
    Drop(client);
  }
  else if (!SendAll(client))
  {
    ErrorLog("Failed to send all output data to client. Unregistering client.");
    Drop(client);
  }
}

// Says goodbye ("mame_stop = 1") and closes the connection.
void CNetOutputs::Drop(TCPsocket client)
{
  std::lock_guard<std::recursive_mutex> lock(m_lock);
  auto it = std::find(m_clients.begin(), m_clients.end(), client);
  if (it == m_clients.end())
    return;
  m_clients.erase(it);
  Write(client, Line("mame_stop", "1"));
  SDLNet_TCP_DelSocket(m_set, client);
  SDLNet_TCP_Close(client);
}

void CNetOutputs::SendOutput(EOutputs output, UINT8, UINT8 value)
{
  std::lock_guard<std::recursive_mutex> lock(m_lock);
  const std::string line = Line(GetOutputName(output), std::to_string(value));
  std::vector<TCPsocket> failed;
  for (TCPsocket client : m_clients)
  {
    if (!Write(client, line))
    {
      ErrorLog("Failed to send output data to client. Unregistering client.");
      failed.push_back(client);
    }
  }
  for (TCPsocket client : failed)
    Drop(client);
}
