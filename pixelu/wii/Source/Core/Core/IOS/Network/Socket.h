// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// Host socket headers. Other files (the IP device, the Triforce media board) rely on this header
// for them, for pollfd_t and for poll() being available on Windows.
#ifdef _WIN32
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>

using pollfd_t = pollfd;

#define poll WSAPoll
#define MALLOC(x) HeapAlloc(GetProcessHeap(), 0, (x))
#define FREE(x) HeapFree(GetProcessHeap(), 0, (x))

#elif defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) ||                          \
    defined(__NetBSD__) || defined(__OpenBSD__) || defined(__HAIKU__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

using pollfd_t = struct pollfd;
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include <chrono>
#include <list>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/Network/IP/Top.h"
#include "Core/IOS/Network/SSL.h"

namespace IOS::HLE
{
// Number of sockets a title can have open at once.
constexpr int WII_SOCKET_FD_MAX = 24;

// Flags of send/recv as the Wii passes them.
enum
{
  SO_MSG_OOB = 0x01,
  SO_MSG_PEEK = 0x02,
  SO_MSG_NONBLOCK = 0x04,
};

// The Wii's socket error numbers. Calls report them negated.
enum
{
  SO_SUCCESS,
  SO_E2BIG = 1,
  SO_EACCES,
  SO_EADDRINUSE,
  SO_EADDRNOTAVAIL,
  SO_EAFNOSUPPORT,
  SO_EAGAIN,
  SO_EALREADY,
  SO_EBADF,
  SO_EBADMSG,
  SO_EBUSY,
  SO_ECANCELED,
  SO_ECHILD,
  SO_ECONNABORTED,
  SO_ECONNREFUSED,
  SO_ECONNRESET,
  SO_EDEADLK,
  SO_EDESTADDRREQ,
  SO_EDOM,
  SO_EDQUOT,
  SO_EEXIST,
  SO_EFAULT,
  SO_EFBIG,
  SO_EHOSTUNREACH,
  SO_EIDRM,
  SO_EILSEQ,
  SO_EINPROGRESS,
  SO_EINTR,
  SO_EINVAL,
  SO_EIO,
  SO_EISCONN,
  SO_EISDIR,
  SO_ELOOP,
  SO_EMFILE,
  SO_EMLINK,
  SO_EMSGSIZE,
  SO_EMULTIHOP,
  SO_ENAMETOOLONG,
  SO_ENETDOWN,
  SO_ENETRESET,
  SO_ENETUNREACH,
  SO_ENFILE,
  SO_ENOBUFS,
  SO_ENODATA,
  SO_ENODEV,
  SO_ENOENT,
  SO_ENOEXEC,
  SO_ENOLCK,
  SO_ENOLINK,
  SO_ENOMEM,
  SO_ENOMSG,
  SO_ENOPROTOOPT,
  SO_ENOSPC,
  SO_ENOSR,
  SO_ENOSTR,
  SO_ENOSYS,
  SO_ENOTCONN,
  SO_ENOTDIR,
  SO_ENOTEMPTY,
  SO_ENOTSOCK,
  SO_ENOTSUP,
  SO_ENOTTY,
  SO_ENXIO,
  SO_EOPNOTSUPP,
  SO_EOVERFLOW,
  SO_EPERM,
  SO_EPIPE,
  SO_EPROTO,
  SO_EPROTONOSUPPORT,
  SO_EPROTOTYPE,
  SO_ERANGE,
  SO_EROFS,
  SO_ESPIPE,
  SO_ESRCH,
  SO_ESTALE,
  SO_ETIME,
  SO_ETIMEDOUT,
  SO_ETXTBSY,
  SO_EXDEV
};

// Socket addresses in guest memory (BSD layout with a length byte, network byte order).
#pragma pack(push, 1)
struct WiiInAddr
{
  u32 addr;
};

struct WiiSockAddr
{
  u8 len;
  u8 family;
  u8 data[6];
};

struct WiiSockAddrIn
{
  u8 len;
  u8 family;
  u16 port;
  WiiInAddr addr;
};
#pragma pack(pop)

class WiiSockMan;

// One guest socket, backed by a non-blocking host socket. Requests that can block are queued
// here and retried on every update until they finish (or straight away for non-blocking use).
class WiiSocket
{
public:
  explicit WiiSocket(WiiSockMan& manager) : m_manager(manager) {}
  WiiSocket(const WiiSocket&) = delete;
  WiiSocket(WiiSocket&&) = delete;
  WiiSocket& operator=(const WiiSocket&) = delete;
  WiiSocket& operator=(WiiSocket&&) = delete;
  ~WiiSocket();

private:
  friend class WiiSockMan;

  using Deadline = std::chrono::steady_clock::time_point;

  struct QueuedRequest
  {
    Request request;
    bool is_ssl = false;
    bool aborted = false;
    // A NET_IOCTL, or an SSL_IOCTL if is_ssl is set.
    u32 command = 0;
  };

  enum class ConnectingState
  {
    None,
    Connecting,
    Connected,
    Error
  };

  void AdoptHostSocket(s32 host_fd, s32 guest_fd);
  s32 Shutdown(u32 how);
  s32 Close();
  s32 FCntl(u32 cmd, u32 arg);
  void Abort(QueuedRequest& queued, s32 result) const;

  void Queue(const Request& request, NET_IOCTL type);
  void Queue(const Request& request, SSL_IOCTL type);

  // Retries every queued request once.
  void Update();
  s32 RunIOCtl(const Request& request, u32 command);
  s32 RunSSL(const Request& request, u32 command);
  s32 RunSendTo(const Request& request, bool* force_non_blocking);
  s32 RunRecvFrom(const Request& request, bool* force_non_blocking);
  s32 AdjustBlockingConnect(s32 result);

  void SetConnectingState(s32 connect_result);
  ConnectingState PollConnectingState() const;
  bool IsValid() const { return m_host_fd >= 0; }
  bool IsTCP() const;
  const Deadline& ConnectDeadline();

  WiiSockMan& m_manager;

  s32 m_host_fd = -1;
  s32 m_guest_fd = -1;
  bool m_non_blocking = false;
  ConnectingState m_connecting = ConnectingState::None;
  std::list<QueuedRequest> m_queue;
  std::optional<Deadline> m_connect_deadline;
};

// The socket table shared by /dev/net/ip/top, /dev/net/ssl and KD.
class WiiSockMan
{
public:
  enum class ConvertDirection
  {
    WiiToNative,
    NativeToWii
  };

  // A poll that waits (without blocking emulation) until something is ready or it times out.
  struct PollCommand
  {
    u32 request_addr = 0;
    u32 buffer_out = 0;
    std::vector<pollfd_t> wii_fds;
    s64 timeout = 0;
  };

  explicit WiiSockMan(EmulationKernel& ios);
  WiiSockMan(const WiiSockMan&) = delete;
  WiiSockMan& operator=(const WiiSockMan&) = delete;
  WiiSockMan(WiiSockMan&&) = delete;
  WiiSockMan& operator=(WiiSockMan&&) = delete;
  ~WiiSockMan();

  // Turns a host call result into a Wii result, reading the host error when it failed.
  s32 GetNetErrorCode(s32 ret, std::string_view caller, bool is_rw);

  void Update();
  static sockaddr_in ToNativeAddrIn(WiiSockAddrIn from);
  static WiiSockAddrIn ToWiiAddrIn(const sockaddr_in& from,
                                   socklen_t addrlen = sizeof(WiiSockAddrIn));
  static s32 ConvertEvents(s32 events, ConvertDirection dir);

  void DoState(PointerWrap& p);
  void AddPollCommand(const PollCommand& cmd);

  // These never block.
  s32 NewSocket(s32 af, s32 type, s32 protocol);
  s32 AddSocket(s32 fd, bool is_rw);
  bool IsSocketBlocking(s32 wii_fd) const;
  s32 GetHostSocket(s32 wii_fd) const;
  s32 ShutdownSocket(s32 wii_fd, u32 how);
  s32 DeleteSocket(s32 wii_fd);
  s32 GetLastNetError() const { return m_last_error; }
  void SetLastNetError(s32 error) { m_last_error = error; }
  void Clean() { m_sockets.clear(); }
  void EnqueueIPCReply(const Request& request, s32 return_value) const;

  // Queues a request on a socket; it is answered from Update().
  template <typename T>
  void DoSock(s32 sock, const Request& request, T type)
  {
    const auto found = m_sockets.find(sock);
    if (found != m_sockets.end())
    {
      found->second.Queue(request, type);
      return;
    }
    ERROR_LOG_FMT(IOS_NET, "DoSock: no socket {:08x} (request {:08X}, command {:08X})", sock,
                  request.address, std::to_underlying(type));
    EnqueueIPCReply(request, -SO_EBADF);
  }

  void UpdateWantDeterminism(bool want);

private:
  friend class WiiSocket;

  void UpdatePollCommands();

  EmulationKernel& m_ios;
  std::unordered_map<s32, WiiSocket> m_sockets;
  s32 m_last_error = 0;
  std::vector<PollCommand> m_polls;
  std::chrono::time_point<std::chrono::high_resolution_clock> m_last_poll_update =
      std::chrono::high_resolution_clock::now();
};
}  // namespace IOS::HLE
