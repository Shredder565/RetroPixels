// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/PowerPC/GDBStub.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#ifdef _WIN32
#include <iphlpapi.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "Common/SocketContext.h"
#include "Core/Core.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/Gekko.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PPCCache.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

namespace GDBStub
{
namespace
{
#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket NO_SOCKET = INVALID_SOCKET;
constexpr int SHUTDOWN_BOTH = SD_BOTH;
void CloseNativeSocket(NativeSocket s)
{
  closesocket(s);
}
#else
using NativeSocket = int;
constexpr NativeSocket NO_SOCKET = -1;
constexpr int SHUTDOWN_BOTH = SHUT_RDWR;
void CloseNativeSocket(NativeSocket s)
{
  close(s);
}
#endif

// Largest packet (framing included) the stub buffers in either direction.
constexpr std::size_t PACKET_LIMIT = 10000;
// Largest payload that still fits once wrapped as $payload#xx.
constexpr std::size_t REPLY_PAYLOAD_LIMIT = PACKET_LIMIT - 4;

constexpr char PACKET_BEGIN = '$';
constexpr char PACKET_CHECKSUM = '#';
constexpr char ACKNOWLEDGE = '+';
constexpr char REJECT = '-';
constexpr u8 INTERRUPT = 0x03;

// How often the stub polls the connection while the CPU is running freely.
constexpr s64 POLL_PERIOD_CYCLES = 100000;

// Mach-O CPU identifiers reported by qHostInfo (PowerPC, 750 subtype).
constexpr int HOST_CPU_TYPE = 18;
constexpr int HOST_CPU_SUBTYPE = 9;

// Breakpoint kinds named by the first field of Z/z packets. Software and hardware execution
// breakpoints are treated alike.
enum class StopPointKind : u32
{
  SoftwareExecute = 0,
  HardwareExecute = 1,
  WriteWatch = 2,
  ReadWatch = 3,
  AccessWatch = 4,
};
constexpr u32 LAST_STOP_POINT_KIND = static_cast<u32>(StopPointKind::AccessWatch);

bool IsExecuteKind(StopPointKind kind)
{
  return kind == StopPointKind::SoftwareExecute || kind == StopPointKind::HardwareExecute;
}

struct Connection
{
  std::optional<Common::SocketContext> socket_library;
  NativeSocket listener = NO_SOCKET;
  NativeSocket peer = NO_SOCKET;
  bool debugger_has_control = false;
  bool fresh_connection = false;
  CoreTiming::EventType* poll_event = nullptr;
};

Connection s_conn;

// The payload of the packet being served, always followed by at least one zero byte so it can
// be treated as a C string.
std::array<char, PACKET_LIMIT> s_payload_storage;
std::size_t s_payload_size = 0;

// The payload up to its first zero byte, as text commands are matched.
std::string_view PayloadText()
{
  return s_payload_storage.data();
}

// Returns the payload byte at `index`, or zero past its end.
u8 PayloadByte(std::size_t index)
{
  return index < s_payload_size ? static_cast<u8>(s_payload_storage[index]) : 0;
}

// --- hex helpers ----------------------------------------------------------------------------

u8 HexValue(u8 digit)
{
  if (digit >= '0' && digit <= '9')
    return digit - '0';
  if (digit >= 'A' && digit <= 'F')
    return digit - 'A' + 10;
  if (digit >= 'a' && digit <= 'f')
    return digit - 'a' + 10;
  ERROR_LOG_FMT(GDB_STUB, "gdb: '{}' ({:02x}) is not a hex digit", static_cast<char>(digit), digit);
  return 0;
}

char HexDigit(u32 value)
{
  static constexpr char DIGITS[] = "0123456789ABCDEF";
  return DIGITS[value & 0xF];
}

// Appends `digits` hex digits of `value`, most significant first.
void AppendHex(std::string& out, u64 value, int digits)
{
  for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4)
    out.push_back(HexDigit(static_cast<u32>(value >> shift)));
}

// Reads `digits` hex digits from the payload starting at `pos`.
u64 PayloadHex(std::size_t pos, int digits)
{
  u64 value = 0;
  for (int i = 0; i < digits; ++i)
    value = (value << 4) | HexValue(PayloadByte(pos + i));
  return value;
}

// Accumulates hex digits from `pos` until `terminator` or the end of the payload; leaves `pos`
// just past the terminator.
u32 PayloadHexField(std::size_t& pos, char terminator)
{
  u32 value = 0;
  while (pos < s_payload_size && s_payload_storage[pos] != terminator)
    value = (value << 4) | HexValue(PayloadByte(pos++));
  ++pos;
  return value;
}

// Accumulates hex digits from `pos` to the end of the payload.
u32 PayloadHexRest(std::size_t pos)
{
  u32 value = 0;
  while (pos < s_payload_size)
    value = (value << 4) | HexValue(PayloadByte(pos++));
  return value;
}

// --- transport -------------------------------------------------------------------------------

void SendRaw(char byte)
{
  if (send(s_conn.peer, &byte, 1, 0) != 1)
    ERROR_LOG_FMT(GDB_STUB, "gdb: could not send '{}'", byte);
}

std::optional<u8> ReceiveByte()
{
  char byte = 0;
  const auto received = recv(s_conn.peer, &byte, 1, MSG_WAITALL);
  if (received == 1)
    return static_cast<u8>(byte);

  ERROR_LOG_FMT(GDB_STUB, "gdb: connection read failed ({})", static_cast<long long>(received));
  Deinit();
  return std::nullopt;
}

bool InputPending()
{
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(s_conn.peer, &readable);
  timeval wait{};
  wait.tv_usec = 20;
  if (select(static_cast<int>(s_conn.peer + 1), &readable, nullptr, nullptr, &wait) < 0)
  {
    ERROR_LOG_FMT(GDB_STUB, "gdb: select on the connection failed");
    return false;
  }
  return FD_ISSET(s_conn.peer, &readable) != 0;
}

u8 Checksum(std::string_view bytes)
{
  u8 sum = 0;
  for (const char c : bytes)
    sum += static_cast<u8>(c);
  return sum;
}

void Reply(std::string_view payload)
{
  if (!IsActive())
    return;

  if (payload.size() > REPLY_PAYLOAD_LIMIT)
  {
    ERROR_LOG_FMT(GDB_STUB, "gdb: reply of {} bytes truncated", payload.size());
    payload = payload.substr(0, REPLY_PAYLOAD_LIMIT);
  }

  std::string framed;
  framed.reserve(payload.size() + 4);
  framed.push_back(PACKET_BEGIN);
  framed.append(payload);
  framed.push_back(PACKET_CHECKSUM);
  AppendHex(framed, Checksum(payload), 2);
  DEBUG_LOG_FMT(GDB_STUB, "gdb: sending {}", framed);

  std::size_t sent = 0;
  while (sent < framed.size())
  {
    const auto chunk =
        send(s_conn.peer, framed.data() + sent, static_cast<int>(framed.size() - sent), 0);
    if (chunk <= 0)
    {
      ERROR_LOG_FMT(GDB_STUB, "gdb: sending a reply failed");
      Deinit();
      return;
    }
    sent += static_cast<std::size_t>(chunk);
  }
}

void ReplyOk()
{
  Reply("OK");
}

void ReplyError(u32 code)
{
  Reply(fmt::format("E{:02X}", code));
}

// Reads one unit of input. Afterwards s_payload_size is non-zero only if a complete packet with
// a valid checksum arrived (and was acknowledged).
void ReceivePacket()
{
  s_payload_storage.fill(0);
  s_payload_size = 0;

  const std::optional<u8> lead = ReceiveByte();
  if (!lead || *lead == ACKNOWLEDGE)
    return;

  if (*lead == INTERRUPT)
  {
    Core::System::GetInstance().GetCPU().Break();
    SendSignal(Signal::Sigtrap);
    s_conn.debugger_has_control = true;
    INFO_LOG_FMT(GDB_STUB, "gdb: interrupt requested, CPU halted");
    return;
  }

  if (*lead != PACKET_BEGIN)
  {
    WARN_LOG_FMT(GDB_STUB, "gdb: ignoring stray byte {:02x}", *lead);
    return;
  }

  std::size_t size = 0;
  while (true)
  {
    const std::optional<u8> next = ReceiveByte();
    if (!next)
      return;
    if (*next == PACKET_CHECKSUM)
      break;
    s_payload_storage[size++] = static_cast<char>(*next);
    if (size == s_payload_storage.size())
    {
      ERROR_LOG_FMT(GDB_STUB, "gdb: packet too large");
      s_payload_storage.fill(0);
      SendRaw(REJECT);
      return;
    }
  }

  u8 expected = 0;
  for (int i = 0; i < 2; ++i)
  {
    const std::optional<u8> digit = ReceiveByte();
    if (!digit)
    {
      s_payload_storage.fill(0);
      return;
    }
    expected = static_cast<u8>((expected << 4) | HexValue(*digit));
  }

  const std::string_view body(s_payload_storage.data(), size);
  const u8 actual = Checksum(body);
  if (actual != expected)
  {
    ERROR_LOG_FMT(GDB_STUB, "gdb: checksum mismatch (got {:02x}, computed {:02x}) on {}", expected,
                  actual, body);
    SendRaw(REJECT);
    return;
  }

  DEBUG_LOG_FMT(GDB_STUB, "gdb: received {}", body);
  s_payload_size = size;
  SendRaw(ACKNOWLEDGE);
}

// --- queries and thread packets ------------------------------------------------------------

void ServeQuery()
{
  const std::string_view query = PayloadText();
  DEBUG_LOG_FMT(GDB_STUB, "gdb: query {}", query);

  if (query.starts_with("qAttached"))
    return Reply("1");
  if (query == "qC")
    return Reply("QC1");
  if (query == "qfThreadInfo")
    return Reply("m1");
  if (query == "qsThreadInfo")
    return Reply("l");
  if (query.starts_with("qThreadExtraInfo"))
    return Reply("00");
  if (query.starts_with("qHostInfo"))
  {
    return Reply(fmt::format(
        "cputype:{};cpusubtype:{};ostype:unknown;vendor:unknown;endian:big;ptrsize:4",
        HOST_CPU_TYPE, HOST_CPU_SUBTYPE));
  }
  if (query.starts_with("qSupported"))
    return Reply("swbreak+;hwbreak+");
  Reply("");
}

// The emulated machine is presented as a single thread whose id is 1 (0 and -1 mean "any" and
// "all").
void ServeSelectThread()
{
  const std::string_view packet = PayloadText();
  for (const std::string_view accepted : {"Hg-1", "Hc-1", "Hg0", "Hc0", "Hg1", "Hc1"})
  {
    if (packet.starts_with(accepted))
      return ReplyOk();
  }
  ReplyError(1);
}

void ServeThreadAlive()
{
  const std::string_view packet = PayloadText();
  if (packet.starts_with("T1") || packet.starts_with("T-1"))
    return ReplyOk();
  ReplyError(1);
}

// --- registers -------------------------------------------------------------------------------

// GDB register numbers for Broadway:
//   0-31 r0-r31, 32-63 f0-f31 (paired-single slot 0), 64 pc, 65 msr, 66 cr, 67 lr, 68 ctr,
//   69 xer, 70 fpscr, 71-86 sr0-sr15, 87 pvr, 88-103 the eight IBAT/DBAT pairs, then the SPRs
//   listed in TAIL_REGISTERS starting at 104.
constexpr u32 FIRST_GPR = 0;
constexpr u32 FIRST_FPR = 32;
constexpr u32 REG_PC = 64;
constexpr u32 REG_MSR = 65;
constexpr u32 REG_CR = 66;
constexpr u32 REG_FPSCR = 70;
constexpr u32 FIRST_SR = 71;
constexpr u32 FIRST_BAT = 88;
constexpr u32 FIRST_TAIL = 104;

constexpr u32 NOT_AN_SPR = 0xFFFFFFFF;

constexpr std::array<u32, 39> TAIL_REGISTERS = {
    SPR_SDR,    SPR_ASR,   SPR_DAR,   SPR_DSISR, SPR_SPRG0,  SPR_SPRG1, SPR_SPRG2, SPR_SPRG3,
    SPR_SRR0,   SPR_SRR1,  SPR_TL,    SPR_TU,    SPR_DEC,    SPR_DABR,  SPR_EAR,   SPR_HID0,
    SPR_HID1,   SPR_IABR,  SPR_DABR,  NOT_AN_SPR, SPR_UMMCR0, SPR_UPMC1, SPR_UPMC2, SPR_USIA,
    SPR_UMMCR1, SPR_UPMC3, SPR_UPMC4, SPR_MMCR0, SPR_PMC1,   SPR_PMC2,  SPR_SIA,   SPR_MMCR1,
    SPR_PMC3,   SPR_PMC4,  SPR_L2CR,  SPR_ICTC,  SPR_THRM1,  SPR_THRM2, SPR_THRM3,
};

enum class RegisterBank
{
  None,
  Gpr,
  Fpr,
  Pc,
  Msr,
  Cr,
  Fpscr,
  Segment,
  Special,
};

struct RegisterSlot
{
  RegisterBank bank = RegisterBank::None;
  u32 index = 0;
  // Number of hex digits the register occupies on the wire.
  int digits = 8;
};

RegisterSlot LocateRegister(u32 number)
{
  if (number < FIRST_FPR)
    return {RegisterBank::Gpr, number - FIRST_GPR};
  if (number < REG_PC)
    return {RegisterBank::Fpr, number - FIRST_FPR, 16};
  if (number >= FIRST_SR && number < FIRST_SR + 16)
    return {RegisterBank::Segment, number - FIRST_SR};
  if (number >= FIRST_BAT && number < FIRST_TAIL)
    return {RegisterBank::Special, SPR_IBAT0U + (number - FIRST_BAT)};
  if (number >= FIRST_TAIL && number < FIRST_TAIL + TAIL_REGISTERS.size())
  {
    const u32 spr = TAIL_REGISTERS[number - FIRST_TAIL];
    if (spr == NOT_AN_SPR)
      return {};
    return {RegisterBank::Special, spr, spr == SPR_ASR ? 16 : 8};
  }

  switch (number)
  {
  case REG_PC:
    return {RegisterBank::Pc};
  case REG_MSR:
    return {RegisterBank::Msr};
  case REG_CR:
    return {RegisterBank::Cr};
  case 67:
    return {RegisterBank::Special, SPR_LR};
  case 68:
    return {RegisterBank::Special, SPR_CTR};
  case 69:
    return {RegisterBank::Special, SPR_XER};
  case REG_FPSCR:
    return {RegisterBank::Fpscr};
  case 87:
    return {RegisterBank::Special, SPR_PVR};
  default:
    return {};
  }
}

u64 FetchRegister(const PowerPC::PowerPCState& ppc, const RegisterSlot& slot)
{
  switch (slot.bank)
  {
  case RegisterBank::Gpr:
    return ppc.gpr[slot.index];
  case RegisterBank::Fpr:
    return ppc.ps[slot.index].PS0AsU64();
  case RegisterBank::Pc:
    return ppc.pc;
  case RegisterBank::Msr:
    return ppc.msr.Hex;
  case RegisterBank::Cr:
    return ppc.cr.Get();
  case RegisterBank::Fpscr:
    return ppc.fpscr.Hex;
  case RegisterBank::Segment:
    return ppc.sr[slot.index];
  case RegisterBank::Special:
    return ppc.spr[slot.index];
  case RegisterBank::None:
    break;
  }
  return 0;
}

void StoreRegister(Core::System& system, const RegisterSlot& slot, u64 value)
{
  auto& ppc = system.GetPPCState();
  const u32 value32 = static_cast<u32>(value);
  switch (slot.bank)
  {
  case RegisterBank::Gpr:
    ppc.gpr[slot.index] = value32;
    break;
  case RegisterBank::Fpr:
    ppc.ps[slot.index].SetPS0(value);
    break;
  case RegisterBank::Pc:
    ppc.pc = value32;
    break;
  case RegisterBank::Msr:
    ppc.msr.Hex = value32;
    system.GetPowerPC().MSRUpdated();
    break;
  case RegisterBank::Cr:
    ppc.cr.Set(value32);
    break;
  case RegisterBank::Fpscr:
    ppc.fpscr.Hex = value32;
    break;
  case RegisterBank::Segment:
    ppc.sr[slot.index] = value32;
    system.GetMMU().SRUpdated();
    break;
  case RegisterBank::Special:
    ppc.spr[slot.index] = value32;
    if (slot.index == SPR_SDR)
      system.GetMMU().SDRUpdated();
    else if (slot.index == SPR_MMCR0 || slot.index == SPR_MMCR1)
      PowerPC::MMCRUpdated(ppc);
    break;
  case RegisterBank::None:
    break;
  }
}

// "p" followed by one or two hex digits of register number.
void ServeReadRegister()
{
  u32 number = HexValue(PayloadByte(1));
  if (PayloadByte(2) != 0)
    number = (number << 4) | HexValue(PayloadByte(2));

  const RegisterSlot slot = LocateRegister(number);
  if (slot.bank == RegisterBank::None)
    return ReplyError(1);

  std::string text;
  AppendHex(text, FetchRegister(Core::System::GetInstance().GetPPCState(), slot), slot.digits);
  Reply(text);
}

// "P" followed by one or two hex digits of register number, '=', and the value.
void ServeWriteRegister()
{
  u32 number = HexValue(PayloadByte(1));
  std::size_t value_pos = 3;
  if (PayloadByte(2) != '=')
  {
    number = (number << 4) | HexValue(PayloadByte(2));
    value_pos = 4;
  }

  const RegisterSlot slot = LocateRegister(number);
  if (slot.bank == RegisterBank::None)
    return ReplyError(1);

  StoreRegister(Core::System::GetInstance(), slot, PayloadHex(value_pos, slot.digits));
  ReplyOk();
}

// "g" reports, and "G" replaces, the general-purpose registers only.
void ServeReadAllRegisters()
{
  const auto& ppc = Core::System::GetInstance().GetPPCState();
  std::string text;
  text.reserve(32 * 8);
  for (const u32 value : ppc.gpr)
    AppendHex(text, value, 8);
  Reply(text);
}

void ServeWriteAllRegisters()
{
  auto& ppc = Core::System::GetInstance().GetPPCState();
  for (std::size_t r = 0; r < 32; ++r)
    ppc.gpr[r] = static_cast<u32>(PayloadHex(1 + r * 8, 8));
  ReplyOk();
}

// --- memory ----------------------------------------------------------------------------------

// "m addr,length"
void ServeReadMemory(const Core::CPUThreadGuard& guard)
{
  std::size_t pos = 1;
  const u32 address = PayloadHexField(pos, ',');
  const u32 length = PayloadHexRest(pos);
  INFO_LOG_FMT(GDB_STUB, "gdb: memory read of {:08x} bytes at {:08x}", length, address);

  if (u64{length} * 2 > REPLY_PAYLOAD_LIMIT)
    return ReplyError(1);
  if (!PowerPC::MMU::HostIsRAMAddress(guard, address))
    return ReplyError(0);

  const u8* bytes = Core::System::GetInstance().GetMemory().GetPointerForRange(address, length);
  if (bytes == nullptr)
    return ReplyError(0);

  std::string text;
  text.reserve(std::size_t{length} * 2);
  for (const u8 byte : std::span(bytes, length))
    AppendHex(text, byte, 2);
  Reply(text);
}

// "M addr,length:data"
void ServeWriteMemory(const Core::CPUThreadGuard& guard)
{
  std::size_t pos = 1;
  const u32 address = PayloadHexField(pos, ',');
  const u32 length = PayloadHexField(pos, ':');
  INFO_LOG_FMT(GDB_STUB, "gdb: memory write of {:08x} bytes at {:08x}", length, address);

  if (!PowerPC::MMU::HostIsRAMAddress(guard, address))
    return ReplyError(0);

  u8* bytes = Core::System::GetInstance().GetMemory().GetPointerForRange(address, length);
  if (bytes == nullptr)
    return ReplyError(0);

  for (u32 i = 0; i < length; ++i)
    bytes[i] = static_cast<u8>(PayloadHex(pos + std::size_t{i} * 2, 2));
  ReplyOk();
}

// --- breakpoints -----------------------------------------------------------------------------

void InsertStopPoint(StopPointKind kind, u32 address, u32 length)
{
  auto& power_pc = Core::System::GetInstance().GetPowerPC();
  if (IsExecuteKind(kind))
  {
    power_pc.GetBreakPoints().Add(address);
    INFO_LOG_FMT(GDB_STUB, "gdb: breakpoint (kind {}) at {:08x}, length {:08x}",
                 static_cast<u32>(kind), address, length);
  }
  else
  {
    TMemCheck watch;
    watch.start_address = address;
    watch.end_address = address + length - 1;
    watch.is_ranged = length > 1;
    watch.is_break_on_read = kind == StopPointKind::ReadWatch || kind == StopPointKind::AccessWatch;
    watch.is_break_on_write =
        kind == StopPointKind::WriteWatch || kind == StopPointKind::AccessWatch;
    watch.break_on_hit = true;
    watch.log_on_hit = false;
    watch.is_enabled = true;
    power_pc.GetMemChecks().Add(std::move(watch));
    INFO_LOG_FMT(GDB_STUB, "gdb: watchpoint (kind {}) at {:08x}, length {:08x}",
                 static_cast<u32>(kind), address, length);
  }
  Host_PPCBreakpointsChanged();
}

void ErasePoint(StopPointKind kind, u32 address, u32 length)
{
  auto& power_pc = Core::System::GetInstance().GetPowerPC();
  if (IsExecuteKind(kind))
  {
    if (power_pc.GetBreakPoints().Remove(address))
      INFO_LOG_FMT(GDB_STUB, "gdb: breakpoint at {:08x} removed", address);
  }
  else
  {
    auto& watches = power_pc.GetMemChecks();
    DelayedMemCheckUpdate pending_update(&watches);
    // Removal is by start address, so stop once nothing starting there is left even if some
    // other watch still overlaps the range.
    while (watches.GetMemCheck(address, length) != nullptr)
    {
      DelayedMemCheckUpdate removed = watches.Remove(address);
      const bool made_progress = removed;
      pending_update |= std::move(removed);
      if (!made_progress)
        break;
      INFO_LOG_FMT(GDB_STUB, "gdb: watchpoint at {:08x}, length {:08x} removed", address, length);
    }
  }
  Host_PPCBreakpointsChanged();
}

struct StopPointRequest
{
  StopPointKind kind;
  u32 address;
  u32 length;
};

// "Z kind,addr,length" / "z kind,addr,length". Only a single kind digit is understood.
std::optional<StopPointRequest> ParseStopPoint()
{
  const u32 kind = HexValue(PayloadByte(1));
  if (kind > LAST_STOP_POINT_KIND)
    return std::nullopt;

  std::size_t pos = 3;
  const u32 address = PayloadHexField(pos, ',');
  const u32 length = PayloadHexRest(pos);
  return StopPointRequest{static_cast<StopPointKind>(kind), address, length};
}

void ServeInsertStopPoint()
{
  const auto request = ParseStopPoint();
  if (!request)
    return ReplyError(1);
  InsertStopPoint(request->kind, request->address, request->length);
  ReplyOk();
}

void ServeRemoveStopPoint()
{
  const auto request = ParseStopPoint();
  if (!request)
    return ReplyError(1);
  ErasePoint(request->kind, request->address, request->length);
  ReplyOk();
}

// --- setup -----------------------------------------------------------------------------------

void PollCallback(Core::System& system, u64, s64)
{
  ProcessCommands(false);
  if (IsActive())
    system.GetCoreTiming().ScheduleEvent(POLL_PERIOD_CYCLES, s_conn.poll_event);
}

// Listens on the given address, blocks until one debugger connects, then starts polling.
void ListenAndAccept(int family, const sockaddr* address, socklen_t address_size)
{
  s_conn.socket_library.emplace();

  s_conn.listener = socket(family, SOCK_STREAM, 0);
  if (s_conn.listener == NO_SOCKET)
    ERROR_LOG_FMT(GDB_STUB, "gdb: could not create the listening socket");

  const int reuse = 1;
  if (setsockopt(s_conn.listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                 static_cast<socklen_t>(sizeof(reuse))) < 0)
  {
    ERROR_LOG_FMT(GDB_STUB, "gdb: could not set SO_REUSEADDR");
  }
  if (bind(s_conn.listener, address, address_size) < 0)
    ERROR_LOG_FMT(GDB_STUB, "gdb: could not bind the listening socket");
  if (listen(s_conn.listener, 1) < 0)
    ERROR_LOG_FMT(GDB_STUB, "gdb: could not listen for a debugger");

  INFO_LOG_FMT(GDB_STUB, "gdb: waiting for a debugger to connect");
  s_conn.peer = accept(s_conn.listener, nullptr, nullptr);
  if (s_conn.peer == NO_SOCKET)
    ERROR_LOG_FMT(GDB_STUB, "gdb: accepting the debugger failed");
  else
    INFO_LOG_FMT(GDB_STUB, "gdb: debugger connected");
  s_conn.fresh_connection = true;

  if (s_conn.listener != NO_SOCKET)
    CloseNativeSocket(s_conn.listener);
  s_conn.listener = NO_SOCKET;

  auto& core_timing = Core::System::GetInstance().GetCoreTiming();
  s_conn.poll_event = core_timing.RegisterEvent("GDBStubUpdate", PollCallback);
  core_timing.ScheduleEvent(POLL_PERIOD_CYCLES, s_conn.poll_event);
  s_conn.debugger_has_control = true;
}

void Disconnect(NativeSocket& s)
{
  if (s == NO_SOCKET)
    return;
  shutdown(s, SHUTDOWN_BOTH);
  CloseNativeSocket(s);
  s = NO_SOCKET;
}
}  // namespace

void ProcessCommands(bool loop_until_continue)
{
  s_conn.fresh_connection = false;
  auto& system = Core::System::GetInstance();
  auto& cpu = system.GetCPU();

  while (IsActive())
  {
    if (cpu.GetState() == CPU::State::PowerDown)
    {
      Deinit();
      INFO_LOG_FMT(GDB_STUB, "gdb: emulated machine powered down, debugger detached");
      return;
    }

    if (!InputPending())
    {
      if (!loop_until_continue)
        return;
      continue;
    }

    ReceivePacket();
    if (s_payload_size == 0)
      continue;

    switch (s_payload_storage[0])
    {
    case 'q':
      ServeQuery();
      break;
    case 'H':
      ServeSelectThread();
      break;
    case 'T':
      ServeThreadAlive();
      break;
    case '?':
      SendSignal(Signal::Sigterm);
      break;
    case 'g':
      ServeReadAllRegisters();
      break;
    case 'G':
      ServeWriteAllRegisters();
      break;
    case 'p':
      ServeReadRegister();
      break;
    case 'P':
      ServeWriteRegister();
      break;
    case 'm':
    {
      ASSERT(Core::IsCPUThread());
      const Core::CPUThreadGuard guard(system);
      ServeReadMemory(guard);
      break;
    }
    case 'M':
    {
      ASSERT(Core::IsCPUThread());
      const Core::CPUThreadGuard guard(system);
      ServeWriteMemory(guard);
      // The debugger may have patched code.
      system.GetPPCState().iCache.Reset(system.GetJitInterface());
      Host_UpdateDisasmDialog();
      break;
    }
    case 'Z':
      ServeInsertStopPoint();
      break;
    case 'z':
      ServeRemoveStopPoint();
      break;
    case 's':
      cpu.SetStepping(true);
      Core::NotifyStateChanged(Core::State::Paused);
      return;
    case 'c':
    case 'C':
      cpu.Continue();
      s_conn.debugger_has_control = false;
      return;
    case 'k':
      Deinit();
      INFO_LOG_FMT(GDB_STUB, "gdb: debugger sent kill, detached");
      return;
    default:
      // An empty reply tells the debugger the packet is not supported.
      Reply("");
      break;
    }
  }
}

#ifndef _WIN32
void InitLocal(const char* socket)
{
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::size_t path_length = std::strlen(socket);
  if (path_length >= sizeof(address.sun_path))
  {
    ERROR_LOG_FMT(GDB_STUB, "gdb: socket path {} is too long", socket);
    return;
  }
  std::memcpy(address.sun_path, socket, path_length + 1);

  unlink(socket);
  ListenAndAccept(AF_UNIX, reinterpret_cast<const sockaddr*>(&address),
                  static_cast<socklen_t>(sizeof(address)));
}
#endif

void Init(u32 port)
{
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<u16>(port));
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  ListenAndAccept(AF_INET, reinterpret_cast<const sockaddr*>(&address),
                  static_cast<socklen_t>(sizeof(address)));
}

void Deinit()
{
  Disconnect(s_conn.listener);
  Disconnect(s_conn.peer);
  s_conn.socket_library.reset();
  s_conn.debugger_has_control = false;
}

bool IsActive()
{
  return s_conn.listener != NO_SOCKET || s_conn.peer != NO_SOCKET;
}

bool HasControl()
{
  return s_conn.debugger_has_control;
}

void TakeControl()
{
  s_conn.debugger_has_control = true;
}

bool JustConnected()
{
  return s_conn.fresh_connection;
}

void SendSignal(Signal signal)
{
  // Stop reply carrying pc (register 0x40) and r1 (the stack pointer).
  const auto& ppc = Core::System::GetInstance().GetPPCState();
  Reply(fmt::format("T{:02x}{:02x}:{:08x};{:02x}:{:08x};", static_cast<u8>(signal), REG_PC, ppc.pc,
                    1, ppc.gpr[1]));
}
}  // namespace GDBStub
