// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// TCPSend with the sending done on a thread of its own: Send() queues the
// message (length-prefixed, like TCPSend) and returns at once.

#ifndef INCLUDED_TCPSENDASYNC_H
#define INCLUDED_TCPSENDASYNC_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "SDLIncludes.h"

class TCPSendAsync
{
public:
  TCPSendAsync(std::string &ip, int port);
  ~TCPSendAsync();

  bool Send(const void *data, int length);
  bool Connect();
  bool Connected();

private:
  void SendLoop();

  std::string m_ip;
  int m_port;
  std::atomic<TCPsocket> m_socket;
  std::mutex m_mutex;
  std::condition_variable m_cv;
  std::deque<std::vector<char>> m_queue;
  bool m_quit;
  std::thread m_sendThread;
};

#endif // INCLUDED_TCPSENDASYNC_H
