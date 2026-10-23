// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// The sending half of a link between linked cabinets: a TCP connection to
// the next machine in the ring. Each message goes out as a 4-byte length
// (the sender's own byte order, as every Supermodel build does) followed by
// that many bytes.

#ifndef INCLUDED_TCPSEND_H
#define INCLUDED_TCPSEND_H

#include <string>
#include "SDLIncludes.h"

class TCPSend
{
public:
  TCPSend(std::string &ip, int port);
  ~TCPSend();

  // Sends one message. False when not connected; a failed send drops the
  // connection.
  bool Send(const void *data, int length);

  // Tries once to connect; true if connected.
  bool Connect();
  bool Connected();

private:
  std::string m_ip;
  int m_port;
  TCPsocket m_socket;
};

#endif // INCLUDED_TCPSEND_H
