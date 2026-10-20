// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/SocketContext.h"
#include "Common/WorkQueueThread.h"
#include "Core/IOS/Device.h"

#ifdef _WIN32
#include <ws2tcpip.h>
#endif

// Poll events WSAPoll rejects; they are never passed to it.
#ifdef _WIN32
constexpr int UNSUPPORTED_WSAPOLL = POLLPRI | POLLWRBAND;
#else
constexpr int UNSUPPORTED_WSAPOLL = 0;
#endif

namespace IOS::HLE
{
// Commands of /dev/net/ip/top (the IOCTLV_ ones come as ioctlv).
enum NET_IOCTL
{
  IOCTL_SO_ACCEPT = 1,
  IOCTL_SO_BIND,
  IOCTL_SO_CLOSE,
  IOCTL_SO_CONNECT,
  IOCTL_SO_FCNTL,
  IOCTL_SO_GETPEERNAME,
  IOCTL_SO_GETSOCKNAME,
  IOCTL_SO_GETSOCKOPT,
  IOCTL_SO_SETSOCKOPT,
  IOCTL_SO_LISTEN,
  IOCTL_SO_POLL,
  IOCTLV_SO_RECVFROM,
  IOCTLV_SO_SENDTO,
  IOCTL_SO_SHUTDOWN,
  IOCTL_SO_SOCKET,
  IOCTL_SO_GETHOSTID,
  IOCTL_SO_GETHOSTBYNAME,
  IOCTL_SO_GETHOSTBYADDR,
  IOCTLV_SO_GETNAMEINFO,
  IOCTL_SO_GETLASTERROR,
  IOCTL_SO_INETATON,
  IOCTL_SO_INETPTON,
  IOCTL_SO_INETNTOP,
  IOCTLV_SO_GETADDRINFO,
  IOCTL_SO_SOCKATMARK,
  IOCTLV_SO_STARTUP,
  IOCTLV_SO_CLEANUP,
  IOCTLV_SO_GETINTERFACEOPT,
  IOCTLV_SO_SETINTERFACEOPT,
  IOCTL_SO_SETINTERFACE,
  IOCTL_SO_INITINTERFACE,
  IOCTL_SO_ICMPSOCKET = 0x30,
  IOCTLV_SO_ICMPPING,
  IOCTL_SO_ICMPCANCEL,
  IOCTL_SO_ICMPCLOSE
};

// /dev/net/ip/top: the socket API. Calls that can block are queued on the socket manager;
// name lookups run on a worker thread.
class NetIPTopDevice : public EmulationDevice
{
public:
  NetIPTopDevice(EmulationKernel& ios, const std::string& device_name);

  void DoState(PointerWrap& p) override;
  std::optional<IPCReply> IOCtl(const IOCtlRequest& request) override;
  std::optional<IPCReply> IOCtlV(const IOCtlVRequest& request) override;

  void Update() override;

private:
  struct AsyncTask
  {
    IOS::HLE::Request request;
    std::function<IPCReply()> handler;
  };

  struct AsyncReply
  {
    IOS::HLE::Request request;
    s32 return_value;
  };

  // Runs `handler` on the worker thread; its result is sent from Update().
  template <typename RequestType>
  std::optional<IPCReply> RunOnWorker(IPCReply (NetIPTopDevice::*handler)(const RequestType&),
                                      const RequestType& request)
  {
    m_work_queue.EmplaceItem(
        AsyncTask{request, [this, handler, request] { return (this->*handler)(request); }});
    return std::nullopt;
  }

  IPCReply InitInterface(const IOCtlRequest& request);
  IPCReply OpenSocket(const IOCtlRequest& request);
  IPCReply OpenICMPSocket(const IOCtlRequest& request);
  IPCReply CloseSocket(const IOCtlRequest& request);
  std::optional<IPCReply> QueueOnSocket(const IOCtlRequest& request);
  IPCReply Shutdown(const IOCtlRequest& request);
  IPCReply Listen(const IOCtlRequest& request);
  IPCReply GetSockOpt(const IOCtlRequest& request);
  IPCReply SetSockOpt(const IOCtlRequest& request);
  IPCReply GetSockName(const IOCtlRequest& request);
  IPCReply GetPeerName(const IOCtlRequest& request);
  IPCReply GetHostID(const IOCtlRequest& request);
  IPCReply InetAToN(const IOCtlRequest& request);
  IPCReply InetPToN(const IOCtlRequest& request);
  IPCReply InetNToP(const IOCtlRequest& request);
  std::optional<IPCReply> Poll(const IOCtlRequest& request);
  IPCReply GetHostByName(const IOCtlRequest& request);
  IPCReply ICMPCancel(const IOCtlRequest& request);

  IPCReply GetInterfaceOpt(const IOCtlVRequest& request);
  std::optional<IPCReply> SendTo(const IOCtlVRequest& request);
  std::optional<IPCReply> RecvFrom(const IOCtlVRequest& request);
  IPCReply GetAddrInfo(const IOCtlVRequest& request);
  IPCReply ICMPPing(const IOCtlVRequest& request);

  Common::SocketContext m_socket_context;
  Common::WorkQueueThreadSP<AsyncTask> m_work_queue;
  std::mutex m_async_reply_lock;
  std::queue<AsyncReply> m_async_replies;
};
}  // namespace IOS::HLE
