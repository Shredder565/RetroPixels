// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Network/IP/Top.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <netdb.h>
#include <poll.h>
#endif

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Swap.h"

#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Network/ICMP.h"
#include "Core/IOS/Network/MACUtils.h"
#include "Core/IOS/Network/Socket.h"
#include "Core/System.h"
#include "Core/WC24PatchEngine.h"

#ifdef _WIN32
#include <iphlpapi.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <resolv.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include "jni/AndroidCommon/AndroidCommon.h"
#endif

#ifdef __linux__
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#endif

namespace IOS::HLE
{
namespace
{
// Results of the IP module itself (not socket errors).
constexpr s32 SO_ERROR_INVALID_REQUEST = -51;
constexpr s32 SO_ERROR_HOST_NOT_FOUND = -305;

#ifdef _WIN32
using HostSocket = SOCKET;
#else
using HostSocket = int;
#endif

HostSocket Host(s32 fd)
{
  return static_cast<HostSocket>(fd);
}

const char* DescribeLookupError(int error)
{
#ifdef _WIN32
  // gai_strerror is not thread-safe on Windows, but there the codes are ordinary error numbers.
  return Common::DecodeNetworkError(error);
#else
  // Elsewhere getaddrinfo has its own codes (EAI_*).
  return gai_strerror(error);
#endif
}

// Dotted-quad IPv4 address, exactly four parts of 0-255, digits and dots only.
std::optional<std::array<u8, 4>> ParseIPv4(std::string_view text)
{
  std::array<u8, 4> parts{};
  size_t count = 0;
  bool in_part = false;
  for (const char c : text)
  {
    if (c >= '0' && c <= '9')
    {
      if (!in_part)
      {
        if (count == parts.size())
          return std::nullopt;
        ++count;
        in_part = true;
      }
      const unsigned value = parts[count - 1] * 10u + static_cast<unsigned>(c - '0');
      if (value > 255)
        return std::nullopt;
      parts[count - 1] = static_cast<u8>(value);
    }
    else if (c == '.' && in_part && count < parts.size())
    {
      in_part = false;
    }
    else
    {
      return std::nullopt;
    }
  }
  if (count != parts.size())
    return std::nullopt;
  return parts;
}

// Socket option levels and names, Wii -> host. Unknown values are passed through.
int HostOptionLevel(u32 level)
{
  if (level == 0xffff)
    return SOL_SOCKET;
  INFO_LOG_FMT(IOS_NET, "SO_SETSOCKOPT: unknown level {}", level);
  return static_cast<int>(level);
}

int HostOptionName(u32 name)
{
  static constexpr std::pair<u32, int> NAMES[] = {
      {0x4, SO_REUSEADDR},   {0x80, SO_LINGER},     {0x100, SO_OOBINLINE},
      {0x1001, SO_SNDBUF},   {0x1002, SO_RCVBUF},   {0x1003, SO_SNDLOWAT},
      {0x1004, SO_RCVLOWAT}, {0x1008, SO_TYPE},     {0x1009, SO_ERROR},
  };
  for (const auto& [wii, host] : NAMES)
  {
    if (wii == name)
      return host;
  }
  INFO_LOG_FMT(IOS_NET, "SO_SETSOCKOPT: unknown optname {}", name);
  return static_cast<int>(name);
}

struct Route
{
  u32 index;
  in_addr destination;
  in_addr netmask;
  in_addr gateway;
};

struct HostInterface
{
  in_addr address;
  in_addr netmask;
  in_addr broadcast;
  std::vector<Route> routes;
};

in_addr ToInAddr(u32 network_order)
{
  in_addr result;
  result.s_addr = network_order;
  return result;
}

#ifdef _WIN32
// Fetches one of the IP Helper tables, which are sized by a first call without a buffer.
// The returned storage stays empty (a null table) if that sizing call does not ask for room.
template <typename Fetch>
std::vector<u8> SizedTableStorage(Fetch fetch, DWORD* size)
{
  std::vector<u8> storage;
  if (fetch(nullptr, size) == ERROR_INSUFFICIENT_BUFFER)
    storage.resize(*size);
  return storage;
}

std::vector<Route> ReadRoutingTable()
{
  std::vector<Route> routes;
  const auto fetch = [](MIB_IPFORWARDTABLE* table, DWORD* size) {
    return GetIpForwardTable(table, size, FALSE);
  };
  DWORD size = 0;
  std::vector<u8> storage = SizedTableStorage(fetch, &size);
  auto* const table =
      storage.empty() ? nullptr : reinterpret_cast<MIB_IPFORWARDTABLE*>(storage.data());

  // ERROR_MORE_DATA has been seen (on XP) even after sizing; the call is then repeated.
  for (DWORD status = fetch(table, &size); status == NO_ERROR || status == ERROR_MORE_DATA;
       status = fetch(table, &size))
  {
    for (const MIB_IPFORWARDROW& row : std::span(table->table, table->dwNumEntries))
    {
      routes.push_back(Route{row.dwForwardIfIndex, ToInAddr(row.dwForwardDest),
                             ToInAddr(row.dwForwardMask), ToInAddr(row.dwForwardNextHop)});
    }
    if (status == NO_ERROR)
      break;
  }
  return routes;
}
#elif defined(__linux__)
// Dumps the kernel routing table over rtnetlink.
std::vector<Route> ReadRoutingTable()
{
  constexpr size_t BUFFER_SIZE = 8192;
  constexpr u32 REQUEST_SEQUENCE = 0;

  const int sock = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
  if (sock < 0)
  {
    ERROR_LOG_FMT(IOS_NET, "Could not open a netlink socket: {}", Common::StrNetworkError());
    return {};
  }
  constexpr timeval receive_timeout{.tv_sec = 2, .tv_usec = 0};
  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)) < 0)
  {
    ERROR_LOG_FMT(IOS_NET, "Could not set the netlink receive timeout: {}",
                  Common::StrNetworkError());
    return {};
  }
  Common::ScopeGuard close_socket{[sock] { close(sock); }};

  std::array<char, BUFFER_SIZE> buffer{};
  const u32 pid = static_cast<u32>(getpid());
  {
    auto* const request = reinterpret_cast<nlmsghdr*>(buffer.data());
    request->nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
    request->nlmsg_type = RTM_GETROUTE;
    request->nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
    request->nlmsg_seq = REQUEST_SEQUENCE;
    request->nlmsg_pid = pid;
    if (send(sock, request, request->nlmsg_len, 0) < 0)
    {
      ERROR_LOG_FMT(IOS_NET, "Could not send the netlink request: {}", Common::StrNetworkError());
      return {};
    }
  }

  // Collect the reply parts until the dump is done, a part is not flagged as multi-part, or
  // the buffer is full. (Parts carrying the request's sequence number + 1 also end it.)
  buffer.fill(0);
  unsigned int received = 0;
  while (true)
  {
    char* const chunk = buffer.data() + received;
    const ssize_t length = recv(sock, chunk, BUFFER_SIZE - received, 0);
    if (length < 0)
    {
      ERROR_LOG_FMT(IOS_NET, "Could not receive the netlink reply: {}", Common::StrNetworkError());
      return {};
    }
    const auto length_u = static_cast<unsigned int>(length);
    const auto* const part = reinterpret_cast<const nlmsghdr*>(chunk);
    if (!NLMSG_OK(part, length_u))
    {
      ERROR_LOG_FMT(IOS_NET, "Malformed netlink reply");
      return {};
    }
    if (part->nlmsg_type == NLMSG_ERROR)
    {
      if (part->nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr)))
      {
        ERROR_LOG_FMT(IOS_NET, "Truncated netlink error reply");
      }
      else
      {
        const auto* const error = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(part));
        ERROR_LOG_FMT(IOS_NET, "Netlink error {}: {}", error->error,
                      Common::DecodeNetworkError(-error->error));
      }
      return {};
    }
    if (part->nlmsg_type == NLMSG_DONE)
      break;

    received += length_u;
    if ((part->nlmsg_flags & NLM_F_MULTI) == 0)
      break;
    if (received > BUFFER_SIZE ||
        (part->nlmsg_seq == REQUEST_SEQUENCE + 1 && part->nlmsg_pid == pid))
    {
      break;
    }
  }

  // IPv4 routes only. The interface index is not filled in, and the "netmask" is the prefix
  // length turned into its low bits (the shift count wraps at 32, as on x86).
  std::vector<Route> routes;
  unsigned int remaining = received;
  for (auto* message = reinterpret_cast<nlmsghdr*>(buffer.data()); NLMSG_OK(message, remaining);
       message = NLMSG_NEXT(message, remaining))
  {
    const auto* const route_message = reinterpret_cast<const rtmsg*>(NLMSG_DATA(message));
    if (route_message->rtm_family != AF_INET)
      continue;

    Route route{};
    auto attributes_length = RTM_PAYLOAD(message);
    for (auto* attribute = reinterpret_cast<rtattr*>(RTM_RTA(route_message));
         RTA_OK(attribute, attributes_length); attribute = RTA_NEXT(attribute, attributes_length))
    {
      if (attribute->rta_type == RTA_GATEWAY)
        std::memcpy(&route.gateway.s_addr, RTA_DATA(attribute), sizeof(u32));
      else if (attribute->rta_type == RTA_DST)
        std::memcpy(&route.destination.s_addr, RTA_DATA(attribute), sizeof(u32));
    }
    route.netmask.s_addr = (1u << (route_message->rtm_dst_len & 31u)) - 1u;
    routes.push_back(route);
  }
  return routes;
}
#else
std::vector<Route> ReadRoutingTable()
{
  return {};
}
#endif

std::optional<HostInterface> FindHostInterface()
{
  std::vector<Route> routes = ReadRoutingTable();

#ifdef _WIN32
  const auto fetch = [](MIB_IPADDRTABLE* table, DWORD* size) {
    return GetIpAddrTable(table, size, FALSE);
  };
  DWORD size = 0;
  std::vector<u8> storage = SizedTableStorage(fetch, &size);
  auto* const table =
      storage.empty() ? nullptr : reinterpret_cast<MIB_IPADDRTABLE*>(storage.data());

  // The interface of the (first) default route.
  const auto default_route = std::ranges::find_if(
      routes, [](const Route& route) { return route.destination.s_addr == 0; });
  const NET_IFINDEX interface_index =
      default_route != routes.end() ? default_route->index : NET_IFINDEX_UNSPECIFIED;

  if (interface_index != NET_IFINDEX_UNSPECIFIED && fetch(table, &size) == NO_ERROR)
  {
    for (const MIB_IPADDRROW& row : std::span(table->table, table->dwNumEntries))
    {
      if (row.dwIndex == interface_index)
      {
        return HostInterface{ToInAddr(row.dwAddr), ToInAddr(row.dwMask),
                             ToInAddr(row.dwBCastAddr), routes};
      }
    }
  }
#elif defined(__ANDROID__)
  const u32 address = GetNetworkIpAddress();
  const u32 netmask = (1 << GetNetworkPrefixLength()) - 1;
  const u32 gateway = GetNetworkGateway();
  // Not a real routing table, but enough to report the gateway.
  if (routes.empty())
    routes = {Route{0, ToInAddr(0), ToInAddr(0), ToInAddr(gateway)}};
  // The gateway is also what is reported as the broadcast address.
  if (address != 0 || netmask != 0 || gateway != 0)
    return HostInterface{ToInAddr(address), ToInAddr(netmask), ToInAddr(gateway), routes};
#else
  // The interface used to reach the Internet counts as the default one. Connecting a UDP
  // socket sends nothing; it only picks the local address.
  const auto internet_facing_address = []() -> std::optional<in_addr> {
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    Common::ScopeGuard close_socket{[sock] { close(sock); }};
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = inet_addr("8.8.8.8");
    target.sin_port = htons(53);
    if (connect(sock, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == -1)
      return std::nullopt;
    socklen_t length = sizeof(target);
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&target), &length) == -1)
      return std::nullopt;
    return target.sin_addr;
  }();
  if (!internet_facing_address)
    return std::nullopt;

  ifaddrs* interfaces;
  if (getifaddrs(&interfaces) != 0)
    return std::nullopt;
  Common::ScopeGuard free_interfaces{[interfaces] { freeifaddrs(interfaces); }};

  const auto ipv4 = [](const sockaddr* address) {
    return reinterpret_cast<const sockaddr_in*>(address)->sin_addr;
  };
  for (const ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next)
  {
    if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET ||
        ipv4(entry->ifa_addr).s_addr != internet_facing_address->s_addr)
    {
      continue;
    }
    // Not a real routing table, but enough to report the gateway.
    if (routes.empty())
      routes = {Route{0, {}, {}, ipv4(entry->ifa_dstaddr)}};
    return HostInterface{ipv4(entry->ifa_addr), ipv4(entry->ifa_netmask),
                         ipv4(entry->ifa_broadaddr), routes};
  }
#endif
  return std::nullopt;
}

HostInterface HostInterfaceOrFallback()
{
  // A typical home network, for when the host's own cannot be found.
  static const HostInterface FALLBACK = [] {
    HostInterface fallback;
    fallback.address = ToInAddr(inet_addr("10.0.1.30"));
    fallback.netmask = ToInAddr(inet_addr("255.255.255.0"));
    fallback.broadcast = ToInAddr(inet_addr("10.0.1.255"));
    fallback.routes = {Route{0, ToInAddr(0), ToInAddr(0), ToInAddr(inet_addr("10.0.1.1"))}};
    return fallback;
  }();
  return FindHostInterface().value_or(FALLBACK);
}

// The host's first IPv4 DNS server (host byte order), or 0 if none was found.
u32 FindHostDnsServer([[maybe_unused]] u32 probe_address)
{
  u32 server = 0;
#ifdef _WIN32
  if (Core::WantsDeterminism())
    return server;

  PIP_ADAPTER_ADDRESSES adapters = nullptr;
  ULONG length = 0;
  ULONG status = 0;
  // The adapter list can grow between the sizing call and the real one, so allow a few tries.
  for (int attempt = 0; attempt < 5; ++attempt)
  {
    status = GetAdaptersAddresses(AF_INET, 0, nullptr, adapters, &length);
    if (status != ERROR_BUFFER_OVERFLOW)
      break;
    if (adapters != nullptr)
      FREE(adapters);
    adapters = static_cast<PIP_ADAPTER_ADDRESSES>(MALLOC(length));
    if (adapters == nullptr)
    {
      status = GetLastError();
      break;
    }
  }

  unsigned long best_interface = 0;
  if (status == NO_ERROR &&
      GetBestInterface(static_cast<IPAddr>(probe_address), &best_interface) == NO_ERROR)
  {
    for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != nullptr; adapter = adapter->Next)
    {
      if (adapter->IfIndex != best_interface || adapter->FirstDnsServerAddress == nullptr ||
          adapter->OperStatus != IfOperStatusUp)
      {
        continue;
      }
      const char* const dns = adapter->FirstDnsServerAddress->Address.lpSockaddr->sa_data;
      INFO_LOG_FMT(IOS_NET, "Name of valid interface: {}", WStringToUTF8(adapter->FriendlyName));
      INFO_LOG_FMT(IOS_NET, "DNS: {}.{}.{}.{}", static_cast<u8>(dns[2]), static_cast<u8>(dns[3]),
                   static_cast<u8>(dns[4]), static_cast<u8>(dns[5]));
      u32 raw;
      std::memcpy(&raw, dns + 2, sizeof(raw));
      server = Common::swap32(raw);
      break;
    }
  }
  if (adapters != nullptr)
    FREE(adapters);
#elif (defined(__linux__) && !defined(ANDROID)) || defined(__APPLE__) || defined(__FreeBSD__) ||   \
    defined(__OpenBSD__) || defined(__NetBSD__) || defined(__HAIKU__)
  if (Core::WantsDeterminism())
    return server;

  if (res_init() != 0)
  {
    WARN_LOG_FMT(IOS_NET, "res_init failed");
    return server;
  }
  for (int i = 0; i < _res.nscount; ++i)
  {
    if (_res.nsaddr_list[i].sin_family == AF_INET)
    {
      server = ntohl(_res.nsaddr_list[i].sin_addr.s_addr);
      break;
    }
  }
#endif
  return server;
}

std::string DescribeHints(const addrinfo& hints)
{
  return fmt::format("flags={}, family={}, socktype={}, protocol={}, addrlen={}", hints.ai_flags,
                     hints.ai_family, hints.ai_socktype, hints.ai_protocol, hints.ai_addrlen);
}
}  // namespace

NetIPTopDevice::NetIPTopDevice(EmulationKernel& ios, const std::string& device_name)
    : EmulationDevice(ios, device_name)
{
  m_work_queue.Reset("Network Worker", [this](const AsyncTask& task) {
    const s32 result = task.handler().return_value;
    std::lock_guard guard(m_async_reply_lock);
    m_async_replies.push(AsyncReply{task.request, result});
  });
}

void NetIPTopDevice::DoState(PointerWrap& p)
{
  Device::DoState(p);
}

std::optional<IPCReply> NetIPTopDevice::IOCtl(const IOCtlRequest& request)
{
  // Network results cannot be reproduced, so none are given while determinism is needed.
  if (Core::WantsDeterminism())
    return IPCReply(IPC_EACCES);

  switch (request.request)
  {
  case IOCTL_SO_INITINTERFACE:
    return InitInterface(request);
  case IOCTL_SO_SOCKET:
    return OpenSocket(request);
  case IOCTL_SO_ICMPSOCKET:
    return OpenICMPSocket(request);
  case IOCTL_SO_CLOSE:
  case IOCTL_SO_ICMPCLOSE:
    return CloseSocket(request);
  case IOCTL_SO_ACCEPT:
  case IOCTL_SO_BIND:
  case IOCTL_SO_CONNECT:
  case IOCTL_SO_FCNTL:
    return QueueOnSocket(request);
  case IOCTL_SO_SHUTDOWN:
    return Shutdown(request);
  case IOCTL_SO_LISTEN:
    return Listen(request);
  case IOCTL_SO_GETSOCKOPT:
    return GetSockOpt(request);
  case IOCTL_SO_SETSOCKOPT:
    return SetSockOpt(request);
  case IOCTL_SO_GETSOCKNAME:
    return GetSockName(request);
  case IOCTL_SO_GETPEERNAME:
    return GetPeerName(request);
  case IOCTL_SO_GETHOSTID:
    return GetHostID(request);
  case IOCTL_SO_INETATON:
    return RunOnWorker(&NetIPTopDevice::InetAToN, request);
  case IOCTL_SO_INETPTON:
    return InetPToN(request);
  case IOCTL_SO_INETNTOP:
    return InetNToP(request);
  case IOCTL_SO_POLL:
    return Poll(request);
  case IOCTL_SO_GETHOSTBYNAME:
    return RunOnWorker(&NetIPTopDevice::GetHostByName, request);
  case IOCTL_SO_ICMPCANCEL:
    return ICMPCancel(request);
  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_NET);
    return IPCReply(IPC_SUCCESS);
  }
}

std::optional<IPCReply> NetIPTopDevice::IOCtlV(const IOCtlVRequest& request)
{
  switch (request.request)
  {
  case IOCTLV_SO_GETINTERFACEOPT:
    return GetInterfaceOpt(request);
  case IOCTLV_SO_SENDTO:
    return SendTo(request);
  case IOCTLV_SO_RECVFROM:
    return RecvFrom(request);
  case IOCTLV_SO_GETADDRINFO:
    return RunOnWorker(&NetIPTopDevice::GetAddrInfo, request);
  case IOCTLV_SO_ICMPPING:
    return ICMPPing(request);
  default:
    request.DumpUnknown(GetSystem(), GetDeviceName(), Common::Log::LogType::IOS_NET);
    return IPCReply(IPC_SUCCESS);
  }
}

void NetIPTopDevice::Update()
{
  {
    std::lock_guard guard(m_async_reply_lock);
    for (; !m_async_replies.empty(); m_async_replies.pop())
    {
      const AsyncReply& reply = m_async_replies.front();
      GetEmulationKernel().EnqueueIPCReply(reply.request, reply.return_value);
    }
  }
  GetEmulationKernel().GetSocketManager()->Update();
}

IPCReply NetIPTopDevice::InitInterface(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_WC24);
  return IPCReply(IPC_SUCCESS);
}

IPCReply NetIPTopDevice::OpenSocket(const IOCtlRequest& request)
{
  auto& memory = GetSystem().GetMemory();
  const u32 family = memory.Read_U32(request.buffer_in);
  const u32 type = memory.Read_U32(request.buffer_in + 4);
  const u32 protocol = memory.Read_U32(request.buffer_in + 8);

  const s32 result = GetEmulationKernel().GetSocketManager()->NewSocket(
      static_cast<s32>(family), static_cast<s32>(type), static_cast<s32>(protocol));
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_SOCKET {:08x} ({},{},{}), in ({:08x}, {}), out ({:08x}, {})",
               result, family, type, protocol, request.buffer_in, request.buffer_in_size,
               request.buffer_out, request.buffer_out_size);
  return IPCReply(result);
}

IPCReply NetIPTopDevice::OpenICMPSocket(const IOCtlRequest& request)
{
  const u32 family = GetSystem().GetMemory().Read_U32(request.buffer_in);
  const s32 result = GetEmulationKernel().GetSocketManager()->NewSocket(static_cast<s32>(family),
                                                                        SOCK_RAW, IPPROTO_ICMP);
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_ICMPSOCKET({:x}) {}", family, result);
  return IPCReply(result);
}

IPCReply NetIPTopDevice::CloseSocket(const IOCtlRequest& request)
{
  const u32 fd = GetSystem().GetMemory().Read_U32(request.buffer_in);
  const s32 result = GetEmulationKernel().GetSocketManager()->DeleteSocket(static_cast<s32>(fd));
  INFO_LOG_FMT(IOS_NET, "{}({:x}) {:x}",
               request.request == IOCTL_SO_ICMPCLOSE ? "IOCTL_SO_ICMPCLOSE" : "IOCTL_SO_CLOSE", fd,
               result);
  return IPCReply(result);
}

std::optional<IPCReply> NetIPTopDevice::QueueOnSocket(const IOCtlRequest& request)
{
  const u32 fd = GetSystem().GetMemory().Read_U32(request.buffer_in);
  GetEmulationKernel().GetSocketManager()->DoSock(static_cast<s32>(fd), request,
                                                  static_cast<NET_IOCTL>(request.request));
  return std::nullopt;
}

IPCReply NetIPTopDevice::Shutdown(const IOCtlRequest& request)
{
  // Input: fd, how.
  if (request.buffer_in == 0 || request.buffer_in_size < 8)
  {
    ERROR_LOG_FMT(IOS_NET, "IOCTL_SO_SHUTDOWN = EINVAL, in ({:08x}, {})", request.buffer_in,
                  request.buffer_in_size);
    return IPCReply(-SO_EINVAL);
  }

  auto& memory = GetSystem().GetMemory();
  const u32 fd = memory.Read_U32(request.buffer_in);
  const u32 how = memory.Read_U32(request.buffer_in + 4);
  const s32 result =
      GetEmulationKernel().GetSocketManager()->ShutdownSocket(static_cast<s32>(fd), how);
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_SHUTDOWN(fd={}, how={}) = {}", fd, how, result);
  return IPCReply(result);
}

IPCReply NetIPTopDevice::Listen(const IOCtlRequest& request)
{
  auto& memory = GetSystem().GetMemory();
  const u32 fd = memory.Read_U32(request.buffer_in);
  const u32 backlog = memory.Read_U32(request.buffer_in + 4);

  const auto sockets = GetEmulationKernel().GetSocketManager();
  const int ret = listen(Host(sockets->GetHostSocket(static_cast<s32>(fd))),
                         static_cast<int>(backlog));
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_WC24);
  return IPCReply(sockets->GetNetErrorCode(ret, "SO_LISTEN", false));
}

IPCReply NetIPTopDevice::GetSockOpt(const IOCtlRequest& request)
{
  // Everything is in the output buffer: +0 fd, +4 level, +8 name; the length goes back to
  // +0xc and the value to +0x10.
  auto& memory = GetSystem().GetMemory();
  const u32 out = request.buffer_out;
  const u32 fd = memory.Read_U32(out);
  const u32 level = memory.Read_U32(out + 4);
  const u32 name = memory.Read_U32(out + 8);
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_WC24);

  const int host_level = HostOptionLevel(level);
  const int host_name = HostOptionName(name);
  std::array<u8, 20> value{};
  socklen_t length = 4;

  const auto sockets = GetEmulationKernel().GetSocketManager();
  const int ret = getsockopt(Host(sockets->GetHostSocket(static_cast<s32>(fd))), host_level,
                             host_name, reinterpret_cast<char*>(value.data()), &length);
  const s32 result = sockets->GetNetErrorCode(ret, "SO_GETSOCKOPT", false);

  memory.Write_U32(static_cast<u32>(length), out + 0xc);
  memory.CopyToEmu(out + 0x10, value.data(), static_cast<size_t>(length));

  // Note: compared with the host's SO_ERROR number. The value reported is the last network
  // result, which by now is that of this very call.
  if (name == static_cast<u32>(SO_ERROR))
  {
    memory.Write_U32(sizeof(s32), out + 0xc);
    memory.Write_U32(static_cast<u32>(sockets->GetLastNetError()), out + 0x10);
  }
  return IPCReply(result);
}

IPCReply NetIPTopDevice::SetSockOpt(const IOCtlRequest& request)
{
  // Input: +0 fd, +4 level, +8 name, +0xc length, +0x10 value (at most 20 bytes are used).
  auto& memory = GetSystem().GetMemory();
  const u32 in = request.buffer_in;
  const u32 fd = memory.Read_U32(in);
  const u32 level = memory.Read_U32(in + 4);
  const u32 name = memory.Read_U32(in + 8);
  std::array<u8, 20> value{};
  const u32 length = std::min(memory.Read_U32(in + 0xc), static_cast<u32>(value.size()));
  memory.CopyFromEmu(value.data(), in + 0x10, length);

  INFO_LOG_FMT(IOS_NET,
               "IOCTL_SO_SETSOCKOPT({:08x}, {:08x}, {:08x}, {:08x}) in ({:08x}, {}), "
               "out ({:08x}, {}) {:02x}",
               fd, level, name, length, request.buffer_in, request.buffer_in_size,
               request.buffer_out, request.buffer_out_size, fmt::join(value, " "));

  // TCP options 0x2001 (probably TCP_NODELAY) and 0x2005 (probably a timeout) are accepted
  // without doing anything.
  if (level == 6 && (name == 0x2001 || name == 0x2005))
    return IPCReply(0);

  const int host_level = HostOptionLevel(level);
  const int host_name = HostOptionName(name);
  const auto sockets = GetEmulationKernel().GetSocketManager();
  const int ret = setsockopt(Host(sockets->GetHostSocket(static_cast<s32>(fd))), host_level,
                             host_name, reinterpret_cast<const char*>(value.data()),
                             static_cast<socklen_t>(length));
  return IPCReply(sockets->GetNetErrorCode(ret, "SO_SETSOCKOPT", false));
}

namespace
{
// Writes a socket address the way the Wii lays it out: length byte (the buffer size), family
// byte, then as much of the address data as fits.
void WriteSockAddr(Memory::MemoryManager& memory, const IOCtlRequest& request, u8 family,
                   const sockaddr& address)
{
  const u32 size = request.buffer_out_size;
  if (size > 0)
    memory.Write_U8(static_cast<u8>(size), request.buffer_out);
  if (size > 1)
    memory.Write_U8(family, request.buffer_out + 1);
  if (size > 2)
  {
    memory.CopyToEmu(request.buffer_out + 2, &address.sa_data,
                     std::min<size_t>(sizeof(address.sa_data), size - 2));
  }
}
}  // namespace

IPCReply NetIPTopDevice::GetSockName(const IOCtlRequest& request)
{
  auto& memory = GetSystem().GetMemory();
  const u32 fd = memory.Read_U32(request.buffer_in);
  request.Log(GetDeviceName(), Common::Log::LogType::IOS_WC24);

  sockaddr address{};
  socklen_t length = sizeof(address);
  const int ret = getsockname(
      Host(GetEmulationKernel().GetSocketManager()->GetHostSocket(static_cast<s32>(fd))), &address,
      &length);
  if (request.buffer_out_size < 2 + sizeof(address.sa_data))
    WARN_LOG_FMT(IOS_NET, "IOCTL_SO_GETSOCKNAME: output buffer too small, truncating");
  WriteSockAddr(memory, request, static_cast<u8>(address.sa_family & 0xff), address);
  // The host's own result, not a Wii error code.
  return IPCReply(ret);
}

IPCReply NetIPTopDevice::GetPeerName(const IOCtlRequest& request)
{
  auto& memory = GetSystem().GetMemory();
  const u32 fd = memory.Read_U32(request.buffer_in);

  sockaddr address{};
  socklen_t length = sizeof(address);
  const int ret = getpeername(
      Host(GetEmulationKernel().GetSocketManager()->GetHostSocket(static_cast<s32>(fd))), &address,
      &length);
  if (request.buffer_out_size < 2 + sizeof(address.sa_data))
    WARN_LOG_FMT(IOS_NET, "IOCTL_SO_GETPEERNAME: output buffer too small, truncating");
  // The family is always reported as IPv4 here.
  WriteSockAddr(memory, request, AF_INET, address);
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_GETPEERNAME({:x})", fd);
  return IPCReply(ret);
}

IPCReply NetIPTopDevice::GetHostID(const IOCtlRequest&)
{
  const u32 ip = ntohl(HostInterfaceOrFallback().address.s_addr);
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_GETHOSTID = {}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xff,
               (ip >> 8) & 0xff, ip & 0xff);
  return IPCReply(static_cast<s32>(ip));
}

IPCReply NetIPTopDevice::InetAToN(const IOCtlRequest& request)
{
  auto& memory = GetSystem().GetMemory();
  const std::string name = memory.GetString(request.buffer_in);
  const hostent* const host = gethostbyname(name.c_str());

  // Returns 1 with the address in the output buffer, or 0 if the name does not resolve.
  if (host == nullptr || host->h_addr_list == nullptr || host->h_addr_list[0] == nullptr)
  {
    INFO_LOG_FMT(IOS_NET, "IOCTL_SO_INETATON = -1 {}, in ({:08x}, {}), out ({:08x}, {}), no IP",
                 name, request.buffer_in, request.buffer_in_size, request.buffer_out,
                 request.buffer_out_size);
    return IPCReply(0);
  }

  const u32 ip = Common::swap32(reinterpret_cast<const u8*>(host->h_addr_list[0]));
  memory.Write_U32(ip, request.buffer_out);
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_INETATON = 0 {}, in ({:08x}, {}), out ({:08x}, {}), IP {:08X}",
               name, request.buffer_in, request.buffer_in_size, request.buffer_out,
               request.buffer_out_size, ip);
  return IPCReply(1);
}

IPCReply NetIPTopDevice::InetPToN(const IOCtlRequest& request)
{
  auto& memory = GetSystem().GetMemory();
  const std::string text = memory.GetString(request.buffer_in);
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_INETPTON (Translating: {})", text);

  // 1 with the four address bytes at +4 of the output, or 0 for a malformed address.
  const std::optional<std::array<u8, 4>> address = ParseIPv4(text);
  if (!address)
    return IPCReply(0);
  memory.CopyToEmu(request.buffer_out + 4, address->data(), address->size());
  return IPCReply(1);
}

IPCReply NetIPTopDevice::InetNToP(const IOCtlRequest& request)
{
  // The address bytes are at +8 of the input (+0 family, +4 unknown). The text is written
  // without a terminator.
  auto& memory = GetSystem().GetMemory();
  const u32 bytes = request.buffer_in + 8;
  const std::string text =
      fmt::format("{}.{}.{}.{}", memory.Read_U8(bytes), memory.Read_U8(bytes + 1),
                  memory.Read_U8(bytes + 2), memory.Read_U8(bytes + 3));
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_INETNTOP {}", text);
  memory.CopyToEmu(request.buffer_out, text.data(), text.size());
  return IPCReply(0);
}

std::optional<IPCReply> NetIPTopDevice::Poll(const IOCtlRequest& request)
{
  const auto sockets = GetEmulationKernel().GetSocketManager();
  if (request.buffer_in == 0 || request.buffer_out == 0)
    return IPCReply(-SO_EINVAL);

  // Input: a 64-bit timeout in milliseconds (negative waits forever). Output: 12-byte
  // entries of fd, events and revents.
  auto& memory = GetSystem().GetMemory();
  const s64 timeout = static_cast<s64>(memory.Read_U64(request.buffer_in));
  const u32 count = request.buffer_out_size / 0xc;
  if (count == 0 || count > WII_SOCKET_FD_MAX)
  {
    ERROR_LOG_FMT(IOS_NET, "IOCTL_SO_POLL: bad entry count {}, ret={}", count, -SO_EINVAL);
    return IPCReply(-SO_EINVAL);
  }

  std::vector<pollfd_t> fds(count);
  for (u32 i = 0; i < count; ++i)
  {
    const u32 entry = request.buffer_out + 0xc * i;
    const s32 wii_fd = static_cast<s32>(memory.Read_U32(entry));
    const s32 events = static_cast<s32>(memory.Read_U32(entry + 4));
    const s32 host_events =
        WiiSockMan::ConvertEvents(events, WiiSockMan::ConvertDirection::WiiToNative);

    pollfd_t& fd = fds[i];
    fd.fd = Host(sockets->GetHostSocket(wii_fd));
    fd.revents = 0;
    DEBUG_LOG_FMT(IOS_NET, "IOCTL_SO_POLL({}) socket {:08x}, events {:08x}, native {:08x}", i,
                  wii_fd, events, host_events);
    // Error and hang-up events are results only, and the host may reject others.
    constexpr int RESULT_ONLY = POLLERR | POLLHUP | POLLNVAL | UNSUPPORTED_WSAPOLL;
    fd.events = static_cast<short>(host_events & ~RESULT_ONLY);
  }

  // Answered by the socket manager once ready or timed out, so emulation never blocks here.
  sockets->AddPollCommand({request.address, request.buffer_out, std::move(fds), timeout});
  return std::nullopt;
}

IPCReply NetIPTopDevice::GetHostByName(const IOCtlRequest& request)
{
  // The output is a hostent the PPC side converts itself, so these offsets are fixed:
  //   +0x000 h_name, +0x004 h_aliases, +0x008 h_addrtype, +0x00a h_length, +0x00c h_addr_list
  //   +0x010 the name, +0x110 the addresses, +0x340 the address pointers (null-terminated).
  constexpr u32 OUTPUT_SIZE = 0x460;
  constexpr u32 NAME_OFFSET = 0x10;
  constexpr u32 ADDRESSES_OFFSET = 0x110;
  constexpr u32 POINTERS_OFFSET = 0x340;
  // Room for 72 pointers, one of them the terminator.
  constexpr u32 MAX_ADDRESSES = 71;

  if (request.buffer_out_size != OUTPUT_SIZE)
  {
    ERROR_LOG_FMT(IOS_NET, "IOCTL_SO_GETHOSTBYNAME: bad output size");
    return IPCReply(-1);
  }

  auto& memory = GetSystem().GetMemory();
  const std::string name = memory.GetString(request.buffer_in);
  const hostent* const host = gethostbyname(name.c_str());
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_GETHOSTBYNAME {}, in ({:08x}, {}), out ({:08x}, {})", name,
               request.buffer_in, request.buffer_in_size, request.buffer_out,
               request.buffer_out_size);
  if (host == nullptr)
    return IPCReply(-1);

  for (int i = 0; host->h_aliases[i] != nullptr; ++i)
    DEBUG_LOG_FMT(IOS_NET, "alias{}:{}", i, host->h_aliases[i]);
  u32 address_count = 0;
  for (; host->h_addr_list[address_count] != nullptr; ++address_count)
  {
    const u8* const ip = reinterpret_cast<const u8*>(host->h_addr_list[address_count]);
    DEBUG_LOG_FMT(IOS_NET, "addr{}:{}.{}.{}.{}", address_count, ip[0], ip[1], ip[2], ip[3]);
  }

  const u32 out = request.buffer_out;
  const u32 name_size = static_cast<u32>(std::strlen(host->h_name)) + 1;
  if (name_size > ADDRESSES_OFFSET - NAME_OFFSET)
  {
    ERROR_LOG_FMT(IOS_NET, "IOCTL_SO_GETHOSTBYNAME: host name too long");
    return IPCReply(-1);
  }
  memory.CopyToEmu(out + NAME_OFFSET, host->h_name, name_size);
  memory.Write_U32(out + NAME_OFFSET, out);

  address_count = std::min(address_count, MAX_ADDRESSES);
  for (u32 i = 0; i < address_count; ++i)
    memory.CopyToEmu(out + ADDRESSES_OFFSET + 4 * i, host->h_addr_list[i], sizeof(u32));

  memory.Write_U32(out + POINTERS_OFFSET, out + 0xc);
  for (u32 i = 0; i < address_count; ++i)
    memory.Write_U32(out + ADDRESSES_OFFSET + 4 * i, out + POINTERS_OFFSET + 4 * i);
  const u32 terminator = out + POINTERS_OFFSET + 4 * address_count;
  memory.Write_U32(0, terminator);
  // No aliases (hardware gives none either): point at the empty end of the pointer list.
  memory.Write_U32(terminator, out + 4);

  ASSERT_MSG(IOS_NET, host->h_addrtype == AF_INET && host->h_length == sizeof(u32),
             "returned host info is not IPv4");
  memory.Write_U16(AF_INET, out + 8);
  memory.Write_U16(sizeof(u32), out + 0xa);
  return IPCReply(0);
}

IPCReply NetIPTopDevice::ICMPCancel(const IOCtlRequest&)
{
  ERROR_LOG_FMT(IOS_NET, "IOCTL_SO_ICMPCANCEL");
  return IPCReply(0);
}

IPCReply NetIPTopDevice::GetInterfaceOpt(const IOCtlVRequest& request)
{
  auto& memory = GetSystem().GetMemory();
  const IOCtlVRequest::IOVector& in = request.in_vectors[0];
  const IOCtlVRequest::IOVector& out = request.io_vectors[0];
  const IOCtlVRequest::IOVector& out_size = request.io_vectors[1];

  const u32 interface_id = memory.Read_U32(in.address);
  const u32 option = memory.Read_U32(in.address + 4);
  const u32 out_first = memory.Read_U32(out.address);
  const u32 out_capacity = memory.Read_U32(out_size.address);

  if (interface_id != 0xfffe)
  {
    WARN_LOG_FMT(IOS_NET, "GetInterfaceOpt: invalid interface {:08x}", interface_id);
    return IPCReply(SO_ERROR_INVALID_REQUEST);
  }

  // Where to continue writing a routing table, if the buffer says so.
  u32 cursor = out.size >= 8 ? memory.Read_U32(out.address + 4) : 0;

  INFO_LOG_FMT(IOS_NET,
               "IOCTLV_SO_GETINTERFACEOPT({:08X}, {:08X}, {:X}, {:X}, {:X}) in ({:08x}, {}), "
               "in2 ({:08x}, {})",
               interface_id, option, out_first, out_capacity, cursor, in.address, in.size,
               request.in_vectors.size() > 1 ? request.in_vectors[1].address : 0,
               request.in_vectors.size() > 1 ? request.in_vectors[1].size : 0);

  switch (option)
  {
  case 0xb003:
  {
    // DNS servers: the host's first one (or Google's), then Google's second.
    const u32 google_primary = ntohl(inet_addr("8.8.8.8"));
    const u32 google_secondary = ntohl(inet_addr("8.8.4.4"));
    u32 primary = FindHostDnsServer(google_primary);
    if (primary == 0)
      primary = google_primary;
    INFO_LOG_FMT(IOS_NET, "Primary DNS: {:X}", primary);
    INFO_LOG_FMT(IOS_NET, "Secondary DNS: {:X}", google_secondary);
    memory.Write_U32(primary, out.address);
    memory.Write_U32(google_secondary, out.address + 4);
    break;
  }

  case 0x1003:  // Last error.
    memory.Write_U32(0, out.address);
    break;

  case 0x1004:  // MAC address.
  {
    const Common::MACAddress mac = IOS::Net::GetMACAddress();
    memory.CopyToEmu(out.address, mac.data(), mac.size());
    break;
  }

  case 0x1005:  // Link is up.
  case 0x4002:  // Number of IP addresses.
    memory.Write_U32(1, out.address);
    break;

  case 0x3001:
    memory.Write_U32(0x10, out.address);
    break;

  case 0x4003:
  {
    // Address, netmask and broadcast address. (Depending on interface settings IOS can add
    // twelve more bytes when the buffer allows; that is not done here.)
    memory.Write_U32(0xc, out_size.address);
    const HostInterface host = HostInterfaceOrFallback();
    memory.Write_U32(ntohl(host.address.s_addr), out.address);
    memory.Write_U32(ntohl(host.netmask.s_addr), out.address + 4);
    memory.Write_U32(ntohl(host.broadcast.s_addr), out.address + 8);
    break;
  }

  case 0x4005:
    // Routing table size; IOS nearly always says 0x20.
    memory.Write_U32(0x20, out.address);
    break;

  case 0x4006:
  {
    // Routing table: 24-byte rows of destination, netmask, gateway, flags and 8 unknown
    // (zero) bytes. Bit 0 of the flags is clear when there is no gateway.
    for (const Route& route : HostInterfaceOrFallback().routes)
    {
      const u32 row = out.address + cursor;
      memory.Write_U32(ntohl(route.destination.s_addr), row);
      memory.Write_U32(ntohl(route.netmask.s_addr), row + 4);
      memory.Write_U32(ntohl(route.gateway.s_addr), row + 8);
      memory.Write_U32(route.gateway.s_addr == 0 ? 0 : 1, row + 12);
      memory.Write_U64(0, row + 16);
      cursor += 24;
      if (cursor >= out_capacity)
        break;
    }
    memory.Write_U32(cursor, out_size.address);
    break;
  }

  case 0x6003:
  case 0x600a:
  case 0x600c:
    memory.Write_U32(0x80, out.address);
    break;

  case 0xb002:
    memory.Write_U32(2, out.address);
    break;

  default:
    ERROR_LOG_FMT(IOS_NET, "GetInterfaceOpt: unknown option {:08X}", option);
    break;
  }
  return IPCReply(0);
}

std::optional<IPCReply> NetIPTopDevice::SendTo(const IOCtlVRequest& request)
{
  // The fd is the first word of the second input vector.
  const u32 fd = GetSystem().GetMemory().Read_U32(request.in_vectors[1].address);
  GetEmulationKernel().GetSocketManager()->DoSock(static_cast<s32>(fd), request, IOCTLV_SO_SENDTO);
  return std::nullopt;
}

std::optional<IPCReply> NetIPTopDevice::RecvFrom(const IOCtlVRequest& request)
{
  const u32 fd = GetSystem().GetMemory().Read_U32(request.in_vectors[0].address);
  GetEmulationKernel().GetSocketManager()->DoSock(static_cast<s32>(fd), request,
                                                  IOCTLV_SO_RECVFROM);
  return std::nullopt;
}

IPCReply NetIPTopDevice::GetAddrInfo(const IOCtlVRequest& request)
{
  auto& system = GetSystem();
  auto& memory = system.GetMemory();
  const auto& in = request.in_vectors;

  // Inputs: node name, service name, hints (each optional).
  const bool has_hints = in.size() > 2 && in[2].size != 0;
  addrinfo hints;
  if (has_hints)
  {
    const u32 h = in[2].address;
    hints.ai_flags = static_cast<int>(memory.Read_U32(h));
    hints.ai_family = static_cast<int>(memory.Read_U32(h + 4));
    hints.ai_socktype = static_cast<int>(memory.Read_U32(h + 8));
    hints.ai_protocol = static_cast<int>(memory.Read_U32(h + 0xc));
    hints.ai_addrlen = memory.Read_U32(h + 0x10);
    hints.ai_canonname = nullptr;
    hints.ai_addr = nullptr;
    hints.ai_next = nullptr;
  }

  std::optional<std::string> node;
  if (!in.empty() && in[0].size > 0)
  {
    node = memory.GetString(in[0].address, in[0].size);
    if (std::optional<std::string> patched =
            WC24PatchEngine::GetNetworkPatch(*node, WC24PatchEngine::IsKD{false}))
    {
      node = std::move(patched);
    }
  }
  std::optional<std::string> service;
  if (in.size() > 1 && in[1].size > 0)
    service = memory.GetString(in[1].address, in[1].size);

  addrinfo* results = nullptr;
  int ret = getaddrinfo(node ? node->c_str() : nullptr, service ? service->c_str() : nullptr,
                        has_hints ? &hints : nullptr, &results);
  if (ret == 0)
  {
    // 32-byte Wii addrinfo entries from the start of the output, with the socket addresses
    // (28 bytes each) from +0x460. There is no room for canonical names.
    constexpr u32 ENTRY_SIZE = 0x20;
    u32 entry = request.io_vectors[0].address;
    u32 address_slot = entry + 0x460;
    for (const addrinfo* result = results; result != nullptr; result = result->ai_next)
    {
      memory.Write_U32(static_cast<u32>(result->ai_flags), entry);
      memory.Write_U32(static_cast<u32>(result->ai_family), entry + 4);
      memory.Write_U32(static_cast<u32>(result->ai_socktype), entry + 8);
      memory.Write_U32(static_cast<u32>(result->ai_protocol), entry + 0xc);
      memory.Write_U32(static_cast<u32>(result->ai_addrlen), entry + 0x10);
      memory.Write_U32(0, entry + 0x14);

      if (result->ai_addr != nullptr)
      {
        memory.Write_U32(address_slot, entry + 0x18);
        memory.Write_U8(static_cast<u8>(result->ai_addrlen & 0xff), address_slot);
        memory.Write_U8(static_cast<u8>(result->ai_addr->sa_family & 0xff), address_slot + 1);
        memory.CopyToEmu(address_slot + 2, result->ai_addr->sa_data,
                         sizeof(result->ai_addr->sa_data));
        address_slot += 0x1c;
      }
      else
      {
        memory.Write_U32(0, entry + 0x18);
      }

      memory.Write_U32(result->ai_next != nullptr ? entry + ENTRY_SIZE : 0, entry + 0x1c);
      entry += ENTRY_SIZE;
    }
    freeaddrinfo(results);
  }
  else
  {
    ERROR_LOG_FMT(IOS_NET,
                  "getaddrinfo failed with error {}: {}\n - hostname: {}\n - service: {}\n"
                  " - hints: {}",
                  ret, DescribeLookupError(ret), node ? *node : "(null)",
                  service ? *service : "(null)", has_hints ? DescribeHints(hints) : "(null)");
    ret = SO_ERROR_HOST_NOT_FOUND;
  }

  request.Dump(system, GetDeviceName(), Common::Log::LogType::IOS_NET,
               Common::Log::LogLevel::LINFO);
  return IPCReply(ret);
}

IPCReply NetIPTopDevice::ICMPPing(const IOCtlVRequest& request)
{
  // First input: +0 fd, +4 address count, +8 timeout (64-bit), +16 one address: length,
  // family, ICMP id (16-bit), IPv4 address. An optional second input holds the 32-byte packet.
  auto& memory = GetSystem().GetMemory();
  const u32 args = request.in_vectors[0].address;
  const u32 fd = memory.Read_U32(args);
  const u32 address_count = memory.Read_U32(args + 4);
  const u64 timeout = memory.Read_U64(args + 8);
  if (address_count != 1)
    INFO_LOG_FMT(IOS_NET, "IOCTLV_SO_ICMPPING {} IPs", address_count);

  const u8 length = memory.Read_U8(args + 16);
  const u8 family = memory.Read_U8(args + 17);
  const u16 icmp_id = memory.Read_U16(args + 18);
  const u32 ip = memory.Read_U32(args + 20);
  if (length != 8 || family != AF_INET)
    INFO_LOG_FMT(IOS_NET, "IOCTLV_SO_ICMPPING unusual address: length {:x} family {:x}", length,
                 family);
  INFO_LOG_FMT(IOS_NET, "IOCTLV_SO_ICMPPING {:x}", ip);

  sockaddr_in target{};
  target.sin_family = AF_INET;
  target.sin_addr.s_addr = Common::swap32(ip);

  std::array<u8, 0x20> packet{};
  u32 packet_size = static_cast<u32>(packet.size());
  if (request.in_vectors.size() > 1 && request.in_vectors[1].size == packet.size())
  {
    memory.CopyFromEmu(packet.data(), request.in_vectors[1].address, packet.size());
  }
  else
  {
    // Just the identifier; the sequence number stays 0 (how IOS picks it does not matter).
    const u16 id_field = Common::swap16(icmp_id);
    std::memcpy(packet.data(), &id_field, sizeof(id_field));
    packet_size = 22;
  }

  const s32 host_socket =
      GetEmulationKernel().GetSocketManager()->GetHostSocket(static_cast<s32>(fd));
  const auto host_fd = static_cast<u32>(host_socket);
  if (icmp_echo_req(host_fd, &target, packet.data(), packet_size) == static_cast<int>(packet_size))
    icmp_echo_rep(host_fd, &target, static_cast<u32>(timeout), packet_size);

  // The ping's outcome is not reported.
  return IPCReply(0);
}
}  // namespace IOS::HLE
