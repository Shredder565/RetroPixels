// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/ChunkFile.h"
#include "Common/CommonTypes.h"
#include "Common/Network.h"

#include "DiscIO/Volume.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

// The Triforce AM media board (DIMM / GD-ROM / NAND board). On a Triforce the
// DVD interface talks to this board instead of a drive: it serves the game
// image, the board's own status and command buffers, the SegaBoot firmware and
// a small socket API that the games use for networking.
namespace AMMediaboard
{

enum GameType
{
  FZeroAX = 1,
  FZeroAXMonster,
  MarioKartGP,
  MarioKartGP2,
  VirtuaStriker3,
  VirtuaStriker4,
  VirtuaStriker4_2006,
  GekitouProYakyuu,
  KeyOfAvalon,
  FirmwareUpdate,
};

enum MediaType
{
  GDROM = 1,
  NAND,
};

enum MediaBoardType
{
  NANDMaskBoardHDD = 0,
  NANDMaskBoardMask = 1,
  NANDMaskBoardNAND = 2,
  DIMMBoardType3 = 4,
};

enum MediaBoardStatus
{
  Initializing = 0,
  CheckingNetwork = 1,
  SystemDisc = 2,
  TestingGameProgram = 3,
  LoadingGameProgram = 4,
  LoadedGameProgram = 5,
  Error = 6,
};

enum InquiryType
{
  Version1 = 0x21484100,
  Version2 = 0x29484100,
};

// A window of board memory that DI commands can address.
struct MediaBoardRange
{
  u32 start;
  u32 end;
  u8* buffer;
  std::size_t buffer_size;

  MediaBoardRange(u32 start_, u32 size_, std::span<u8> buffer_);
};

// DI opcodes the board answers.
enum class AMMBDICommand : u16
{
  Inquiry = 0x12,
  Read = 0xA8,
  Write = 0xAA,
  Execute = 0xAB,
};

// Board command codes. Some numbers have two names: the NETDIMM firmware
// reuses the low command numbers.
enum class AMMBCommand : u16
{
  Unknown_000 = 0x000,
  GetDIMMSize = 0x001,

  GetMediaBoardStatus = 0x100,
  GetSegaBootVersion = 0x101,
  GetSystemFlags = 0x102,
  GetMediaBoardSerial = 0x103,
  GetNetworkConfig = 0x104,

  NetworkReInit = 0x204,

  TestHardware = 0x301,

  // Sockets (Mario Kart Arcade GP).
  Accept = 0x401,
  Bind = 0x402,
  Closesocket = 0x403,
  Connect = 0x404,
  GetIPbyDNS = 0x405,
  InetAddr = 0x406,
  Ioctl = 0x407,
  Listen = 0x408,
  Recv = 0x409,
  Send = 0x40A,
  Socket = 0x40B,
  Select = 0x40C,
  Shutdown = 0x40D,
  SetSockOpt = 0x40E,
  GetSockOpt = 0x40F,
  SetTimeOuts = 0x410,
  GetLastError = 0x411,
  RouteAdd = 0x412,
  RouteDelete = 0x413,
  GetParambyDHCPExec = 0x414,
  ModifyMyIPaddr = 0x415,
  Recvfrom = 0x416,
  Sendto = 0x417,
  RecvDimmImage = 0x418,
  SendDimmImage = 0x419,

  // Cabinet link (F-Zero AX).
  InitLink = 0x601,
  Unknown_605 = 0x605,
  SetupLink = 0x606,
  SearchDevices = 0x607,
  Unknown_608 = 0x608,
  Unknown_614 = 0x614,

  // All.Net (Virtua Striker 4 2006).
  AllNetInit = 0x700,
  Unknown_701 = 0x701,
  Unknown_702 = 0x702,
  Unknown_703 = 0x703,
  Unknown_704 = 0x704,

  // NETDIMM names.
  Unknown_001 = 0x001,
  GetNetworkFirmVersion = 0x101,
  Unknown_103 = 0x103,
};

enum MediaBoardAddress : u32
{
  MediaBoardStatus1 = 0x80000000,
  MediaBoardStatus2 = 0x80000020,
  MediaBoardStatus3 = 0x80000040,

  FirmwareStatus1 = 0x80000120,
  FirmwareStatus2 = 0x80000140,

  BackupMemory = 0x000006A0,

  DIMMMemory = 0x1F000000,
  DIMMMemory2 = 0xFF000000,

  DIMMExtraSettings = 0x1FFEFFE0,

  AllNetSettings = 0x1EFF8000,

  NetworkControl = 0xFFFF0000,

  DIMMCommandVersion1 = 0x1F900000,
  DIMMCommandVersion2 = 0x84000000,
  DIMMCommandVersion2_2 = 0x89000000,
  DIMMCommandExecute1 = 0x84000040,
  DIMMCommandExecute2 = 0x88000000,

  NetworkCommandAddress1 = 0x1F800200,
  NetworkCommandAddress2 = 0x89040200,

  NetworkBufferAddress1 = 0x1FA00000,
  NetworkBufferAddress2 = 0x1FD00000,
  NetworkBufferAddress3 = 0x89100000,
  NetworkBufferAddress4 = 0x89180000,
  NetworkBufferAddress5 = 0x1FB00000,

  AllNetBuffer = 0x89010000,

  FirmwareAddress = 0x84800000,

  FirmwareMagicWrite1 = 0x00600000,
  FirmwareMagicWrite2 = 0x00700000,
};

// Socket call status codes as the board reports them (close to the WSA
// numbering; EWOULDBLOCK and SUCCESS share a value).
enum SocketStatusCodes
{
  SSC_E_4 = -4,
  SSC_E_3 = -3,
  SSC_E_2 = -2,
  SSC_E_1 = -1,

  SSC_EINTR = 4,
  SSC_EBADF = 9,
  SSC_E_11 = 11,
  SSC_EACCES = 13,
  SSC_EFAULT = 14,
  SSC_E_23 = 23,
  SSC_AEMFILE = 24,
  SSC_EMSGSIZE = 36,
  SSC_EAFNOSUPPORT = 47,
  SSC_EADDRINUSE = 48,
  SSC_EADDRNOTAVAIL = 49,
  SSC_E_50 = 50,
  SSC_ENETUNREACH = 51,
  SSC_ENOBUFS = 55,
  SSC_EISCONN = 56,
  SSC_ENOTCONN = 57,
  SSC_ETIMEDOUT = 60,
  SSC_ECONNREFUSED = 61,
  SSC_EHOSTUNREACH = 65,
  SSC_EHOSTDOWN = 67,
  SSC_EWOULDBLOCK = 70,
  SSC_E_69 = 69,
  SSC_SUCCESS = 70,
};

// Number of guest socket numbers.
static constexpr std::size_t SOCKET_FD_MAX = 64;

void Init();
void FirmwareMap(bool on);
void InitDIMM(const DiscIO::Volume& volume);
void InitKeys(u32 key_a, u32 key_b, u32 key_c);
// Returns 0 when the board handled the command.
u32 ExecuteCommand(std::array<u32, 3>& dicmd_buf, u32* diimm_buf, u32 address, u32 length);
u32 GetGameType();
u32 GetMediaType();
bool GetTestMenu();
void Shutdown();
void DoState(PointerWrap& p);

// "original=replacement description", as entered in the settings.
struct ParsedIPRedirection
{
  std::string_view original;
  std::string_view replacement;
  std::string_view description;
};

std::optional<ParsedIPRedirection> ParseIPRedirection(std::string_view str);

struct IPRedirection
{
  Common::IPv4PortRange original;
  Common::IPv4PortRange replacement;

  // Maps an address inside `original` onto `replacement` (and back); check
  // IsMatch first.
  Common::IPv4Port Apply(Common::IPv4Port subject) const;
  Common::IPv4Port Reverse(Common::IPv4Port subject) const;
  std::string ToString() const;
};

using IPRedirections = std::vector<IPRedirection>;

IPRedirections GetIPRedirections();

// The host socket behind a guest socket number, for the debugger.
s32 DebuggerGetSocket(u32 triforce_fd);

};  // namespace AMMediaboard
