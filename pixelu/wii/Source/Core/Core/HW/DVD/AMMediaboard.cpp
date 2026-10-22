// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/DVD/AMMediaboard.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "Common/ChunkFile.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Network.h"
#include "Common/StringUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/EXI/EXI_DeviceBaseboard.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"
#include "DiscIO/Blob.h"
#include "DiscIO/CachedBlob.h"
#include "VideoCommon/OnScreenDisplay.h"

namespace AMMediaboard
{

// ================================================================================================
// Host sockets
// ================================================================================================

namespace
{
#ifdef _WIN32
using HostSocket = SOCKET;
constexpr HostSocket NO_SOCKET = INVALID_SOCKET;
using PollEntry = WSAPOLLFD;
using SockLen = int;
using IoLength = int;

int SocketError()
{
  return WSAGetLastError();
}

int CloseSocket(HostSocket s)
{
  return closesocket(s);
}

int SetBlocking(HostSocket s, bool blocking)
{
  u_long non_blocking = blocking ? 0 : 1;
  return ioctlsocket(s, static_cast<long>(FIONBIO), &non_blocking);
}

int PollSockets(PollEntry* entries, std::size_t count, int timeout_ms)
{
  return WSAPoll(entries, static_cast<ULONG>(count), timeout_ms);
}
#else
using HostSocket = int;
constexpr HostSocket NO_SOCKET = -1;
using PollEntry = pollfd;
using SockLen = socklen_t;
using IoLength = std::size_t;

// Windows numbering: a call that would block reports WSAEWOULDBLOCK.
int SocketError()
{
  const int error = errno;
  if (error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS)
    return 10035;
  return error;
}

int CloseSocket(HostSocket s)
{
  return close(s);
}

int SetBlocking(HostSocket s, bool blocking)
{
  int non_blocking = blocking ? 0 : 1;
  return ioctl(s, FIONBIO, &non_blocking);
}

int PollSockets(PollEntry* entries, std::size_t count, int timeout_ms)
{
  int result;
  do
  {
    result = poll(entries, static_cast<nfds_t>(count), timeout_ms);
  } while (result == -1 && errno == EINTR);
  return result;
}
#endif

constexpr int WOULD_BLOCK = 10035;
constexpr int SOCKET_FAILED = -1;

// For logs: the handle as a signed number (-1 for "none" on every platform).
s64 Show(HostSocket s)
{
  return s == NO_SOCKET ? -1 : static_cast<s64>(s);
}

// ================================================================================================
// Board state
// ================================================================================================

constexpr std::size_t FIRMWARE_SIZE = 0x200000;
constexpr std::size_t MEDIA_BUFFER_SIZE = 0x300;
constexpr std::size_t NETWORK_COMMAND_SIZE = 0x4FFE00;
constexpr std::size_t NETWORK_BUFFER_SIZE = 0x80000;
constexpr std::size_t ALLNET_BUFFER_SIZE = 0x1000;
constexpr std::size_t ALLNET_SETTINGS_SIZE = 0x8500;

constexpr u32 STATUS_OK = 70;  // SSC_SUCCESS
constexpr u32 STATUS_FAILED = 0xFFFFFFFF;

bool s_firmware_map = false;
bool s_test_menu = false;
std::array<u32, 3> s_timeouts{20000, 20000, 20000};
u32 s_last_error = STATUS_OK;
u32 s_key_a = 0;
u32 s_key_b = 0;
u32 s_key_c = 0;

std::array<u8, FIRMWARE_SIZE> s_firmware{};
alignas(4) std::array<u8, MEDIA_BUFFER_SIZE> s_media{};
std::array<u32, 8> s_exec1_reply{};
std::array<u32, 8> s_exec2_reply{};
std::array<u8, NETWORK_COMMAND_SIZE> s_net_commands{};
std::array<u8, NETWORK_BUFFER_SIZE> s_net_buffer{};
std::array<u8, ALLNET_BUFFER_SIZE> s_allnet_buffer{};
std::array<u8, ALLNET_SETTINGS_SIZE> s_allnet_settings{};

Common::IPAddress s_game_ip{};
bool s_netconfig_pending = false;
u32 s_board_status = LoadingGameProgram;
u32 s_load_progress = 80;

std::array<HostSocket, SOCKET_FD_MAX> s_sockets;
u32 s_next_fd = 1;

File::IOFile s_netcfg;
File::IOFile s_netctrl;
File::IOFile s_extra;
File::IOFile s_dimm;
File::IOFile s_backup;

std::unique_ptr<DiscIO::BlobReader> s_disc;
CoreTiming::EventType* s_test_event = nullptr;

// The copy length is the size of this text with its NUL (230 bytes). The spec
// states 252, which does not match its own text; see docs/w43-log.txt.
constexpr char ALLNET_REPLY[] =
    "uri=http://sega.com&host=sega.com&nickname=sega&name=sega&year=2025&month=08&day=16"
    "&hour=21&minute=10&second=12&place_id=1234&setting=0x123&region0=jap&region_name0=japan"
    "&region_name1=usa&region_name2=asia&region_name3=export&end";
static_assert(sizeof(ALLNET_REPLY) == 230);

constexpr std::string_view BOARD_SERIAL = "A89E-27A50364511";

constexpr u32 TEST_OK_1 = 0x54455354;
constexpr u32 TEST_OK_2 = 0x204F4B00;

// Lookup order matters: the first range containing an address wins.
const std::array<MediaBoardRange, 12> s_ranges{{
    {DIMMCommandVersion1, 0x40, s_media},
    {DIMMCommandVersion2, 0x60, s_media},
    {DIMMCommandVersion2_2, 0x220, s_media},
    {NetworkCommandAddress1, 0x1040, s_net_commands},
    {NetworkCommandAddress2, 0x40000, s_net_commands},
    {NetworkBufferAddress1, 0x10000, s_net_buffer},
    {NetworkBufferAddress2, 0x10000, s_net_buffer},
    {NetworkBufferAddress3, 0x50000, s_net_buffer},
    {NetworkBufferAddress4, 0xC0000, s_net_buffer},
    {NetworkBufferAddress5, 0x10000, s_net_buffer},
    {AllNetSettings, 0x8000, s_allnet_settings},
    {AllNetBuffer, 0x1000, s_allnet_buffer},
}};

// ---- Media buffer words (host order) ---------------------------------------------------------

u32 Word(std::size_t index)
{
  u32 value;
  std::memcpy(&value, &s_media[index * 4], sizeof(value));
  return value;
}

void SetWord(std::size_t index, u32 value)
{
  std::memcpy(&s_media[index * 4], &value, sizeof(value));
}

u16 Half(std::size_t byte_offset)
{
  return static_cast<u16>(s_media[byte_offset] | (s_media[byte_offset + 1] << 8));
}

void ClearMedia(std::size_t from, std::size_t to)
{
  std::fill(s_media.begin() + static_cast<std::ptrdiff_t>(from),
            s_media.begin() + static_cast<std::ptrdiff_t>(to), u8{0});
}

void SaveReply(std::array<u32, 8>& reply)
{
  std::memcpy(reply.data(), s_media.data(), sizeof(reply));
}

Memory::MemoryManager& Mem()
{
  return Core::System::GetInstance().GetMemory();
}

// From `address` to the end of the buffer of the first range that holds it.
std::span<u8> BoardSpan(u32 address)
{
  for (const MediaBoardRange& range : s_ranges)
  {
    if (address >= range.start && address < range.end)
    {
      const std::size_t skip = address - range.start;
      return {range.buffer + skip, range.buffer_size - skip};
    }
  }
  return {};
}

bool SafeCopyToEmu(u32 address, const u8* source, std::size_t source_size, std::size_t offset,
                   std::size_t length)
{
  if (offset > source_size || length > source_size - offset)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Read overflow: offset=0x{:08x}, length={}, source_size={}",
                  offset, length, source_size);
    return false;
  }
  const std::span<u8> target = Mem().GetSpanForAddress(address);
  if (target.size() < length)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Memory buffer too small: address=0x{:08x}, length={}, span={}",
                  address, length, target.size());
    return false;
  }
  std::memcpy(target.data(), source + offset, length);
  return true;
}

bool SafeCopyFromEmu(u8* destination, u32 address, std::size_t destination_size,
                     std::size_t offset, std::size_t length)
{
  if (offset > destination_size || length > destination_size - offset)
  {
    ERROR_LOG_FMT(AMMEDIABOARD,
                  "GC-AM: Write overflow: offset=0x{:08x}, length={}, destination_size={}", offset,
                  length, destination_size);
    return false;
  }
  const std::span<u8> source = Mem().GetSpanForAddress(address);
  if (source.size() < length)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Memory buffer too small: address=0x{:08x}, length={}, span={}",
                  address, length, source.size());
    return false;
  }
  std::memcpy(destination + offset, source.data(), length);
  return true;
}

void FileReadData(File::IOFile& file, u64 position, u32 address, u32 length)
{
  const std::span<u8> target = Mem().GetSpanForAddress(address);
  if (target.size() < length)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Read overflow: address=0x{:08x}, length={}, span={}",
                  address, length, target.size());
    return;
  }
  file.Seek(static_cast<s64>(position), File::SeekOrigin::Begin);
  file.ReadBytes(target.data(), length);
}

void FileWriteData(File::IOFile& file, u64 position, u32 address, u32 length)
{
  const std::span<u8> source = Mem().GetSpanForAddress(address);
  if (source.size() < length)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Write overflow: address=0x{:08x}, length={}, span={}",
                  address, length, source.size());
    return;
  }
  file.Seek(static_cast<s64>(position), File::SeekOrigin::Begin);
  file.WriteBytes(source.data(), length);
  file.Flush();
}

// A NUL-terminated string from board memory, at most `limit` characters.
std::string BoardString(u32 address, std::size_t limit)
{
  const std::span<u8> bytes = BoardSpan(address);
  const std::size_t max = std::min(limit, bytes.size());
  std::size_t length = 0;
  while (length < max && bytes[length] != 0)
    ++length;
  return std::string(reinterpret_cast<const char*>(bytes.data()), length);
}

void WriteTestOK(u32 address)
{
  auto& memory = Mem();
  memory.Write_U32_Swap(TEST_OK_1, address);
  memory.Write_U32_Swap(TEST_OK_2, address + 4);
}

void ScheduleTestPhase2(u64 which)
{
  auto& system = Core::System::GetInstance();
  auto& timing = system.GetCoreTiming();
  timing.RemoveEvent(s_test_event);
  timing.ScheduleEvent(system.GetSystemTimers().GetTicksPerSecond() / 10000, s_test_event, which);
}

void TestHardwarePhase2(Core::System&, u64 userdata, s64)
{
  std::array<u32, 8>& reply = userdata != 0 ? s_exec2_reply : s_exec1_reply;
  reply.fill(0);
  reply[0] = 0x03020000;
  reply[1] = 2;
  reply[2] = 100;
  DEBUG_LOG_FMT(AMMEDIABOARD,
                "GC-AM: TestHardware phase 2 ({}): sending result response (testStatus=2, "
                "checkProgress=100)",
                userdata);
  ExpansionInterface::GenerateInterrupt(userdata != 0 ? 0x10 : 0x04);
}

// ================================================================================================
// Guest sockets
// ================================================================================================

// What the guest keeps in its sockaddr (16 bytes).
struct GuestAddress
{
  u8 unknown;
  u8 family;
  u16 port;  // network order
  Common::IPAddress ip;
};

GuestAddress ReadGuestAddress(std::span<const u8> bytes)
{
  GuestAddress address{};
  address.unknown = bytes[0];
  address.family = bytes[1];
  std::memcpy(&address.port, &bytes[2], sizeof(address.port));
  std::memcpy(address.ip.data(), &bytes[4], address.ip.size());
  return address;
}

u16 HostPort(u16 network_port)
{
  return ntohs(network_port);
}

sockaddr_in MakeSockaddr(u8 family, const Common::IPv4Port& where)
{
  sockaddr_in result{};
  result.sin_family = family;
  result.sin_port = where.port;
  std::memcpy(&result.sin_addr, where.ip_address.data(), where.ip_address.size());
  return result;
}

std::string IPText(const Common::IPAddress& ip)
{
  return Common::IPAddressToString(ip);
}

s32 FreeGuestSocket()
{
  for (std::size_t attempt = 0; attempt != SOCKET_FD_MAX; ++attempt)
  {
    const u32 candidate = s_next_fd;
    s_next_fd = (s_next_fd + 1) % SOCKET_FD_MAX;
    if (candidate == 0)
      continue;
    if (s_sockets[candidate] == NO_SOCKET)
      return static_cast<s32>(candidate);
  }
  return -1;
}

HostSocket HostFor(u32 guest)
{
  if (guest < SOCKET_FD_MAX)
    return s_sockets[guest];
  WARN_LOG_FMT(AMMEDIABOARD, "GC-AM: Bad GuestSocket value: {}", guest);
  return NO_SOCKET;
}

s32 GuestFor(HostSocket host)
{
  for (std::size_t i = 1; i != SOCKET_FD_MAX; ++i)
  {
    if (s_sockets[i] == host)
      return static_cast<s32>(i);
  }
  ERROR_LOG_FMT(AMMEDIABOARD, "GuestSocket not found. This should not happen.");
  return -1;
}

void CloseAllSockets()
{
  for (std::size_t i = 1; i != SOCKET_FD_MAX; ++i)
  {
    if (s_sockets[i] != NO_SOCKET)
    {
      CloseSocket(s_sockets[i]);
      s_sockets[i] = NO_SOCKET;
    }
  }
}

std::optional<IPRedirection> FindRedirection(const Common::IPv4Port& subject, bool by_replacement)
{
  for (const IPRedirection& rule : GetIPRedirections())
  {
    const Common::IPv4PortRange& range = by_replacement ? rule.replacement : rule.original;
    if (range.IsMatch(subject))
      return rule;
  }
  return std::nullopt;
}

// Binds `s` to a port in [first, first + 999] (or any port when first is 0),
// trying random ports.
bool BindEphemeralPort(HostSocket s, const Common::IPv4Port& where)
{
  const u16 first = where.GetPortValue();
  const bool any = first == 0;
  const int attempts = any ? 1 : 10;
  const int spread = any ? 0 : 999;

  std::mt19937 generator(
      static_cast<u32>(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::uniform_int_distribution<int> distribution(0, spread);

  for (int i = 0; i != attempts; ++i)
  {
    const u16 port = static_cast<u16>(first + distribution(generator));
    const Common::IPv4Port target{where.ip_address, htons(port)};
    const sockaddr_in address = MakeSockaddr(AF_INET, target);
    const int result =
        bind(s, reinterpret_cast<const sockaddr*>(&address), static_cast<SockLen>(sizeof(address)));
    INFO_LOG_FMT(AMMEDIABOARD, "BindEphemeralPort: bind ({}:{}) = {} ({})", IPText(target.ip_address),
                 port, result, Common::StrNetworkError());
    if (result == 0)
      return true;
  }
  return false;
}

u32 NetDIMMConnect(u32 guest, const GuestAddress& guest_address)
{
  Common::IPv4Port destination{guest_address.ip, guest_address.port};
  INFO_LOG_FMT(AMMEDIABOARD, "NetDIMMConnect: {}:{}", IPText(destination.ip_address),
               HostPort(destination.port));

  if (const auto rule = FindRedirection(destination, false))
  {
    destination = rule->Apply(destination);
    INFO_LOG_FMT(AMMEDIABOARD, "NetDIMMConnect: Redirecting to: {}:{}",
                 IPText(destination.ip_address), HostPort(destination.port));
  }

  const sockaddr_in address = MakeSockaddr(guest_address.family, destination);
  const HostSocket s = HostFor(guest);

  // Pin the source address when the game's own IP is redirected.
  const Common::IPv4Port own{s_game_ip, 0};
  if (const auto rule = FindRedirection(own, false))
  {
    if (!BindEphemeralPort(s, rule->Apply(own)))
    {
      s_last_error = STATUS_FAILED;
      return STATUS_FAILED;
    }
  }

  SetBlocking(s, false);
  const auto finish = [s](u32 last_error, u32 result) {
    SetBlocking(s, true);
    s_last_error = last_error;
    return result;
  };

  const int result =
      connect(s, reinterpret_cast<const sockaddr*>(&address), static_cast<SockLen>(sizeof(address)));
  const int error = SocketError();
  INFO_LOG_FMT(AMMEDIABOARD_NET, "NetDIMMConnect: connect( {}({}), ({},{}:{}) ):{} ({})", Show(s),
               guest, guest_address.family, IPText(destination.ip_address),
               HostPort(destination.port), result, error);

  if (result == 0)
    return finish(STATUS_OK, 0);

  if (error != WOULD_BLOCK)
  {
    WARN_LOG_FMT(AMMEDIABOARD, "NetDIMMConnect: connect: {} ({})", error,
                 Common::StrNetworkError());
    return finish(STATUS_FAILED, STATUS_FAILED);
  }

  PollEntry entry{};
  entry.fd = s;
  entry.events = POLLOUT;
  const int timeout_ms = static_cast<int>(s_timeouts[0] / 1000);
  const int ready = PollSockets(&entry, 1, timeout_ms);
  if (ready < 0)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "NetDIMMConnect: PlatformPoll: {}", Common::StrNetworkError());
    return finish(STATUS_FAILED, STATUS_FAILED);
  }
  if ((entry.revents & (POLLOUT | POLLERR)) == 0)
  {
    // Still connecting.
    return finish(STATUS_OK, STATUS_FAILED);
  }

  int connect_error = 0;
  SockLen error_size = sizeof(connect_error);
  if (getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connect_error), &error_size) < 0)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "NetDIMMConnect: getsockopt: {}", Common::StrNetworkError());
  }
  else if (connect_error == 0)
  {
    INFO_LOG_FMT(AMMEDIABOARD_NET, "NetDIMMConnect: connect( {}({}) ) succeeded", Show(s), guest);
    return finish(STATUS_OK, 0);
  }

  ERROR_LOG_FMT(AMMEDIABOARD_NET, "NetDIMMConnect: connect( {}({}) ) failed with error {}: {}",
                Show(s), guest, connect_error, Common::DecodeNetworkError(connect_error));
  return finish(STATUS_FAILED, STATUS_FAILED);
}

u32 NetDIMMAccept(u32 guest, u8* address_out, u8* length_inout)
{
  if ((address_out == nullptr) != (length_inout == nullptr))
  {
    ERROR_LOG_FMT(AMMEDIABOARD_NET, "NetDIMMAccept: bad parmeters");
    s_last_error = 14;
    return STATUS_FAILED;
  }

  const HostSocket s = HostFor(guest);
  DEBUG_LOG_FMT(AMMEDIABOARD, "NetDIMMAccept: {}({})", Show(s), guest);

  PollEntry entry{};
  entry.fd = s;
  entry.events = POLLIN;
  const int ready = PollSockets(&entry, 1, 0);
  if (ready < 0)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "NetDIMMAccept: PlatformPoll: {}", Common::StrNetworkError());
    s_last_error = STATUS_FAILED;
    return STATUS_FAILED;
  }
  if ((entry.revents & POLLIN) == 0)
  {
    DEBUG_LOG_FMT(AMMEDIABOARD, "NetDIMMAccept: Timeout.");
    s_last_error = STATUS_OK;
    return STATUS_FAILED;
  }

  const s32 new_guest = FreeGuestSocket();
  sockaddr_in peer{};
  SockLen peer_size = sizeof(peer);
  const HostSocket accepted = accept(s, reinterpret_cast<sockaddr*>(&peer), &peer_size);
  if (new_guest < 0 || accepted == NO_SOCKET)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "AMMBCommandAccept: accept( {}({}) ) failed: {}", Show(s), guest,
                  Common::StrNetworkError());
    s_last_error = STATUS_FAILED;
    return STATUS_FAILED;
  }

  Common::SetPlatformSocketOptions(static_cast<int>(accepted));
  s_sockets[static_cast<std::size_t>(new_guest)] = accepted;
  s_last_error = STATUS_OK;

  Common::IPv4Port from{};
  std::memcpy(from.ip_address.data(), &peer.sin_addr, from.ip_address.size());
  from.port = peer.sin_port;
  NOTICE_LOG_FMT(AMMEDIABOARD, "AMMBCommandAccept: {}:{}", IPText(from.ip_address),
                 HostPort(from.port));

  if (address_out != nullptr)
  {
    if (const auto rule = FindRedirection(from, true))
    {
      from = rule->Reverse(from);
      NOTICE_LOG_FMT(AMMEDIABOARD, "AMMBCommandAccept: Translating result to: {}:{}",
                     IPText(from.ip_address), HostPort(from.port));
    }

    std::array<u8, 16> guest_address{};
    guest_address[1] = static_cast<u8>(peer.sin_family);
    std::memcpy(&guest_address[2], &from.port, sizeof(from.port));
    std::memcpy(&guest_address[4], from.ip_address.data(), from.ip_address.size());

    u32 wanted;
    std::memcpy(&wanted, length_inout, sizeof(wanted));
    std::memcpy(address_out, guest_address.data(), std::min<u32>(wanted, 16));
    // Only the low byte of the length word is updated.
    *length_inout = 16;
  }

  return static_cast<u32>(new_guest);
}

u32 NetDIMMBind(u32 guest, const GuestAddress& guest_address)
{
  const HostSocket s = HostFor(guest);
  NOTICE_LOG_FMT(AMMEDIABOARD, "NetDIMMBind: {}({}) {}, {}, {}:{}", Show(s), guest,
                 guest_address.unknown, guest_address.family, IPText(guest_address.ip),
                 HostPort(guest_address.port));

  Common::IPv4Port considered{guest_address.ip, guest_address.port};
  if (considered.ip_address == Common::IP_ADDR_ANY)
  {
    considered.ip_address = s_game_ip;
    INFO_LOG_FMT(AMMEDIABOARD, "GetAdjustedBindIPv4Port: Considering game modified IP: {}",
                 IPText(s_game_ip));
  }

  Common::IPv4Port target{guest_address.ip, guest_address.port};
  if (const auto rule = FindRedirection(considered, false))
  {
    target = rule->Apply(considered);
    INFO_LOG_FMT(AMMEDIABOARD, "GetAdjustedBindIPv4Port: Redirecting to: {}:{}",
                 IPText(target.ip_address), HostPort(target.port));
  }

  const sockaddr_in address = MakeSockaddr(guest_address.family, target);
  const int result =
      bind(s, reinterpret_cast<const sockaddr*>(&address), static_cast<SockLen>(sizeof(address)));
  INFO_LOG_FMT(AMMEDIABOARD_NET, "NetDIMMBind: bind( {}({}), ({},{}:{}) ):{}", Show(s), guest,
               guest_address.family, IPText(target.ip_address), HostPort(target.port), result);

  if (result < 0)
  {
    const std::string message = fmt::format("Failed to bind socket {}:{}",
                                            IPText(target.ip_address), HostPort(target.port));
    ERROR_LOG_FMT(AMMEDIABOARD, "NetDIMMBind: {} with error {}: {}", message, SocketError(),
                  Common::StrNetworkError());
    OSD::AddMessage(message, OSD::Duration::SHORT, OSD::Color::RED);
  }
  return static_cast<u32>(result);
}

// ---- The socket commands. `p` is the media buffer word holding the first parameter. ---------

void ReplyWithByte8(u32 result)
{
  s_media[1] = s_media[8];
  SetWord(1, result);
}

void CommandSocket(std::size_t p)
{
  const u32 domain = Word(p);
  const u32 type = Word(p + 1);
  u32 result = STATUS_FAILED;

  const s32 guest = FreeGuestSocket();
  if (guest >= 0)
  {
    const HostSocket s = socket(static_cast<int>(domain), static_cast<int>(type), 0);
    if (s == NO_SOCKET)
    {
      ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: failed to create socket ({})", Common::StrNetworkError());
    }
    else
    {
      Common::SetPlatformSocketOptions(static_cast<int>(s));
      s_sockets[static_cast<std::size_t>(guest)] = s;
      result = static_cast<u32>(guest);
    }
  }

  NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: socket( {}, {} ):{}", domain, type, result);
  s_media[1] = 0;
  SetWord(1, result);
}

void CommandCloseSocket(std::size_t p)
{
  const u32 guest = Word(p);
  const HostSocket s = HostFor(guest);
  const int result = CloseSocket(s);
  if (guest < SOCKET_FD_MAX)
    s_sockets[guest] = NO_SOCKET;
  s_last_error = STATUS_OK;
  NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: closesocket( {}({}) ):{}", Show(s), guest, result);
  SetWord(1, static_cast<u32>(result));
}

void CommandConnect(std::size_t p)
{
  const u32 guest = Word(p);
  const u32 address_offset = Word(p + 1);
  const u32 length = Word(p + 2);
  if (length != 16)
  {
    ERROR_LOG_FMT(AMMEDIABOARD_NET, "AMMBCommandConnect: Unexpected length: {}", length);
    return;
  }
  const std::span<u8> address = BoardSpan(address_offset);
  if (address.size() < 16)
  {
    ERROR_LOG_FMT(AMMEDIABOARD_NET, "AMMBCommandConnect: Bad address offset: {:08x}",
                  address_offset);
    return;
  }
  ReplyWithByte8(NetDIMMConnect(guest, ReadGuestAddress(address)));
}

void CommandAccept(std::size_t p)
{
  const u32 guest = Word(p);
  const u32 address_offset = Word(p + 1);
  const u32 length_offset = Word(p + 2);

  // The address buffer is only looked at when the length word is usable.
  u8* length_ptr = nullptr;
  u8* address_ptr = nullptr;
  const std::span<u8> length_span = BoardSpan(length_offset);
  if (length_span.size() >= 4)
  {
    length_ptr = length_span.data();
    u32 length;
    std::memcpy(&length, length_ptr, sizeof(length));

    const std::span<u8> address_span = BoardSpan(address_offset);
    if (address_span.size() >= length)
    {
      address_ptr = address_span.data();
    }
    else
    {
      ERROR_LOG_FMT(AMMEDIABOARD_NET,
                    "AMMBCommandAccept: Bad address offset or addrlen: off={:08x} len={}",
                    address_offset, length);
    }
  }

  SetWord(1, NetDIMMAccept(guest, address_ptr, length_ptr));
}

void CommandBind()
{
  const u32 guest = Word(2);
  const u32 address_offset = Word(3);
  const u32 length = Word(4);
  if (length != 16)
  {
    ERROR_LOG_FMT(AMMEDIABOARD_NET, "AMMBCommandBind: Unexpected length: {}", length);
    return;
  }
  const std::span<u8> address = BoardSpan(address_offset);
  if (address.size() < 16)
  {
    ERROR_LOG_FMT(AMMEDIABOARD_NET, "AMMBCommandBind: Bad address offset: {:08x}", address_offset);
    return;
  }
  const u32 result = NetDIMMBind(guest, ReadGuestAddress(address));
  s_last_error = STATUS_OK;
  SetWord(1, result);
}

void CommandListen()
{
  const u32 guest = Word(2);
  const u32 backlog = Word(3);
  const HostSocket s = HostFor(guest);
  const int result = listen(s, static_cast<int>(backlog));
  NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: listen( {}, {} ):{:d}", Show(s), backlog, result);
  ReplyWithByte8(static_cast<u32>(result));
}

void CommandRecv(std::size_t p)
{
  const u32 guest = Word(p);
  const u32 offset = Word(p + 1);
  u32 length = Word(p + 2);
  const HostSocket s = HostFor(guest);

  const std::span<u8> buffer = BoardSpan(offset);
  if (buffer.size() < length)
  {
    ERROR_LOG_FMT(AMMEDIABOARD_NET, "AMMBCommandRecv: Bad data offset or length: off={:08x} len={}",
                  offset, length);
    length = 0;
  }

  const auto result =
      recv(s, reinterpret_cast<char*>(buffer.data()), static_cast<IoLength>(length), 0);
  if (result < 0)
  {
    // A non-blocking socket with nothing to do yet is normal and not logged.
    const int error = SocketError();
    if (error != WOULD_BLOCK)
    {
      ERROR_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: recv( {}, 0x{:08x}, {} ) failed with error {}: {}",
                    Show(s), offset, length, error, Common::StrNetworkError());
    }
  }
  else if (result == 0)
  {
    INFO_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: recv( {}, 0x{:08x}, {} ):0 shutdown received", Show(s),
                 offset, length);
  }
  else
  {
    DEBUG_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: recv( {}, 0x{:08x}, {} ):{} {}", Show(s), offset,
                  length, result, "");
  }
  ReplyWithByte8(static_cast<u32>(result));
}

void CommandSend(std::size_t p)
{
  const u32 guest = Word(p);
  const u32 offset = Word(p + 1);
  u32 length = Word(p + 2);
  const HostSocket s = HostFor(guest);

  const std::span<u8> buffer = BoardSpan(offset);
  if (buffer.size() < length)
  {
    ERROR_LOG_FMT(AMMEDIABOARD_NET, "AMMBCommandSend: Bad data offset or length: off={:08x} len={}",
                  offset, length);
    length = 0;
  }

  const auto result = send(s, reinterpret_cast<const char*>(buffer.data()),
                           static_cast<IoLength>(length), Common::SEND_FLAGS);
  if (result < 0)
  {
    // A non-blocking socket with nothing to do yet is normal and not logged.
    const int error = SocketError();
    if (error != WOULD_BLOCK)
    {
      ERROR_LOG_FMT(AMMEDIABOARD_NET,
                    "GC-AM: send( {}({}), 0x{:08x}, {} ) failed with error {}: {}", Show(s), guest,
                    offset, length, error, Common::StrNetworkError());
    }
  }
  else
  {
    DEBUG_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: send( {}({}), 0x{:08x}, {} ): {} {}", Show(s), guest,
                  offset, length, result, "");
  }
  ReplyWithByte8(static_cast<u32>(result));
}

void CommandSelect(std::size_t p)
{
  const u32 count = std::min<u32>(Word(p), SOCKET_FD_MAX);
  const std::array<u32, 3> set_offsets{Word(p + 1), Word(p + 2), Word(p + 3)};
  const u32 timeout_offset = Word(p + 4);
  static constexpr std::array<short, 3> set_events{POLLIN, POLLOUT, POLLPRI};

  int timeout_ms = -1;
  if (timeout_offset != 0)
  {
    const std::span<u8> timeout = BoardSpan(timeout_offset);
    if (timeout.size() >= 16)
    {
      u64 seconds;
      u32 microseconds;
      std::memcpy(&seconds, timeout.data(), sizeof(seconds));
      std::memcpy(&microseconds, timeout.data() + 8, sizeof(microseconds));
      timeout_ms = static_cast<int>(seconds * 1000 + microseconds / 1000);
    }
    else
    {
      ERROR_LOG_FMT(AMMEDIABOARD_NET, "AMMBCommandSelect: Bad timeout offset: {:08x}",
                    timeout_offset);
    }
  }
  if (timeout_ms < 0)
    WARN_LOG_FMT(AMMEDIABOARD, "AMMBCommandSelect: Infinite timout!");

  DEBUG_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: select( {}, 0x{:08x} 0x{:08x} 0x{:08x} 0x{:08x} ) timeout={}",
                count, set_offsets[0], set_offsets[1], set_offsets[2], timeout_offset, timeout_ms);

  // The guest's fd sets: 256 bits, LSB first.
  std::array<std::span<u8>, 3> sets{};
  for (std::size_t k = 0; k != sets.size(); ++k)
  {
    if (set_offsets[k] == 0)
      continue;
    const std::span<u8> bytes = BoardSpan(set_offsets[k]);
    if (bytes.size() < 32)
    {
      ERROR_LOG_FMT(AMMEDIABOARD_NET, "Bad FDSET offset: {:08x}", set_offsets[k]);
      continue;
    }
    sets[k] = bytes.first(32);
  }

  std::vector<PollEntry> entries(count);
  for (PollEntry& entry : entries)
  {
    entry.fd = NO_SOCKET;
    entry.events = 0;
  }
  for (std::size_t k = 0; k != sets.size(); ++k)
  {
    if (sets[k].empty())
      continue;
    for (u32 index = 0; index != count; ++index)
    {
      if ((sets[k][index / 8] >> (index % 8)) & 1)
      {
        entries[index].fd = HostFor(index);
        entries[index].events |= set_events[k];
      }
    }
  }
  std::erase_if(entries, [](const PollEntry& entry) { return entry.fd == NO_SOCKET; });

  DEBUG_LOG_FMT(AMMEDIABOARD, "AMMBCommandSelect: Polling with socket count: {}", entries.size());
  const int result = PollSockets(entries.data(), entries.size(), timeout_ms);
  DEBUG_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: select result: {}", result);

  if (result >= 0)
  {
    for (std::size_t k = 0; k != sets.size(); ++k)
    {
      if (sets[k].empty())
        continue;
      std::array<u8, 32> fresh{};
      for (const PollEntry& entry : entries)
      {
        if ((entry.revents & set_events[k]) == 0)
          continue;
        const s32 guest = GuestFor(entry.fd);
        if (guest < 0)
          continue;
        fresh[static_cast<std::size_t>(guest) / 8] |= static_cast<u8>(1u << (guest % 8));
      }
      std::ranges::copy(fresh, sets[k].begin());
    }
  }
  else
  {
    ERROR_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: select failed: {} ({})", result,
                  Common::StrNetworkError());
  }

  s_media[1] = 0;
  SetWord(1, static_cast<u32>(result));
}

void CommandSetSockOpt(std::size_t p)
{
  const u32 guest = Word(p);
  const u32 level = Word(p + 1);
  const u32 option = Word(p + 2);
  const u32 value_offset = Word(p + 3);
  const u32 value_length = Word(p + 4);
  const HostSocket s = HostFor(guest);

  const std::span<u8> value = BoardSpan(value_offset);
  if (value.size() < value_length)
  {
    ERROR_LOG_FMT(AMMEDIABOARD_NET,
                  "AMMBCommandSetSockOpt: Bad optval offset or length: off={:08x} len={}",
                  value_offset, value_length);
    return;
  }

  const int result = setsockopt(s, static_cast<int>(level), static_cast<int>(option),
                                reinterpret_cast<const char*>(value.data()),
                                static_cast<SockLen>(value_length));
  NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: setsockopt( {:d}, {:04x}, {}, {:p}, {} ):{:d} ({})",
                 Show(s), level, option, static_cast<const void*>(value.data()), value_length,
                 result, SocketError());
  ReplyWithByte8(static_cast<u32>(result));
}

void CommandSetTimeOuts()
{
  const u32 guest = Word(2);
  const u32 connect_timeout = Word(3);
  const u32 send_timeout = Word(4);
  const u32 receive_timeout = Word(5);
  s_timeouts = {connect_timeout, send_timeout, receive_timeout};

  const HostSocket s = HostFor(guest);
  int result;
  if (s != NO_SOCKET)
  {
    // Milliseconds as a plain int, which is what Winsock takes.
    result = setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send_timeout),
                        sizeof(int));
    if (result < 0)
    {
      result = SocketError();
    }
    else
    {
      result = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                          reinterpret_cast<const char*>(&receive_timeout), sizeof(int));
      if (result < 0)
        result = SocketError();
    }
    INFO_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: SetTimeOuts( {}({}), {}, {}, {} ):{}", Show(s), guest,
                 connect_timeout, send_timeout, receive_timeout, result);
  }
  else
  {
    result = SOCKET_FAILED;
    ERROR_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: Invalid Socket: SetTimeOuts( {}({}), {}, {}, {} ):{}",
                  Show(s), guest, connect_timeout, send_timeout, receive_timeout, result);
  }
  ReplyWithByte8(static_cast<u32>(result));
}

void CommandGetLastError()
{
  const u32 guest = Word(2);
  const HostSocket s = HostFor(guest);
  if (s_last_error == STATUS_OK)
  {
    DEBUG_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: GetLastError( {}({}) ):EWOULDBLOCK", Show(s), guest);
  }
  else
  {
    NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: GetLastError( {}({}) ):{}", Show(s), guest,
                   static_cast<s32>(s_last_error));
  }
  ReplyWithByte8(s_last_error);
}

void CommandInetAddr()
{
  const char* text = reinterpret_cast<const char*>(s_net_commands.data());
  const std::size_t length = strnlen(text, 15);
  const std::string address_text(text, length);
  const u32 address = inet_addr(address_text.c_str());
  NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: InetAddr( {} )", address_text);
  ReplyWithByte8(address);
}

void CommandModifyMyIPaddr(std::size_t p)
{
  const std::string text = BoardString(Word(p), 15);
  NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: modifyMyIPaddr({})", text);
  if (const auto range = Common::StringToIPv4PortRange(text))
    s_game_ip = range->first.ip_address;
}

// Socket commands shared by Execute2 and DI Execute. Returns false for others.
bool RunSocketCommand(u16 command, std::size_t p)
{
  switch (static_cast<AMMBCommand>(command))
  {
  case AMMBCommand::Closesocket:
    CommandCloseSocket(p);
    return true;
  case AMMBCommand::Connect:
    CommandConnect(p);
    return true;
  case AMMBCommand::Recv:
    CommandRecv(p);
    return true;
  case AMMBCommand::Send:
    CommandSend(p);
    return true;
  case AMMBCommand::Socket:
    CommandSocket(p);
    return true;
  case AMMBCommand::Select:
    CommandSelect(p);
    return true;
  case AMMBCommand::SetSockOpt:
    CommandSetSockOpt(p);
    return true;
  case AMMBCommand::ModifyMyIPaddr:
    CommandModifyMyIPaddr(p);
    return true;
  default:
    return false;
  }
}

// ================================================================================================
// Board commands
// ================================================================================================

void PutSerial()
{
  std::ranges::copy(BOARD_SERIAL, s_media.begin() + 4);
}

void SetSegaBootVersion(u8 minor)
{
  s_media[4] = 0x03;
  s_media[5] = minor;
  s_media[6] = 1;
  SetWord(2, 1);
  SetWord(4, 0xFF);
}

void SetSystemFlags()
{
  s_media[4] = 1;
  s_media[5] = 1;
  s_media[6] = 1;
  s_media[8] = 0;
}

// Write of 0x20 bytes to DIMMCommandExecute1 starting with 1.
void Execute1()
{
  const u16 command = Half(0x22);
  DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: Execute command: (1):{0:04X}", command);
  ClearMedia(0x00, 0x20);
  SetWord(0, Word(8) | 0x80000000);

  switch (command)
  {
  case 0x000:
    SetWord(1, 1);
    break;
  case 0x001:
    SetWord(1, 0x20000000);
    break;
  case 0x100:
    SetWord(1, 5);
    SetWord(2, 100);
    break;
  case 0x101:
    SetSegaBootVersion(0x09);
    break;
  case 0x102:
    SetSystemFlags();
    break;
  case 0x103:
    PutSerial();
    break;
  case 0x104:
    s_media[4] = 1;
    break;
  case 0x301:
  {
    const u32 test_type = Word(9);
    const u32 text_address = Word(10);
    DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: TestHardware (Execute1 inner): type={:08x} str_ptr={:08x}",
                  test_type, text_address);
    if (text_address != 0)
      WriteTestOK(text_address);
    SetWord(1, test_type);
    ScheduleTestPhase2(0);
    break;
  }
  default:
    if ((command & 0x8000) != 0)
    {
      DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: Cleanup command {:04x} (Execute1)", command);
    }
    else
    {
      ERROR_LOG_FMT(AMMEDIABOARD, "Unhandled Media Board Command:{0:04x}", command);
      PanicAlertFmtT("Unhandled Media Board Command:{0:04x}", command);
    }
    break;
  }

  ClearMedia(0x20, 0x40);
  SaveReply(s_exec1_reply);
  ExpansionInterface::GenerateInterrupt(0x04);
}

// Read of DIMMCommandExecute2.
void Execute2(u32 address, u32 length)
{
  const u16 command = Half(0x202);
  DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: Execute command: (2){0:04X}", command);
  std::copy(s_media.begin() + 0x200, s_media.begin() + 0x220, s_media.begin());
  ClearMedia(0x200, 0x220);
  s_media[0x204] = 1;

  if (!RunSocketCommand(command, 2))
  {
    switch (static_cast<AMMBCommand>(command))
    {
    case AMMBCommand::Unknown_001:
      SetWord(1, 1);
      break;
    case AMMBCommand::GetNetworkFirmVersion:
      SetWord(1, 0x1305);
      s_media[6] = 1;
      break;
    case AMMBCommand::GetSystemFlags:
      s_media[4] = 1;
      s_media[6] = 2;
      s_media[7] = 1;
      break;
    case AMMBCommand::Unknown_103:
    case AMMBCommand::NetworkReInit:
      break;
    case AMMBCommand::GetNetworkConfig:
      s_media[4] = 1;
      s_netconfig_pending = true;
      break;
    case AMMBCommand::TestHardware:
    {
      const u32 test_type = Word(1);
      const u32 text_address = Word(2);
      DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: TestHardware (Execute2): type={:08x} str_ptr={:08x}",
                    test_type, text_address);
      if (text_address != 0)
        WriteTestOK(text_address);
      SetWord(1, test_type);
      ScheduleTestPhase2(1);
      break;
    }
    case AMMBCommand::Accept:
      CommandAccept(2);
      break;
    case AMMBCommand::Bind:
      CommandBind();
      break;
    case AMMBCommand::InetAddr:
      CommandInetAddr();
      break;
    case AMMBCommand::Listen:
      CommandListen();
      break;
    case AMMBCommand::SetTimeOuts:
      CommandSetTimeOuts();
      break;
    case AMMBCommand::GetParambyDHCPExec:
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: GetParambyDHCPExec({})", Word(2));
      s_media[1] = 0;
      SetWord(1, 0);
      break;
    case AMMBCommand::GetLastError:
      CommandGetLastError();
      break;
    case AMMBCommand::InitLink:
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: InitLink");
      break;
    case AMMBCommand::AllNetInit:
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: AllNetInit");
      break;
    default:
      if ((command & 0x8000) != 0)
      {
        DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: Cleanup command {:04x} (Execute2)", command);
      }
      else
      {
        ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Command:{0:04x}", command);
        ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Command Unhandled!");
      }
      break;
    }
  }

  s_media[3] |= 0x80;
  SaveReply(s_exec2_reply);
  Mem().Memset(address, 0, length);
  ExpansionInterface::GenerateInterrupt(0x10);
}

// DI opcode 0xAB with no offset and length.
void ExecuteDI(u16 command)
{
  ClearMedia(0x00, 0x20);
  SetWord(0, Word(8) | 0x80000000);

  if (!RunSocketCommand(command, 10))
  {
    switch (command)
    {
    case 0x001:
      SetWord(1, 0x20000000);
      break;
    case 0x100:
      SetWord(1, s_board_status);
      SetWord(2, s_load_progress);
      if (s_load_progress < 100)
        ++s_load_progress;
      else
        s_board_status = LoadedGameProgram;
      break;
    case 0x101:
      SetSegaBootVersion(0x11);
      break;
    case 0x102:
      SetSystemFlags();
      break;
    case 0x103:
      PutSerial();
      break;
    case 0x104:
      s_media[4] = 1;
      break;
    case 0x204:
      break;
    case 0x301:
      DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: TestHardware: ({:08x})", Word(9));
      DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM:               ({:08x})", Word(12));
      WriteTestOK(Word(12));
      SetWord(1, Word(9));
      break;
    case 0x601:
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: InitLink");
      break;
    case 0x605:
      NOTICE_LOG_FMT(AMMEDIABOARD, "GC-AM: 0x605");
      break;
    case 0x606:
    {
      const u16 size = Half(0x24);
      const u16 port = static_cast<u16>((s_media[0x27] << 8) | s_media[0x26]);
      Common::IPAddress first_ip;
      Common::IPAddress second_ip;
      std::memcpy(first_ip.data(), &s_media[0x30], first_ip.size());
      std::memcpy(second_ip.data(), &s_media[0x34], second_ip.size());
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: SetupLink:");
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:  Size: ({}) ", size);
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:  Port: ({})", port);
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:LinkNum:({:02x})", s_media[0x28]);
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:        ({:02x})", s_media[0x2A]);
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:        ({:04x})",
                     static_cast<u16>((s_media[0x2C] << 8) | s_media[0x2D]));
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:   IP:  ({})", IPText(first_ip));
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:   IP:  ({})", IPText(second_ip));
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:        ({:08x})", Common::swap32(Word(14)));
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:        ({:08x})", Common::swap32(Word(15)));
      SetWord(1, 0);
      break;
    }
    case 0x607:
    {
      const u16 offset = Half(0x26);
      const u32 base = Word(10);
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: SearchDevices: ({})",
                     static_cast<u16>((s_media[0x24] << 8) | s_media[0x25]));
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:        Offset: ({:04x})", offset);
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM:                ({:08x})", base);
      const std::span<u8> devices = BoardSpan(offset + base);
      if (devices.size() < 0x20)
      {
        ERROR_LOG_FMT(AMMEDIABOARD_NET, "SearchDevices: Bad data offset: {:08x}", offset + base);
        break;
      }
      std::array<u32, 8> words;
      std::memcpy(words.data(), devices.data(), sizeof(words));
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: {:08x} {:08x} {:08x} {:08x}", words[0], words[1],
                     words[2], words[3]);
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: {:08x} {:08x} {:08x} {:08x}", words[4], words[5],
                     words[6], words[7]);
      SetWord(1, 0);
      break;
    }
    case 0x608:
    {
      u16 port_raw;
      u16 flag;
      std::memcpy(&port_raw, &s_media[6], sizeof(port_raw));
      std::memcpy(&flag, &s_media[10], sizeof(flag));
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: 0x608( {} {} {} )", Word(10),
                     Common::swap16(port_raw), flag);
      break;
    }
    case 0x614:
      NOTICE_LOG_FMT(AMMEDIABOARD_NET, "GC-AM: 0x614");
      break;
    default:
      ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Execute buffer UNKNOWN:{0:04x}", command);
      break;
    }
  }

  ClearMedia(0x20, 0x40);
  SaveReply(s_exec1_reply);
}

// ================================================================================================
// DI commands
// ================================================================================================

void UnhandledRead(u32 offset, u32 length)
{
  ERROR_LOG_FMT(AMMEDIABOARD, "Unhandled Media Board Read: offset={0:08x} length={0:08x}", offset,
                length);
  PanicAlertFmtT("Unhandled Media Board Read: offset={0:08x} length={0:08x}", offset, length);
}

u32 DIRead(u32 offset, u32 address, u32 length)
{
  auto& memory = Mem();

  if ((offset & 0x8FFF0000) == 0x80000000)
  {
    switch (offset)
    {
    case MediaBoardStatus1:
      memory.Write_U16(1, address);
      break;
    case MediaBoardStatus2:
      memory.Memset(address, 0x00, length);
      break;
    case MediaBoardStatus3:
      memory.Memset(address, 0xFF, length);
      memory.Write_U32_Swap(0x20000000, address);
      memory.Write_U32(0x4743414D, address + 4);  // "GCAM"
      break;
    default:
    {
      // Status words that are a single value; `swapped` ones are stored little-endian.
      struct StatusWord
      {
        u32 offset;
        u32 value;
        bool swapped;
      };
      static constexpr std::array<StatusWord, 6> words{{
          {0x80000100, 0x001F1F1F, true},
          {FirmwareStatus1, 0x000001FA, true},
          {FirmwareStatus2, 1, true},
          {0x80000160, 0x00001E00, false},
          {0x80000180, 0, false},
          {0x800001A0, 0xFFFFFFFF, false},
      }};
      const auto word = std::ranges::find(words, offset, &StatusWord::offset);
      if (word == words.end())
        UnhandledRead(offset, length);
      else if (word->swapped)
        memory.Write_U32_Swap(word->value, address);
      else
        memory.Write_U32(word->value, address);
      break;
    }
    }
    return 0;
  }

  if (offset == 0 && length == 0x80)
  {
    FileReadData(s_netcfg, 0, address, length);
    return 0;
  }
  if (offset == DIMMExtraSettings && length == 0x20)
  {
    FileReadData(s_extra, 0, address, length);
    return 0;
  }
  if (offset >= DIMMMemory && offset < 0x1F800000)
  {
    FileReadData(s_dimm, offset - DIMMMemory, address, length);
    return 0;
  }
  if (offset >= DIMMMemory2 && offset < 0xFF800000)
  {
    FileReadData(s_dimm, offset - DIMMMemory2, address, length);
    return 0;
  }
  if (offset == NetworkControl && length == 0x20)
  {
    FileReadData(s_netctrl, 0, address, length);
    return 0;
  }
  if (offset >= AllNetBuffer && offset < AllNetBuffer + 0x1000)
  {
    INFO_LOG_FMT(AMMEDIABOARD, "GC-AM: Read All.Net Buffer ({:08x},{})", offset, length);
    // The whole reply, whatever the DI length.
    SafeCopyToEmu(address, reinterpret_cast<const u8*>(ALLNET_REPLY), sizeof(ALLNET_REPLY),
                  offset - AllNetBuffer, sizeof(ALLNET_REPLY));
    return 0;
  }
  if (s_netconfig_pending && length == 0x80 && !BoardSpan(offset).empty())
  {
    s_netconfig_pending = false;
    std::array<u8, 0x80> config{};
    if (s_netcfg.IsOpen())
    {
      s_netcfg.Seek(0, File::SeekOrigin::Begin);
      s_netcfg.ReadBytes(config.data(), config.size());
    }
    if (config[0] == 0)
      config[0] = 2;
    DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: NetConfig Read (intercepted) offset={:08x} config[0]={}",
                  offset, config[0]);
    memory.CopyToEmu(address, config.data(), config.size());
    return 0;
  }
  if (offset == DIMMCommandVersion2 && length == 0x20)
  {
    DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: Read Execute1 response (saved)");
    memory.CopyToEmu(address, s_exec1_reply.data(), sizeof(s_exec1_reply));
    return 0;
  }
  if (offset == DIMMCommandVersion2_2 && length == 0x20)
  {
    DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: Read Execute2 response (saved)");
    memory.CopyToEmu(address, s_exec2_reply.data(), sizeof(s_exec2_reply));
    return 0;
  }
  if (const std::span<u8> board = BoardSpan(offset); !board.empty())
  {
    DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: Read MediaBoard ({:08x},{:08x})", offset, length);
    SafeCopyToEmu(address, board.data(), board.size(), 0, length);
    return 0;
  }
  if (offset == DIMMCommandExecute2)
  {
    Execute2(address, length);
    return 0;
  }
  if (offset >= 0x57058000)
  {
    UnhandledRead(offset, length);
    return 0;
  }
  if (s_firmware_map)
  {
    if (!SafeCopyToEmu(address, s_firmware.data(), s_firmware.size(), offset, length))
    {
      ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Invalid firmware buffer range: offset={}, length={}",
                    offset, length);
    }
    return 0;
  }

  // The game itself.
  const std::span<u8> target = memory.GetSpanForAddress(address);
  if (target.size() < length)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Invalid DIMM Disc read from: offset={}, length={}", offset,
                  length);
    return 1;
  }
  if (!s_disc || !s_disc->Read(offset, length, target.data()))
    return 1;
  return 0;
}

u32 DIWrite(u32 offset, u32 address, u32 length)
{
  auto& memory = Mem();

  if ((offset == FirmwareMagicWrite1 || offset == FirmwareMagicWrite2) && length == 0x20)
  {
    s_firmware_map = true;
    return 0;
  }
  if (s_firmware_map && offset >= 0x00400000 && offset <= 0x00600000)
  {
    if (!SafeCopyFromEmu(s_firmware.data(), address, s_firmware.size(), offset - 0x00400000,
                         length))
    {
      ERROR_LOG_FMT(AMMEDIABOARD, "GC-AM: Invalid firmware write: offset={}, length={}",
                    offset - 0x00400000, length);
    }
    return 0;
  }
  if (offset == 0 && length == 0x80)
  {
    FileWriteData(s_netcfg, 0, address, length);
    return 0;
  }
  if (offset == DIMMExtraSettings && length == 0x20)
  {
    FileWriteData(s_extra, 0, address, length);
    return 0;
  }
  if (offset >= BackupMemory && offset < 0x00800000)
  {
    // Always the start of the file.
    FileWriteData(s_backup, 0, address, length);
    return 0;
  }
  if (offset >= DIMMMemory && offset < 0x1F800000)
  {
    FileWriteData(s_dimm, offset - DIMMMemory, address, length);
    return 0;
  }
  if (offset >= FirmwareAddress && offset < 0x84818000)
  {
    INFO_LOG_FMT(AMMEDIABOARD, "GC-AM: Write Firmware ({:08x})", offset);
    return 0;
  }
  if (offset >= DIMMMemory2 && offset < 0xFF800000)
  {
    FileWriteData(s_dimm, offset - DIMMMemory2, address, length);
    return 0;
  }
  if (offset == NetworkControl && length == 0x20)
  {
    FileWriteData(s_netctrl, 0, address, length);
    return 0;
  }
  if (offset == DIMMCommandExecute1 && length == 0x20 && memory.Read_U8(address) == 1)
  {
    Execute1();
    return 0;
  }
  if (const std::span<u8> board = BoardSpan(offset); !board.empty())
  {
    DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: Write MediaBoard ({:08x},{:08x})", offset, length);
    if (length == 0x80 && memory.Read_U8(address) != 0 && s_netcfg.IsOpen())
    {
      DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: NetConfig persist to trinetcfg.bin (status={})",
                    memory.Read_U8(address));
      FileWriteData(s_netcfg, 0, address, 0x80);
    }
    SafeCopyFromEmu(board.data(), address, board.size(), 0, length);
    return 0;
  }
  if (offset >= 0x57058000)
  {
    ERROR_LOG_FMT(AMMEDIABOARD, "Unhandled Media Board Write: offset={0:08x} length={0:08x}",
                  offset, length);
    PanicAlertFmtT("Unhandled Media Board Write: offset={0:08x} length={0:08x}", offset, length);
  }
  return 0;
}

void OpenBoardFile(File::IOFile& file, const std::string& path)
{
  if (!file.Open(path, "rb+"))
    file.Open(path, "wb+");
}
}  // namespace

// ================================================================================================
// Public interface
// ================================================================================================

MediaBoardRange::MediaBoardRange(u32 start_, u32 size_, std::span<u8> buffer_)
    : start{start_}, end{start_ + static_cast<u32>(std::min<std::size_t>(size_, buffer_.size()))},
      buffer{buffer_.data()}, buffer_size{buffer_.size()}
{
  if (size_ > buffer_.size())
  {
    WARN_LOG_FMT(AMMEDIABOARD,
                 "Invalid MediaBoardRange: start=0x{:08x}, size=0x{:06x}, buffer_size=0x{:06x}",
                 start_, size_, buffer_.size());
  }
}

void Init()
{
  s_media.fill(0);
  s_exec1_reply.fill(0);
  s_exec2_reply.fill(0);
  s_net_commands.fill(0);
  s_net_buffer.fill(0);
  s_allnet_buffer.fill(0);
  s_allnet_settings.fill(0);
  s_firmware.fill(0xFF);
  s_sockets.fill(NO_SOCKET);

  s_game_ip = {};
  s_netconfig_pending = false;
  s_board_status = LoadingGameProgram;
  s_load_progress = 80;
  s_firmware_map = false;
  s_test_menu = false;
  s_last_error = STATUS_OK;
  s_next_fd = 1;
  s_key_a = 0;
  s_key_b = 0;
  s_key_c = 0;

  s_test_event = Core::System::GetInstance().GetCoreTiming().RegisterEvent(
      "AMMediaboardTestHwPhase2", TestHardwarePhase2);

  const std::string& folder = File::GetUserPath(D_TRIUSER_IDX);
  const std::string game_id = SConfig::GetInstance().GetGameID();

  struct BoardFile
  {
    File::IOFile& file;
    std::string name;
    const char* reported_name;
  };
  const std::array<BoardFile, 5> files{{
      {s_netcfg, "trinetcfg.bin", "trinetcfg.bin"},
      {s_netctrl, "trinetctrl.bin", "trinetctrl.bin"},
      {s_extra, "triextra.bin", "triextra.bin"},
      {s_dimm, fmt::format("tridimm_{}.bin", game_id), "tridimm.bin"},
      {s_backup, fmt::format("backup_{}.bin", game_id), "backup.bin"},
  }};
  for (const BoardFile& entry : files)
    OpenBoardFile(entry.file, folder + entry.name);
  for (const BoardFile& entry : files)
  {
    if (!entry.file.IsOpen())
      PanicAlertFmt("Failed to open/create: {}", folder + entry.reported_name);
  }

  const std::string segaboot = folder + "segaboot.gcm";
  if (!File::Exists(segaboot))
  {
    PanicAlertFmt("Failed to open segaboot.gcm({}), which is required for test menus.", segaboot);
    return;
  }
  File::IOFile firmware(segaboot, "rb+");
  if (!firmware.IsOpen())
  {
    PanicAlertFmt("Failed to read: {}", segaboot);
    return;
  }
  const std::size_t size =
      static_cast<std::size_t>(std::min<u64>(firmware.GetSize(), s_firmware.size()));
  firmware.ReadBytes(s_firmware.data(), size);
  s_test_menu = true;
}

void FirmwareMap(bool on)
{
  s_firmware_map = on;
}

void InitKeys(u32 key_a, u32 key_b, u32 key_c)
{
  s_key_a = key_a;
  s_key_b = key_b;
  s_key_c = key_c;
}

void InitDIMM(const DiscIO::Volume& volume)
{
  s_disc = DiscIO::CreateCachedBlobReader(volume.GetBlobReader().CopyReader());
}

bool GetTestMenu()
{
  return s_test_menu;
}

u32 GetGameType()
{
  const std::string game_id = SConfig::GetInstance().GetGameID();
  u32 id = 0x454A;
  if (game_id.size() == 6)
    id = static_cast<u32>((static_cast<int>(game_id[1]) << 8) | static_cast<int>(game_id[2]));

  switch (id)
  {
  case 0x4747:
    return FZeroAX;
  case 0x4841:
    return FZeroAXMonster;
  case 0x4B50:
  case 0x4B5A:
    return MarioKartGP;
  case 0x4E4A:
  case 0x4E4C:
    return MarioKartGP2;
  case 0x454A:
  case 0x4559:
  case 0x5342:
  case 0x3132:
  case 0x454C:
    return VirtuaStriker3;
  case 0x484A:
  case 0x484E:
  case 0x485A:
  case 0x4A41:
  case 0x4A4A:
    return VirtuaStriker4;
  case 0x4C4A:
  case 0x4C4B:
  case 0x4C4C:
    return VirtuaStriker4_2006;
  case 0x4658:
  case 0x4A4E:
    return KeyOfAvalon;
  case 0x4758:
    return GekitouProYakyuu;
  case 0x3030:
    return FirmwareUpdate;
  default:
    PanicAlertFmtT("Unknown game ID:{0:08x}, using default controls.", id);
    return VirtuaStriker3;
  }
}

u32 GetMediaType()
{
  switch (GetGameType())
  {
  case MarioKartGP:
  case MarioKartGP2:
  case FZeroAXMonster:
    return NAND;
  default:
    return GDROM;
  }
}

u32 ExecuteCommand(std::array<u32, 3>& dicmd_buf, u32* diimm_buf, u32 address, u32 length)
{
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();

  // The words arrive XORed with the keys; every command then advances the keys.
  dicmd_buf[0] ^= s_key_a;
  dicmd_buf[1] ^= s_key_b;
  dicmd_buf[2] ^= s_key_c;
  const u32 seed = dicmd_buf[0] >> 16;
  s_key_a *= seed;
  s_key_b *= seed;
  s_key_c *= seed;

  // Until the keys are known, take them from where the IPL leaves them.
  if (s_key_a == 0 && memory.Read_U32(0) != 0)
  {
    HLE::Patch(system, 0x813048B8, "OSReport");
    HLE::Patch(system, 0x8130095C, "OSReport");
    InitKeys(memory.Read_U32(0), memory.Read_U32(4), memory.Read_U32(8));
  }

  const u32 command = dicmd_buf[0] << 24;
  const u32 offset = dicmd_buf[1] << 2;
  DEBUG_LOG_FMT(AMMEDIABOARD, "GC-AM: {:08x} {:08x} DMA=addr:{:08x},len:{:08x} Keys: {:08x} {:08x} {:08x}",
                command, offset, address, length, s_key_a, s_key_b, s_key_c);

  // Booting into the test menu maps the firmware over the disc.
  if (offset == 0x2440 && memory.Read_U32(0x811FFF00) == 1 &&
      memory.Read_U32(0x8006BF70) != 0x0A536567)
  {
    s_firmware_map = true;
  }

  switch (static_cast<AMMBDICommand>(command >> 24))
  {
  case AMMBDICommand::Inquiry:
  {
    s_firmware_map = false;
    switch (GetGameType())
    {
    case VirtuaStriker4_2006:
    case KeyOfAvalon:
    case MarioKartGP:
    case MarioKartGP2:
    case FirmwareUpdate:
      *diimm_buf = Version2;
      break;
    default:
      *diimm_buf = Version1;
      break;
    }
    return 0;
  }
  case AMMBDICommand::Read:
    return DIRead(offset, address, length);
  case AMMBDICommand::Write:
    return DIWrite(offset, address, length);
  case AMMBDICommand::Execute:
  {
    const u16 board_command = static_cast<u16>(Word(8) >> 16);
    if (board_command == 0)
      return 0;
    if (offset == 0 && length == 0)
    {
      ExecuteDI(board_command);
      return 0;
    }
    ERROR_LOG_FMT(AMMEDIABOARD, "Unhandled Media Board Execute:{0:04x}", board_command);
    PanicAlertFmtT("Unhandled Media Board Execute:{0:04x}", board_command);
    return 0;
  }
  default:
    ERROR_LOG_FMT(AMMEDIABOARD, "Unhandled Media Board Command:{0:02x}", command);
    PanicAlertFmtT("Unhandled Media Board Command:{0:02x}", command);
    return 0;
  }
}

void Shutdown()
{
  s_netcfg.Close();
  s_netctrl.Close();
  s_extra.Close();
  s_dimm.Close();
  s_backup.Close();
  s_disc.reset();
  CloseAllSockets();
}

void DoState(PointerWrap& p)
{
  p.Do(s_firmware_map);
  p.Do(s_test_menu);
  p.DoArray(s_timeouts);
  p.Do(s_last_error);
  p.Do(s_key_a);
  p.Do(s_key_b);
  p.Do(s_key_c);
  p.DoArray(s_firmware);
  p.DoArray(s_media);
  p.DoArray(s_exec1_reply);
  p.DoArray(s_exec2_reply);
  p.DoArray(s_net_commands);
  p.DoArray(s_net_buffer);
  p.DoArray(s_allnet_buffer);
  p.DoArray(s_allnet_settings);
  p.Do(s_next_fd);
  p.Do(s_game_ip);
  p.Do(s_netconfig_pending);
  p.Do(s_board_status);
  p.Do(s_load_progress);

  // Which guest sockets are open. The host sockets cannot be saved; on load
  // fresh (unconnected) ones take their place.
  std::array<u8, 32> open{};
  if (!p.IsMeasureMode())
  {
    for (std::size_t i = 1; i != SOCKET_FD_MAX; ++i)
    {
      if (s_sockets[i] != NO_SOCKET)
        open[i / 8] |= static_cast<u8>(1u << (i % 8));
    }
  }
  p.DoArray(open);

  if (p.IsReadMode())
  {
    CloseAllSockets();
    for (std::size_t i = 1; i != SOCKET_FD_MAX; ++i)
    {
      if ((open[i / 8] >> (i % 8)) & 1)
        s_sockets[i] = socket(AF_INET, SOCK_STREAM, 0);
    }
  }
}

std::optional<ParsedIPRedirection> ParseIPRedirection(std::string_view str)
{
  const std::size_t space = str.find(' ');
  const std::string_view pair = str.substr(0, space);
  const std::string_view description =
      space == std::string_view::npos ? std::string_view{} : str.substr(space + 1);

  const std::size_t equals = pair.find('=');
  if (equals == std::string_view::npos || pair.find('=', equals + 1) != std::string_view::npos)
    return std::nullopt;

  return ParsedIPRedirection{pair.substr(0, equals), pair.substr(equals + 1), description};
}

IPRedirections GetIPRedirections()
{
  IPRedirections result;
  const std::string setting = Config::Get(Config::MAIN_TRIFORCE_IP_REDIRECTIONS);
  for (const std::string& piece : SplitString(setting, ','))
  {
    const auto parsed = ParseIPRedirection(piece);
    if (!parsed)
      continue;
    const auto original = Common::StringToIPv4PortRange(parsed->original);
    const auto replacement = Common::StringToIPv4PortRange(parsed->replacement);
    if (!original || !replacement)
    {
      ERROR_LOG_FMT(AMMEDIABOARD, "Bad IP redirection string: {}", piece);
      continue;
    }
    result.push_back({*original, *replacement});
  }
  return result;
}

namespace
{
// Maps `subject` from the range `from` onto the range `to`.
Common::IPv4Port MapAddress(const Common::IPv4PortRange& from, const Common::IPv4PortRange& to,
                            Common::IPv4Port subject)
{
  const u32 to_first_ip = to.first.GetIPAddressValue();
  const u64 ip_count = u64{1} + to.last.GetIPAddressValue() - to_first_ip;
  const u32 ip_step = subject.GetIPAddressValue() - from.first.GetIPAddressValue();
  const u32 ip = static_cast<u32>(u64{to_first_ip} + ip_step % ip_count);

  Common::IPv4Port result = subject;
  result.ip_address = {static_cast<u8>(ip >> 24), static_cast<u8>(ip >> 16),
                       static_cast<u8>(ip >> 8), static_cast<u8>(ip)};

  const u32 port_count = 1u + to.last.GetPortValue() - to.first.GetPortValue();
  if (port_count != 0x10000)
  {
    const int port_step = int{subject.GetPortValue()} - int{from.first.GetPortValue()};
    const u16 port =
        static_cast<u16>(to.first.GetPortValue() + static_cast<u32>(port_step) % port_count);
    result.port = htons(port);
  }
  return result;
}
}  // namespace

Common::IPv4Port IPRedirection::Apply(Common::IPv4Port subject) const
{
  return MapAddress(original, replacement, subject);
}

Common::IPv4Port IPRedirection::Reverse(Common::IPv4Port subject) const
{
  return MapAddress(replacement, original, subject);
}

std::string IPRedirection::ToString() const
{
  return fmt::format("{}={}", original.ToString(), replacement.ToString());
}

s32 DebuggerGetSocket(u32 triforce_fd)
{
  if (triforce_fd < SOCKET_FD_MAX)
    return static_cast<s32>(s_sockets[triforce_fd]);
  WARN_LOG_FMT(AMMEDIABOARD, "GC-AM: Bad socket fd used by the debugger: {}", triforce_fd);
  return -1;
}

}  // namespace AMMediaboard
