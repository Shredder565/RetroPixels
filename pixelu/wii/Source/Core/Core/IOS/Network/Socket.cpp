// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// Guest sockets are not available during NetPlay or TAS movies (see UpdateWantDeterminism).
#include "Core/IOS/Network/Socket.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>

#include <mbedtls/error.h>
#ifndef _WIN32
#include <arpa/inet.h>
#include <unistd.h>
#endif
#ifdef __HAIKU__
#include <sys/select.h>
#endif

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Network.h"
#include "Common/ScopeGuard.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/IOS/Device.h"
#include "Core/IOS/IOS.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/WC24PatchEngine.h"

namespace IOS::HLE
{
namespace
{
#ifdef _WIN32
using HostSocket = SOCKET;
using HostLength = int;
using PollCount = ULONG;
constexpr int HOST_WOULD_BLOCK = WSAEWOULDBLOCK;
constexpr int HOST_NOT_A_SOCKET = WSAENOTSOCK;
constexpr int HOST_MSG_SIZE = WSAEMSGSIZE;

int LastHostError()
{
  return WSAGetLastError();
}

int CloseHost(s32 fd)
{
  return closesocket(static_cast<SOCKET>(fd));
}
#else
using HostSocket = int;
using HostLength = size_t;
using PollCount = nfds_t;
constexpr int HOST_WOULD_BLOCK = EAGAIN;
constexpr int HOST_NOT_A_SOCKET = EBADF;
constexpr int HOST_MSG_SIZE = EMSGSIZE;

int LastHostError()
{
  return errno;
}

int CloseHost(s32 fd)
{
  return close(fd);
}
#endif

HostSocket Host(s32 fd)
{
  return static_cast<HostSocket>(fd);
}

// fcntl commands and flag as the Wii numbers them. Hosts that define F_GETFL/F_SETFL use their
// own values, which match these on all supported desktop systems.
#ifdef F_GETFL
constexpr u32 WII_F_GETFL = F_GETFL;
#else
constexpr u32 WII_F_GETFL = 3;
#endif
#ifdef F_SETFL
constexpr u32 WII_F_SETFL = F_SETFL;
#else
constexpr u32 WII_F_SETFL = 4;
#endif
constexpr u32 WII_O_NONBLOCK = 4;

struct ErrorMapping
{
  int host;
  s32 wii;
};

#ifdef _WIN32
#define HOST_ERRNO(name) WSA##name
#else
#define HOST_ERRNO(name) name
#endif

constexpr ErrorMapping ERROR_MAP[] = {
    {HOST_NOT_A_SOCKET, -SO_EBADF},
    {HOST_ERRNO(EADDRINUSE), -SO_EADDRINUSE},
    {HOST_ERRNO(ECONNRESET), -SO_ECONNRESET},
    {HOST_ERRNO(EISCONN), -SO_EISCONN},
    {HOST_ERRNO(ENOTCONN), -SO_ENOTCONN},
    {HOST_ERRNO(EINPROGRESS), -SO_EINPROGRESS},
    {HOST_ERRNO(EALREADY), -SO_EALREADY},
    {HOST_ERRNO(EACCES), -SO_EACCES},
    {HOST_ERRNO(ECONNREFUSED), -SO_ECONNREFUSED},
    {HOST_ERRNO(ENETUNREACH), -SO_ENETUNREACH},
    {HOST_ERRNO(EHOSTUNREACH), -SO_EHOSTUNREACH},
    // Out of buffer space is reported like out of memory (see ip(7)).
    {ENOMEM, -SO_ENOMEM},
    {HOST_ERRNO(ENOBUFS), -SO_ENOMEM},
    {HOST_ERRNO(ENETRESET), -SO_ENETRESET},
};

#undef HOST_ERRNO

// Host error number -> Wii result. "Would block" means EAGAIN for reads and writes but
// EINPROGRESS for everything else (connect on Windows reports it that way).
s32 HostErrorToWii(int host_error, bool is_rw)
{
  if (host_error == HOST_MSG_SIZE)
  {
    // The Wii would say -SO_EMSGSIZE; this has only been seen as a failed peek.
    ERROR_LOG_FMT(IOS_NET, "Unexpected EMSGSIZE (failed peek?)");
    return -1;
  }
  if (host_error == HOST_WOULD_BLOCK)
    return is_rw ? -SO_EAGAIN : -SO_EINPROGRESS;
  for (const ErrorMapping& entry : ERROR_MAP)
  {
    if (entry.host == host_error)
      return entry.wii;
  }
  return -1;
}

bool IsStillConnecting(s32 result)
{
  return result == -SO_EAGAIN || result == -SO_EALREADY || result == -SO_EINPROGRESS;
}

// Host socket type, or nothing if it cannot be read.
std::optional<int> HostSocketType(s32 fd)
{
  int type = 0;
  socklen_t length = sizeof(type);
  if (getsockopt(Host(fd), SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length) != 0)
    return std::nullopt;
  return type;
}

// Runs `f` without disturbing errno / WSAGetLastError.
template <typename F>
auto KeepingNetworkError(F f)
{
  const auto saved = Common::SaveNetworkErrorState();
  Common::ScopeGuard restore([&saved] { Common::RestoreNetworkErrorState(saved); });
  return f();
}

std::shared_ptr<Core::NetworkCaptureLogger> CaptureLogger(Core::System& system)
{
  return system.GetPowerPC().GetDebugInterface().NetworkLogger();
}

// SSL results for the mbedtls "try again" codes; anything else is a plain failure.
// `blocking_result` gets the code too when the socket blocks, so the request is retried.
void WriteSSLRetryOrFailure(Memory::MemoryManager& memory, int ret, u32 result_addr,
                            bool blocking, s32* blocking_result)
{
  s32 code = SSL_ERR_FAILED;
  if (ret == MBEDTLS_ERR_SSL_WANT_READ)
    code = SSL_ERR_RAGAIN;
  else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    code = SSL_ERR_WAGAIN;

  WriteReturnValue(memory, code, result_addr);
  if (code != SSL_ERR_FAILED && blocking)
    *blocking_result = code;
}

// Picks the Wii's verification error from mbedtls' verify flags.
s32 VerifyFlagsToWii(u32 flags)
{
  if (flags & MBEDTLS_X509_BADCERT_CN_MISMATCH)
    return SSL_ERR_VCOMMONNAME;
  if (flags & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
    return SSL_ERR_VROOTCA;
  if (flags & MBEDTLS_X509_BADCERT_REVOKED)
    return SSL_ERR_VCHAIN;
  if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE))
    return SSL_ERR_VDATE;
  return SSL_ERR_FAILED;
}

// The first vectors of an ioctlv, or zero where the request has fewer.
struct Vectors
{
  explicit Vectors(const IOCtlVRequest& request)
  {
    if (request.in_vectors.size() > 0)
      in = request.in_vectors[0];
    if (request.in_vectors.size() > 1)
      in2 = request.in_vectors[1];
    if (request.io_vectors.size() > 0)
      out = request.io_vectors[0];
    if (request.io_vectors.size() > 1)
      out2 = request.io_vectors[1];
  }

  IOCtlVRequest::IOVector in;
  IOCtlVRequest::IOVector in2;
  IOCtlVRequest::IOVector out;
  IOCtlVRequest::IOVector out2;
};
}  // namespace

WiiSockMan::WiiSockMan(EmulationKernel& ios) : m_ios(ios)
{
}

WiiSockMan::~WiiSockMan() = default;

s32 WiiSockMan::GetNetErrorCode(s32 ret, std::string_view caller, bool is_rw)
{
  int host_error = LastHostError();
#ifdef _WIN32
  // Winsock (or a layer hooked into it, such as ReShade) can report an in-progress connect as
  // WSAEINVAL, the Winsock 1.1 spelling of WSAEALREADY.
  if (host_error == WSAEINVAL && caller == "SO_CONNECT")
    host_error = WSAEALREADY;
#endif

  if (ret >= 0)
  {
    SetLastNetError(ret);
    return ret;
  }

  const s32 result = HostErrorToWii(host_error, is_rw);
  // "Try again" is routine for non-blocking sockets.
  const auto level =
      result == -SO_EAGAIN ? Common::Log::LogLevel::LINFO : Common::Log::LogLevel::LERROR;
  GENERIC_LOG_FMT(Common::Log::LogType::IOS_NET, level, "{}: host error {} ({}), ret = {}",
                  caller, host_error, Common::DecodeNetworkError(host_error), ret);
  SetLastNetError(result);
  return result;
}

WiiSocket::~WiiSocket()
{
  if (m_host_fd >= 0)
    static_cast<void>(Close());
}

void WiiSocket::AdoptHostSocket(s32 host_fd, s32 guest_fd)
{
  if (m_host_fd >= 0)
    static_cast<void>(Close());

  m_host_fd = host_fd;
  m_guest_fd = guest_fd;
  m_non_blocking = false;

  // The host side never blocks; blocking guest calls are emulated by retrying.
#ifdef _WIN32
  u_long non_blocking = 1;
  ioctlsocket(Host(m_host_fd), static_cast<long>(FIONBIO), &non_blocking);
#else
  int flags = fcntl(m_host_fd, F_GETFL, 0);
  if (flags == -1)
    flags = 0;
  fcntl(m_host_fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

s32 WiiSocket::Shutdown(u32 how)
{
  if (how > 2)
    return -SO_EINVAL;

  const std::optional<int> type = HostSocketType(m_host_fd);
  if (!type || (*type != SOCK_STREAM && *type != SOCK_DGRAM))
    return -SO_EBADF;
  // For UDP the Wii does nothing and reports success.
  if (*type == SOCK_DGRAM)
    return SO_SUCCESS;

  const s32 result = m_manager.GetNetErrorCode(shutdown(Host(m_host_fd), static_cast<int>(how)),
                                               "SO_SHUTDOWN", false);

  // What happens to requests still waiting, as measured on hardware (dolp.in/pr8758). SSL
  // requests have not been tested and are left alone.
  const bool reads_closed = how != 1;
  const bool writes_closed = how != 0;
  for (QueuedRequest& queued : m_queue)
  {
    if (queued.is_ssl)
      continue;

    if (queued.command == IOCTL_SO_ACCEPT && writes_closed)
      Abort(queued, -SO_EINVAL);
    else if (queued.command == IOCTL_SO_CONNECT && writes_closed && !m_non_blocking)
      Abort(queued, -SO_ENETUNREACH);
    else if (queued.command == IOCTLV_SO_RECVFROM && reads_closed)
      Abort(queued, -SO_ENOTCONN);
    else if (queued.command == IOCTLV_SO_SENDTO && writes_closed)
      Abort(queued, -SO_ENOTCONN);
  }
  return result;
}

s32 WiiSocket::Close()
{
  // Without a host socket this reports the (positive) "not a socket" error number as is.
  const s32 result =
      m_host_fd >= 0 ?
          m_manager.GetNetErrorCode(CloseHost(m_host_fd), "CloseFd", false) :
          m_manager.GetNetErrorCode(HOST_NOT_A_SOCKET, "CloseFd", false);
  m_host_fd = -1;

  // Whatever was still waiting fails.
  while (!m_queue.empty())
  {
    m_manager.EnqueueIPCReply(m_queue.front().request, -SO_ENOTCONN);
    m_queue.pop_front();
  }
  m_connecting = ConnectingState::None;
  return result;
}

s32 WiiSocket::FCntl(u32 cmd, u32 arg)
{
  s32 result = 0;
  if (cmd == WII_F_GETFL)
    result = m_non_blocking ? static_cast<s32>(WII_O_NONBLOCK) : 0;
  else if (cmd == WII_F_SETFL)
    m_non_blocking = (arg & WII_O_NONBLOCK) != 0;
  else
    ERROR_LOG_FMT(IOS_NET, "SO_FCNTL: unknown command");

  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_FCNTL({:08x}, {:08X}, {:08X})", m_guest_fd, cmd, arg);
  return result;
}

void WiiSocket::Abort(QueuedRequest& queued, s32 result) const
{
  // Answered now; Update() drops it without answering again.
  queued.aborted = true;
  m_manager.EnqueueIPCReply(queued.request, result);
}

void WiiSocket::Queue(const Request& request, NET_IOCTL type)
{
  m_queue.push_back(QueuedRequest{request, false, false, static_cast<u32>(type)});
}

void WiiSocket::Queue(const Request& request, SSL_IOCTL type)
{
  m_queue.push_back(QueuedRequest{request, true, false, static_cast<u32>(type)});
}

const WiiSocket::Deadline& WiiSocket::ConnectDeadline()
{
  if (!m_connect_deadline)
  {
    m_connect_deadline = std::chrono::steady_clock::now() +
                         std::chrono::seconds(Config::Get(Config::MAIN_NETWORK_TIMEOUT));
  }
  return *m_connect_deadline;
}

s32 WiiSocket::AdjustBlockingConnect(s32 result)
{
  // A blocking connect keeps being retried until it finishes or the configured time is up.
  if (IsStillConnecting(result))
  {
    if (std::chrono::steady_clock::now() > ConnectDeadline())
    {
      m_connect_deadline.reset();
      m_connecting = ConnectingState::Error;
      return -SO_ENETUNREACH;
    }
    return result;
  }

  m_connect_deadline.reset();
  if (result == -SO_EISCONN)
  {
    // Connected by an earlier attempt.
    m_connecting = ConnectingState::Connected;
    return SO_SUCCESS;
  }
  return result;
}

s32 WiiSocket::RunIOCtl(const Request& request, u32 command)
{
  auto& system = m_manager.m_ios.GetSystem();
  auto& memory = system.GetMemory();
  IOCtlRequest ioctl{system, request.address};

  const auto read_address = [&memory](u32 address) {
    WiiSockAddrIn wii_addr;
    memory.CopyFromEmu(&wii_addr, address, sizeof(wii_addr));
    return WiiSockMan::ToNativeAddrIn(wii_addr);
  };

  s32 result = 0;
  switch (command)
  {
  case IOCTL_SO_FCNTL:
    result = FCntl(memory.Read_U32(ioctl.buffer_in + 4), memory.Read_U32(ioctl.buffer_in + 8));
    break;

  case IOCTL_SO_BIND:
  {
    sockaddr_in local = read_address(ioctl.buffer_in + 8);
    const int ret = bind(Host(m_host_fd), reinterpret_cast<sockaddr*>(&local), sizeof(local));
    result = m_manager.GetNetErrorCode(ret, "SO_BIND", false);
    INFO_LOG_FMT(IOS_NET, "IOCTL_SO_BIND ({:08X}, {}:{}) = {}", m_guest_fd,
                 inet_ntoa(local.sin_addr), Common::swap16(local.sin_port), ret);
    break;
  }

  case IOCTL_SO_CONNECT:
  {
    sockaddr_in remote = read_address(ioctl.buffer_in + 8);
    const int ret = connect(Host(m_host_fd), reinterpret_cast<sockaddr*>(&remote), sizeof(remote));
    result = m_manager.GetNetErrorCode(ret, "SO_CONNECT", false);
    SetConnectingState(result);
    INFO_LOG_FMT(IOS_NET, "IOCTL_SO_CONNECT ({:08x}, {}:{}) = {}", m_guest_fd,
                 inet_ntoa(remote.sin_addr), Common::swap16(remote.sin_port), ret);
    break;
  }

  case IOCTL_SO_ACCEPT:
  {
    s32 accepted;
    if (ioctl.buffer_out_size > 0)
    {
      // The peer address goes back into the output buffer, even when nothing was accepted.
      sockaddr_in peer = read_address(ioctl.buffer_out);
      socklen_t peer_length = sizeof(sockaddr_in);
      accepted = static_cast<s32>(
          accept(Host(m_host_fd), reinterpret_cast<sockaddr*>(&peer), &peer_length));
      const WiiSockAddrIn wii_peer = WiiSockMan::ToWiiAddrIn(peer, peer_length);
      memory.CopyToEmu(ioctl.buffer_out, &wii_peer, sizeof(wii_peer));
    }
    else
    {
      accepted = static_cast<s32>(accept(Host(m_host_fd), nullptr, nullptr));
    }
    result = m_manager.AddSocket(accepted, true);
    ioctl.Log("IOCTL_SO_ACCEPT", Common::Log::LogType::IOS_NET);
    break;
  }

  default:
    break;
  }

  if (command == IOCTL_SO_CONNECT && !m_non_blocking)
    result = AdjustBlockingConnect(result);
  return result;
}

s32 WiiSocket::RunSSL(const Request& request, u32 command)
{
  auto& system = m_manager.m_ios.GetSystem();
  auto& memory = system.GetMemory();
  const IOCtlVRequest ioctlv{system, request.address};
  const Vectors v{ioctlv};

  // The SSL result always goes to the first input vector; the IPC result only matters for
  // blocking sockets, where a "try again" keeps the request queued.
  const u32 result_addr = v.in.address;
  const bool blocking = !m_non_blocking;
  s32 result = 0;

  const int ssl_id = static_cast<int>(memory.Read_U32(v.out.address) - 1);
  if (!IsSSLIDValid(ssl_id))
  {
    WriteReturnValue(memory, SSL_ERR_ID, result_addr);
    return result;
  }
  WII_SSL& ssl = NetSSLDevice::_SSL[ssl_id];

  switch (command)
  {
  case IOCTLV_NET_SSL_DOHANDSHAKE:
  {
    // The Wii lets a handshake start while the TCP connect is still going on; mbedtls does
    // not, so wait for the connection here.
    m_connecting = PollConnectingState();
    if (m_connecting == ConnectingState::Connecting)
    {
      WriteReturnValue(memory, SSL_ERR_RAGAIN, result_addr);
      return SSL_ERR_RAGAIN;
    }
    if (m_connecting != ConnectingState::Connected)
    {
      WriteReturnValue(memory, SSL_ERR_SYSCALL, result_addr);
      return SSL_ERR_SYSCALL;
    }

    mbedtls_ssl_context* const context = &ssl.ctx;
    const int ret = mbedtls_ssl_handshake(context);
    if (ret != 0)
    {
      char message[256] = "";
      mbedtls_strerror(ret, message, sizeof(message));
      ERROR_LOG_FMT(IOS_SSL, "IOCTLV_NET_SSL_DOHANDSHAKE: {}", message);
    }

    if (ret == 0)
    {
      WriteReturnValue(memory, SSL_OK, result_addr);
    }
    else if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
    {
      const u32 flags = mbedtls_ssl_get_verify_result(context);
      char details[256] = "";
      mbedtls_x509_crt_verify_info(details, sizeof(details), "", flags);
      ERROR_LOG_FMT(IOS_SSL, "Certificate verification failed (verify_result = {}): {}",
                    static_cast<int>(flags), details);

      const s32 code = VerifyFlagsToWii(flags);
      WriteReturnValue(memory, code, result_addr);
      if (blocking)
        result = code;
    }
    else
    {
      WriteSSLRetryOrFailure(memory, ret, result_addr, blocking, &result);
    }

    // The peer certificate is taken from the session being negotiated, since
    // mbedtls_ssl_get_peer_cert() has nothing after a failed handshake.
    if (Config::Get(Config::MAIN_NETWORK_SSL_DUMP_PEER_CERT) &&
        context->session_negotiate != nullptr)
    {
      if (const mbedtls_x509_crt* peer = context->session_negotiate->peer_cert)
      {
        const std::string host = context->hostname != nullptr ? context->hostname : "";
        File::IOFile(File::GetUserPath(D_DUMPSSL_IDX) + host + "_peercert.der", "wb")
            .WriteBytes(peer->raw.p, peer->raw.len);
      }
    }

    INFO_LOG_FMT(IOS_SSL,
                 "IOCTLV_NET_SSL_DOHANDSHAKE = {}, in ({:08x}, {}), in2 ({:08x}, {}), "
                 "out ({:08x}, {}), out2 ({:08x}, {})",
                 ret, v.in.address, v.in.size, v.in2.address, v.in2.size, v.out.address,
                 v.out.size, v.out2.address, v.out2.size);
    break;
  }

  case IOCTLV_NET_SSL_WRITE:
  case IOCTLV_NET_SSL_READ:
  {
    const bool is_write = command == IOCTLV_NET_SSL_WRITE;
    // Data to send is in the second output vector; received data goes to the second input.
    const IOCtlVRequest::IOVector& data = is_write ? v.out2 : v.in2;
    u8* const buffer = memory.GetPointerForRange(data.address, data.size);
    const int ret = is_write ? mbedtls_ssl_write(&ssl.ctx, buffer, data.size) :
                               mbedtls_ssl_read(&ssl.ctx, buffer, data.size);
    if (ret < 0)
    {
      WriteSSLRetryOrFailure(memory, ret, result_addr, blocking, &result);
      break;
    }

    const u8* const done = memory.GetPointerForRange(data.address, static_cast<u32>(ret));
    if (is_write)
      CaptureLogger(system)->LogSSLWrite(done, static_cast<size_t>(ret), ssl.hostfd);
    else
      CaptureLogger(system)->LogSSLRead(done, static_cast<size_t>(ret), ssl.hostfd);
    // The byte count, with zero reported as its own code.
    WriteReturnValue(memory, ret == 0 ? SSL_ERR_ZERO : ret, result_addr);
    break;
  }

  default:
    break;
  }
  return result;
}

s32 WiiSocket::RunSendTo(const Request& request, bool* force_non_blocking)
{
  auto& system = m_manager.m_ios.GetSystem();
  auto& memory = system.GetMemory();
  const IOCtlVRequest ioctlv{system, request.address};
  const Vectors v{ioctlv};

  // A TCP socket may send while it is still connecting on the Wii, but not on every host.
  m_connecting = PollConnectingState();
  if (m_non_blocking && IsTCP() && m_connecting == ConnectingState::Connecting)
    return -SO_EAGAIN;

  // Second input vector: +4 flags, +8 "has destination", +0xc destination address.
  const u32 params = v.in2.address;
  u32 flags = memory.Read_U32(params + 4);
  const bool has_destination = memory.Read_U32(params + 8) != 0;

  const char* payload =
      reinterpret_cast<const char*>(memory.GetPointerForRange(v.in.address, v.in.size));
  u32 payload_size = v.in.size;
  const std::optional<std::string> patched =
      WC24PatchEngine::GetNetworkPatchByPayload(std::string_view{payload, payload_size});
  if (patched)
  {
    payload = patched->c_str();
    payload_size = static_cast<u32>(patched->size());
  }

  *force_non_blocking = (flags & SO_MSG_NONBLOCK) != 0;
  // Out-of-band is the only send flag the host gets.
  flags &= SO_MSG_OOB;

  sockaddr_in destination{};
  if (has_destination)
  {
    WiiSockAddrIn wii_destination;
    memory.CopyFromEmu(&wii_destination, params + 0xc, sizeof(wii_destination));
    destination = WiiSockMan::ToNativeAddrIn(wii_destination);
  }
  sockaddr* const to = has_destination ? reinterpret_cast<sockaddr*>(&destination) : nullptr;
  const socklen_t to_length = has_destination ? sizeof(sockaddr) : 0;

  const int sent = static_cast<int>(sendto(Host(m_host_fd), payload,
                                           static_cast<HostLength>(payload_size),
                                           static_cast<int>(flags), to, to_length));
  const s32 result = m_manager.GetNetErrorCode(sent, "SO_SENDTO", true);
  if (sent > 0)
    CaptureLogger(system)->LogWrite(payload, static_cast<size_t>(sent), m_host_fd, to);

  const u32 ip = destination.sin_addr.s_addr;
  INFO_LOG_FMT(IOS_NET, "{} = {}, socket {:08x}, in ({:08x}, {}), in2 ({:08x}, {}), {}.{}.{}.{}",
               has_destination ? "IOCTLV_SO_SENDTO" : "IOCTLV_SO_SEND", result, m_guest_fd,
               v.in.address, payload_size, v.in2.address, v.in2.size, ip & 0xff, (ip >> 8) & 0xff,
               (ip >> 16) & 0xff, (ip >> 24) & 0xff);
  return result;
}

s32 WiiSocket::RunRecvFrom(const Request& request, bool* force_non_blocking)
{
  auto& system = m_manager.m_ios.GetSystem();
  auto& memory = system.GetMemory();
  const IOCtlVRequest ioctlv{system, request.address};
  const Vectors v{ioctlv};

  // Same rule as for sending.
  m_connecting = PollConnectingState();
  if (m_non_blocking && IsTCP() && m_connecting == ConnectingState::Connecting)
    return -SO_EAGAIN;

  // First input vector: +4 flags. First output vector: the data; second: the source address.
  u32 flags = memory.Read_U32(v.in.address + 4);
  char* const buffer =
      reinterpret_cast<char*>(memory.GetPointerForRange(v.out.address, v.out.size));
  const bool wants_source = v.out2.size != 0;

  sockaddr_in source{};
  if (wants_source)
  {
    WiiSockAddrIn wii_source;
    memory.CopyFromEmu(&wii_source, v.out2.address, sizeof(wii_source));
    source = WiiSockMan::ToNativeAddrIn(wii_source);
  }

  *force_non_blocking = (flags & SO_MSG_NONBLOCK) != 0;
  // Peek and out-of-band are the only receive flags the host gets.
  flags &= SO_MSG_PEEK | SO_MSG_OOB;

#ifdef _WIN32
  if (flags & SO_MSG_PEEK)
  {
    // Peeking is answered with the number of bytes waiting.
    u_long available = 0;
    ioctlsocket(Host(m_host_fd), FIONREAD, &available);
    return static_cast<s32>(available);
  }
#endif

  socklen_t source_length = sizeof(sockaddr_in);
  sockaddr* const from = wants_source ? reinterpret_cast<sockaddr*>(&source) : nullptr;
  const int received = static_cast<int>(
      recvfrom(Host(m_host_fd), buffer, static_cast<HostLength>(static_cast<int>(v.out.size)),
               static_cast<int>(flags), from, wants_source ? &source_length : nullptr));
  const s32 result =
      m_manager.GetNetErrorCode(received, wants_source ? "SO_RECVFROM" : "SO_RECV", true);
  if (received > 0)
    CaptureLogger(system)->LogRead(buffer, static_cast<size_t>(received), m_host_fd, from);

  INFO_LOG_FMT(IOS_NET,
               "{}({}, {}) socket {:08X}, flags {:08X}, in ({:08x}, {}), in2 ({:08x}, {}), "
               "out ({:08x}, {}), out2 ({:08x}, {})",
               wants_source ? "IOCTLV_SO_RECVFROM" : "IOCTLV_SO_RECV", result, fmt::ptr(buffer),
               m_guest_fd, flags, v.in.address, v.in.size, v.in2.address, v.in2.size,
               v.out.address, v.out.size, v.out2.address, v.out2.size);

  if (wants_source)
  {
    const WiiSockAddrIn wii_source = WiiSockMan::ToWiiAddrIn(source, source_length);
    memory.CopyToEmu(v.out2.address, &wii_source, sizeof(wii_source));
  }
  return result;
}

void WiiSocket::Update()
{
  for (auto it = m_queue.begin(); it != m_queue.end();)
  {
    const QueuedRequest& queued = *it;
    const IPCCommandType command_type = queued.request.command;

    s32 result = 0;
    bool force_non_blocking = false;
    if (!queued.is_ssl && command_type == IPC_CMD_IOCTL)
    {
      result = RunIOCtl(queued.request, queued.command);
    }
    else if (command_type == IPC_CMD_IOCTLV)
    {
      if (queued.is_ssl)
        result = RunSSL(queued.request, queued.command);
      else if (queued.command == IOCTLV_SO_SENDTO)
        result = RunSendTo(queued.request, &force_non_blocking);
      else if (queued.command == IOCTLV_SO_RECVFROM)
        result = RunRecvFrom(queued.request, &force_non_blocking);
    }

    if (queued.aborted)
    {
      it = m_queue.erase(it);
      continue;
    }

    const bool try_again = queued.is_ssl ?
                               (result == SSL_ERR_WAGAIN || result == SSL_ERR_RAGAIN) :
                               IsStillConnecting(result);
    if (try_again && !m_non_blocking && !force_non_blocking)
    {
      ++it;
      continue;
    }

    DEBUG_LOG_FMT(IOS_NET,
                  "IOCTL(V) socket {:08x}: command {} returned {} (non-blocking {}, forced {})",
                  m_guest_fd, queued.command, result, m_non_blocking, force_non_blocking);
    m_manager.EnqueueIPCReply(queued.request, result);
    it = m_queue.erase(it);
  }
}

void WiiSocket::SetConnectingState(s32 connect_result)
{
  if (IsStillConnecting(connect_result))
    m_connecting = ConnectingState::Connecting;
  else if (connect_result >= 0)
    m_connecting = ConnectingState::Connected;
  else
    m_connecting = ConnectingState::Error;
}

WiiSocket::ConnectingState WiiSocket::PollConnectingState() const
{
  if (m_connecting != ConnectingState::Connecting)
    return m_connecting;

  return KeepingNetworkError([this] {
    // Writable (or an exception) means the connect has finished one way or the other.
    fd_set unused;
    fd_set writable;
    fd_set exceptional;
    FD_ZERO(&unused);
    FD_ZERO(&writable);
    FD_ZERO(&exceptional);
    FD_SET(Host(m_host_fd), &writable);
    FD_SET(Host(m_host_fd), &exceptional);
    timeval no_wait = {0, 0};
    if (select(m_host_fd + 1, &unused, &writable, &exceptional, &no_wait) < 0)
    {
      const int error = LastHostError();
      ERROR_LOG_FMT(IOS_SSL, "Could not check connection of socket {} (err={}): {}", m_guest_fd,
                    error, Common::DecodeNetworkError(error));
      return ConnectingState::Error;
    }
    if (!FD_ISSET(Host(m_host_fd), &writable) && !FD_ISSET(Host(m_host_fd), &exceptional))
      return m_connecting;

    int error = 0;
    socklen_t error_length = sizeof(error);
    if (getsockopt(Host(m_host_fd), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error),
                   &error_length) != 0)
    {
      error = LastHostError();
      ERROR_LOG_FMT(IOS_SSL, "Could not read error of socket {} (err={}): {}", m_guest_fd, error,
                    Common::DecodeNetworkError(error));
      return ConnectingState::Error;
    }
    if (error != 0)
    {
      ERROR_LOG_FMT(IOS_SSL, "Connect of socket {} failed (err={}): {}", m_guest_fd, error,
                    Common::DecodeNetworkError(error));
      return ConnectingState::Error;
    }

    // Only a known peer proves the connection is up.
    sockaddr_in peer;
    socklen_t peer_length = sizeof(peer);
    if (getpeername(Host(m_host_fd), reinterpret_cast<sockaddr*>(&peer), &peer_length) != 0)
    {
      error = LastHostError();
      ERROR_LOG_FMT(IOS_SSL, "Connect of socket {} has no peer (err={}): {}", m_guest_fd, error,
                    Common::DecodeNetworkError(error));
      return ConnectingState::Error;
    }

    INFO_LOG_FMT(IOS_SSL, "Connect of socket {} finished", m_guest_fd);
    return ConnectingState::Connected;
  });
}

bool WiiSocket::IsTCP() const
{
  return KeepingNetworkError([this] {
    const std::optional<int> type = HostSocketType(m_host_fd);
    return type && *type == SOCK_STREAM;
  });
}

s32 WiiSockMan::AddSocket(s32 fd, bool is_rw)
{
  const char* const caller = is_rw ? "SO_ACCEPT" : "NewSocket";
  if (fd < 0)
    return GetNetErrorCode(fd, caller, is_rw);

  s32 wii_fd = 0;
  while (wii_fd < WII_SOCKET_FD_MAX && m_sockets.contains(wii_fd))
    ++wii_fd;

  if (wii_fd == WII_SOCKET_FD_MAX)
  {
    CloseHost(fd);
    wii_fd = -SO_EMFILE;
    ERROR_LOG_FMT(IOS_NET, "{}: no free socket slot, ret={}", caller, wii_fd);
    SetLastNetError(wii_fd);
    return wii_fd;
  }

  WiiSocket& socket = m_sockets.emplace(wii_fd, *this).first->second;
  socket.AdoptHostSocket(fd, wii_fd);
  CaptureLogger(m_ios.GetSystem())->OnNewSocket(fd);
  Common::SetPlatformSocketOptions(fd);

  // New Wii UDP sockets may send to the broadcast address straight away.
  if (!is_rw)
  {
    KeepingNetworkError([fd] {
      const std::optional<int> type = HostSocketType(fd);
      if (!type || *type != SOCK_DGRAM)
        return;
      const int enable = 1;
      if (setsockopt(Host(fd), SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enable),
                     sizeof(enable)) != 0)
      {
        ERROR_LOG_FMT(IOS_NET, "Could not enable SO_BROADCAST");
      }
    });
  }

  SetLastNetError(wii_fd);
  return wii_fd;
}

bool WiiSockMan::IsSocketBlocking(s32 wii_fd) const
{
  const auto found = m_sockets.find(wii_fd);
  return found != m_sockets.end() && !found->second.m_non_blocking;
}

s32 WiiSockMan::NewSocket(s32 af, s32 type, s32 protocol)
{
  // The Wii's address family numbers (2 and 23, as on Windows) mean different values elsewhere.
  int host_family;
  if (af == 2)
    host_family = AF_INET;
  else if (af == 23)
    host_family = AF_INET6;
  else
    return -SO_EAFNOSUPPORT;

  // Only IPPROTO_IP, and only stream (1) or datagram (2) sockets.
  if (protocol != 0)
    return -SO_EPROTONOSUPPORT;
  if (type != 1 && type != 2)
    return -SO_EPROTOTYPE;

  return AddSocket(static_cast<s32>(socket(host_family, type, protocol)), false);
}

s32 WiiSockMan::GetHostSocket(s32 wii_fd) const
{
  const auto found = m_sockets.find(wii_fd);
  // Note: the host's EBADF, not the Wii's.
  return found != m_sockets.end() ? found->second.m_host_fd : -EBADF;
}

s32 WiiSockMan::ShutdownSocket(s32 wii_fd, u32 how)
{
  const auto found = m_sockets.find(wii_fd);
  return found != m_sockets.end() ? found->second.Shutdown(how) : -SO_EBADF;
}

s32 WiiSockMan::DeleteSocket(s32 wii_fd)
{
  const auto found = m_sockets.find(wii_fd);
  if (found == m_sockets.end())
    return -SO_EBADF;
  const s32 result = found->second.Close();
  m_sockets.erase(found);
  return result;
}

void WiiSockMan::EnqueueIPCReply(const Request& request, s32 return_value) const
{
  m_ios.EnqueueIPCReply(request, return_value);
}

void WiiSockMan::Update()
{
  fd_set readable;
  fd_set writable;
  fd_set exceptional;
  FD_ZERO(&readable);
  FD_ZERO(&writable);
  FD_ZERO(&exceptional);

  // Sockets that lost their host socket are dropped here.
  int highest = 0;
  for (auto it = m_sockets.begin(); it != m_sockets.end();)
  {
    const WiiSocket& socket = it->second;
    if (!socket.IsValid())
    {
      it = m_sockets.erase(it);
      continue;
    }
    FD_SET(Host(socket.m_host_fd), &readable);
    FD_SET(Host(socket.m_host_fd), &writable);
    FD_SET(Host(socket.m_host_fd), &exceptional);
    highest = std::max(highest, socket.m_host_fd + 1);
    ++it;
  }

  // The readiness is not used: every queued request is simply retried.
  timeval no_wait = {0, 0};
  select(highest, &readable, &writable, &exceptional, &no_wait);
  for (auto& entry : m_sockets)
    entry.second.Update();

  UpdatePollCommands();
}

void WiiSockMan::UpdatePollCommands()
{
  if (m_polls.empty())
    return;

  const auto now = std::chrono::high_resolution_clock::now();
  const s64 elapsed_ms = static_cast<s64>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_poll_update).count());
  m_last_poll_update = now;

  // Negative timeouts wait forever and are left as they are.
  for (PollCommand& command : m_polls)
  {
    if (command.timeout > 0)
      command.timeout = std::max<s64>(0, command.timeout - elapsed_ms);
  }

  auto& system = m_ios.GetSystem();
  auto& memory = system.GetMemory();

  std::erase_if(m_polls, [&](PollCommand& command) {
    std::vector<pollfd_t>& fds = command.wii_fds;
    int ready = 0;

    if (fds[0].revents & (POLLHUP | POLLERR))
    {
      // Set by DoState after a load: every socket is reported as broken.
      ready = static_cast<int>(fds.size());
    }
    else
    {
      // Only entries whose Wii socket exists go to the host. Hosts disagree about invalid fds
      // (Linux reports nothing, Windows POLLNVAL, or fails outright), so they are left out.
      std::vector<size_t> order(fds.size());
      std::iota(order.begin(), order.end(), size_t{0});
      const auto invalid = std::ranges::partition(order, [&](size_t index) {
        const u32 entry = command.buffer_out + static_cast<u32>(0xc * index);
        return GetHostSocket(static_cast<s32>(memory.Read_U32(entry))) >= 0;
      });
      const size_t live_count = static_cast<size_t>(std::distance(order.begin(), invalid.begin()));

      if (live_count > 0)
      {
        std::vector<pollfd_t> live(live_count);
        for (size_t i = 0; i < live_count; ++i)
          live[i] = fds[order[i]];
        ready = poll(live.data(), static_cast<PollCount>(live_count), 0);
        for (size_t i = 0; i < live_count; ++i)
          fds[order[i]] = live[i];
      }
      if (ready < 0)
        ready = GetNetErrorCode(ready, "UpdatePollCommands", false);
    }

    // Nothing yet and time left: keep waiting.
    if (ready == 0 && command.timeout != 0)
      return false;

    // Only revents (+8 of each 12-byte entry) is written back; fd and events are inputs.
    for (size_t i = 0; i < fds.size(); ++i)
    {
      const s32 wii_events = ConvertEvents(fds[i].revents, ConvertDirection::NativeToWii);
      memory.Write_U32(static_cast<u32>(wii_events),
                       command.buffer_out + static_cast<u32>(0xc * i + 8));
      DEBUG_LOG_FMT(IOS_NET, "IOCTL_SO_POLL socket {} wevents {:08X} events {:08X} revents {:08X}",
                    i, wii_events, fds[i].events, fds[i].revents);
    }
    EnqueueIPCReply(Request{system, command.request_addr}, ready);
    return true;
  });
}

sockaddr_in WiiSockMan::ToNativeAddrIn(WiiSockAddrIn from)
{
  sockaddr_in native{};
  native.sin_family = from.family;
  native.sin_port = from.port;
  native.sin_addr.s_addr = from.addr.addr;
  return native;
}

namespace
{
struct EventBit
{
  int native;
  int wii;
};

constexpr EventBit EVENT_BITS[] = {
    {POLLRDNORM, 0x0001}, {POLLRDBAND, 0x0002}, {POLLPRI, 0x0004}, {POLLWRNORM, 0x0008},
    {POLLWRBAND, 0x0010}, {POLLERR, 0x0020},    {POLLHUP, 0x0040}, {POLLNVAL, 0x0080},
};
}  // namespace

s32 WiiSockMan::ConvertEvents(s32 events, ConvertDirection dir)
{
  const bool to_wii = dir == ConvertDirection::NativeToWii;
  s32 converted = 0;
  s32 known_wii_bits = 0;
  for (const EventBit& bit : EVENT_BITS)
  {
    const int from = to_wii ? bit.native : bit.wii;
    const int to = to_wii ? bit.wii : bit.native;
    if (events & from)
      converted |= to;
    known_wii_bits |= bit.wii;
  }

  if (!to_wii && (events & ~known_wii_bits) != 0)
    ERROR_LOG_FMT(IOS_NET, "SO_POLL: unhandled Wii event types: {:04x}", events & ~known_wii_bits);
  return converted;
}

WiiSockAddrIn WiiSockMan::ToWiiAddrIn(const sockaddr_in& from, socklen_t addrlen)
{
  WiiSockAddrIn wii;
  const auto length = std::min<socklen_t>(addrlen, static_cast<socklen_t>(sizeof(WiiSockAddrIn)));
  wii.len = static_cast<u8>(length);
  wii.family = static_cast<u8>(from.sin_family & 0xff);
  wii.port = from.sin_port;
  wii.addr.addr = from.sin_addr.s_addr;
  return wii;
}

void WiiSockMan::DoState(PointerWrap& p)
{
  // Pending polls are kept (without their timeouts); open sockets cannot be.
  const bool loading = !p.IsWriteMode() && !p.IsMeasureMode();

  size_t count = m_polls.size();
  p.Do(count);
  if (loading)
    m_polls.resize(count);
  for (PollCommand& command : m_polls)
  {
    p.Do(command.request_addr);
    p.Do(command.buffer_out);
    p.Do(command.wii_fds);
  }

  // The host sockets behind the restored polls are gone; they are answered as broken.
  if (loading)
  {
    for (PollCommand& command : m_polls)
    {
      for (pollfd_t& fd : command.wii_fds)
        fd.revents = POLLHUP | POLLERR;
    }
  }
}

void WiiSockMan::AddPollCommand(const PollCommand& cmd)
{
  m_polls.push_back(cmd);
}

void WiiSockMan::UpdateWantDeterminism(bool want)
{
  // Starting a movie or NetPlay closes every socket.
  if (want)
    Clean();
}
}  // namespace IOS::HLE
