// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "TCPReceive.h"
#include "OSD/Logger.h"
#include "OSD/Thread.h"

TCPReceive::TCPReceive(int port)
  : m_listenSocket(nullptr), m_receiveSocket(nullptr), m_socketSet(nullptr), m_running(false)
{
  SDLNet_Init();
  m_socketSet = SDLNet_AllocSocketSet(1);

  IPaddress address;
  if (SDLNet_ResolveHost(&address, nullptr, Uint16(port)) != 0)
    return;
  m_listenSocket = SDLNet_TCP_Open(&address);
  if (m_listenSocket == nullptr)
    return;
  m_running = true;
  m_listenThread = std::thread(&TCPReceive::AcceptLoop, this);
}

TCPReceive::~TCPReceive()
{
  m_running = false;
  if (m_listenThread.joinable())
    m_listenThread.join();
  if (m_listenSocket)
    SDLNet_TCP_Close(m_listenSocket);
  m_listenSocket = nullptr;
  if (TCPsocket s = m_receiveSocket.exchange(nullptr))
    SDLNet_TCP_Close(s);
  if (m_socketSet)
    SDLNet_FreeSocketSet(m_socketSet);
  m_socketSet = nullptr;
  SDLNet_Quit();
}

// Waits for the previous machine to connect, one connection at a time; after
// a connection drops, the next one is accepted.
void TCPReceive::AcceptLoop()
{
  while (m_running)
  {
    CThread::Sleep(16);
    if (m_receiveSocket)
      continue;
    TCPsocket s = SDLNet_TCP_Accept(m_listenSocket);
    if (s)
    {
      SDLNet_TCP_AddSocket(m_socketSet, s);
      m_receiveSocket = s;
    }
  }
}

bool TCPReceive::Connected()
{
  return m_receiveSocket != nullptr;
}

bool TCPReceive::CheckDataAvailable(int timeoutMS)
{
  if (!m_receiveSocket)
    return false;
  return SDLNet_CheckSockets(m_socketSet, Uint32(timeoutMS)) > 0;
}

void TCPReceive::Drop()
{
  if (TCPsocket s = m_receiveSocket.exchange(nullptr))
  {
    SDLNet_TCP_DelSocket(m_socketSet, s);
    SDLNet_TCP_Close(s);
  }
}

// Reads exactly bytes bytes; false (and the connection dropped) if it ends.
bool TCPReceive::ReadAll(void *dst, int bytes)
{
  char *p = static_cast<char *>(dst);
  while (bytes > 0)
  {
    TCPsocket s = m_receiveSocket;
    int got = s ? SDLNet_TCP_Recv(s, p, bytes) : 0;
    if (got <= 0)
    {
      Drop();
      return false;
    }
    p += got;
    bytes -= got;
  }
  return true;
}

std::vector<char> &TCPReceive::Receive()
{
  m_recBuffer.clear();
  if (!m_receiveSocket)
    return m_recBuffer;

  int length = 0;
  if (!ReadAll(&length, sizeof(length)) || length <= 0)
    return m_recBuffer;

  // If the link breaks part way, the message keeps its length: what arrived,
  // then zeros.
  m_recBuffer.resize(size_t(length));
  ReadAll(m_recBuffer.data(), length);
  return m_recBuffer;
}
