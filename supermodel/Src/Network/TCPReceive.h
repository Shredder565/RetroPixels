// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The receiving half of a link between linked cabinets: listens on a port
// and accepts the previous machine's connection (on a background thread),
// then reads the length-prefixed messages TCPSend sends.

#ifndef INCLUDED_TCPRECEIVE_H
#define INCLUDED_TCPRECEIVE_H

#include <atomic>
#include <thread>
#include <vector>
#include "SDLIncludes.h"

class TCPReceive
{
public:
  TCPReceive(int port);
  ~TCPReceive();

  // True if a message is waiting. timeoutMS: 0 don't wait, -1 wait as long
  // as it takes, otherwise the most milliseconds to wait.
  bool CheckDataAvailable(int timeoutMS = 0);

  // Blocks until the next message has arrived and returns it. Empty when
  // there is no connection or it broke (it is then closed).
  std::vector<char> &Receive();

  bool Connected();

private:
  void AcceptLoop();
  void Drop();
  bool ReadAll(void *dst, int bytes);

  TCPsocket m_listenSocket;
  std::atomic<TCPsocket> m_receiveSocket;
  SDLNet_SocketSet m_socketSet;
  std::thread m_listenThread;
  std::atomic_bool m_running;
  std::vector<char> m_recBuffer;
};

#endif // INCLUDED_TCPRECEIVE_H
