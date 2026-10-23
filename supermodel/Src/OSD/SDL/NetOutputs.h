// Original code for RetroPixels (Supermodel). Not derived from Supermodel.
//
// Outputs for MAMEHooker and similar tools over the network. On start a UDP
// broadcast announces "mame_start = <game>" and "tcp = <port>"; tools then
// connect to that TCP port and receive "<name> = <value>" lines (each ended
// by "\r", or "\r\n" with OutputsWithLF) for every output change, after the
// game's name and the current values. "mame_stop = 1" is sent before a
// client is dropped.

#pragma once

#include "Logger.h"
#include "Outputs.h"
#include "SDLIncludes.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CNetOutputs : public COutputs
{
public:
  CNetOutputs();
  virtual ~CNetOutputs();

  // Opens the TCP port and starts accepting clients.
  bool Initialize();

  // Sends the UDP announcement.
  void Attached();

  void SetFrameEnding(const std::string &ending) { m_lineEnd = ending; }
  void SetTcpPort(unsigned int port) { m_tcpPort = port; }
  void SetUdpBroadcastPort(unsigned int port) { m_udpPort = port; }

protected:
  void SendOutput(EOutputs output, UINT8 prevValue, UINT8 value);

private:
  std::string Line(const std::string &name, const std::string &value) const;
  bool Write(TCPsocket client, const std::string &text);
  void Welcome(TCPsocket client);
  bool SendAll(TCPsocket client);
  void Drop(TCPsocket client);
  void AcceptLoop();

  unsigned int m_tcpPort = 8000;
  unsigned int m_udpPort = 8001;
  std::string m_lineEnd = "\r";

  TCPsocket m_server = nullptr;
  SDLNet_SocketSet m_set = nullptr;
  std::vector<TCPsocket> m_clients;
  std::recursive_mutex m_lock;      // m_clients, used by both threads
  std::thread m_acceptThread;
  std::atomic<bool> m_running{ false };
};
