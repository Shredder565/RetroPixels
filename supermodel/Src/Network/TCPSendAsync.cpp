// Original code for RetroPixels (Supermodel). Not derived from Supermodel.

#include "TCPSendAsync.h"
#include "OSD/Logger.h"

#include <cstdint>
#include <cstring>

TCPSendAsync::TCPSendAsync(std::string &ip, int port)
  : m_ip(ip), m_port(port), m_socket(nullptr), m_quit(false)
{
  SDLNet_Init();
  m_sendThread = std::thread(&TCPSendAsync::SendLoop, this);
}

TCPSendAsync::~TCPSendAsync()
{
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.clear();
    m_quit = true;
  }
  m_cv.notify_all();
  if (m_sendThread.joinable())
    m_sendThread.join();
  if (TCPsocket s = m_socket.exchange(nullptr))
    SDLNet_TCP_Close(s);
  SDLNet_Quit();
}

bool TCPSendAsync::Connected()
{
  return m_socket != nullptr;
}

bool TCPSendAsync::Connect()
{
  IPaddress address;
  if (SDLNet_ResolveHost(&address, m_ip.c_str(), Uint16(m_port)) == 0)
    m_socket = SDLNet_TCP_Open(&address);
  return Connected();
}

bool TCPSendAsync::Send(const void *data, int length)
{
  if (!Connected())
    return false;
  if (length == 0)
    return true;

  // Length and data in one piece, so the thread sends it in one call.
  std::vector<char> message(sizeof(int32_t) + size_t(length));
  int32_t n = length;
  memcpy(message.data(), &n, sizeof(n));
  memcpy(message.data() + sizeof(n), data, size_t(length));
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(std::move(message));
  }
  m_cv.notify_one();
  return true;
}

void TCPSendAsync::SendLoop()
{
  for (;;)
  {
    std::vector<char> message;
    {
      std::unique_lock<std::mutex> lock(m_mutex);
      m_cv.wait(lock, [this] { return m_quit || !m_queue.empty(); });
      if (m_queue.empty())
        return;   // quitting
      message = std::move(m_queue.front());
      m_queue.pop_front();
    }
    TCPsocket s = m_socket;
    int size = int(message.size());
    if (!s || SDLNet_TCP_Send(s, message.data(), size) < size)
    {
      if (TCPsocket old = m_socket.exchange(nullptr))
        SDLNet_TCP_Close(old);
      return;
    }
  }
}
