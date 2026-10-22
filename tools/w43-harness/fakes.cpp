// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.
//
// W43 harness: fakes for everything the W43 device files call outside themselves, plus the
// trace recorder. Every call that a device makes into a neighbour and that the neighbour could
// observe is written to the behaviour trace; log calls go to the log trace.

#include "w43fakes.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <fmt/format.h>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"

#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HW/EXI/BBA/TAPServerConnection.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"
#include "DiscIO/Blob.h"
#include "DiscIO/CachedBlob.h"
#include "DiscIO/Volume.h"
#include "VideoCommon/OnScreenDisplay.h"

// ================================================================================================
// Traces
// ================================================================================================

namespace w43
{
namespace
{
struct TraceState
{
  bool active = false;
  bool echo = false;
  std::string mask_dir;
  u64 behaviour = 0xcbf29ce484222325ULL;
  u64 logs = 0xcbf29ce484222325ULL;
  u64 lines = 0;
  u64 log_lines = 0;
};
TraceState g_trace;

bool IsHex(char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Replaces the case directory with $DIR and host pointers (0x + 9..16 hex digits) with 0xPTR.
std::string Mask(std::string line)
{
  if (!g_trace.mask_dir.empty())
  {
    for (std::size_t pos = line.find(g_trace.mask_dir); pos != std::string::npos;
         pos = line.find(g_trace.mask_dir, pos + 4))
    {
      line.replace(pos, g_trace.mask_dir.size(), "$DIR");
    }
  }
  std::string out;
  out.reserve(line.size());
  for (std::size_t i = 0; i < line.size();)
  {
    if (line[i] == '0' && i + 1 < line.size() && line[i + 1] == 'x')
    {
      std::size_t j = i + 2;
      while (j < line.size() && IsHex(line[j]))
        ++j;
      const std::size_t digits = j - (i + 2);
      if (digits >= 9 && digits <= 16)
      {
        out += "0xPTR";
        i = j;
        continue;
      }
    }
    out += line[i++];
  }
  return out;
}
}  // namespace

u64 Fnv(std::span<const u8> data, u64 h)
{
  for (const u8 b : data)
  {
    h ^= b;
    h *= 0x100000001b3ULL;
  }
  return h;
}

u64 FnvStr(std::string_view s, u64 h)
{
  return Fnv({reinterpret_cast<const u8*>(s.data()), s.size()}, h);
}

std::string Hex(std::span<const u8> data)
{
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (const u8 b : data)
  {
    out += digits[b >> 4];
    out += digits[b & 15];
  }
  return out;
}

u64 Mix(u64 x)
{
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

u64 Rng::Next()
{
  state += 0x9e3779b97f4a7c15ULL;
  u64 z = state;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  return z ^ (z >> 31);
}

std::vector<u8> Rng::Bytes(std::size_t n)
{
  std::vector<u8> out(n);
  for (auto& b : out)
    b = Byte();
  return out;
}

// A bind line is held back until the next trace line: when that line is the device's
// "BindEphemeralPort:" log, the port was picked by a clock-seeded generator and is masked.
std::string g_held_bind;       // with "{PORT}" where the port goes
std::string g_held_bind_port;  // the real port text

void EmitB(std::string line);

void FlushHeldBind(bool ephemeral)
{
  if (g_held_bind.empty())
    return;
  std::string line = std::exchange(g_held_bind, {});
  const auto at = line.find("{PORT}");
  if (at != std::string::npos)
    line.replace(at, 6, ephemeral ? std::string("EPH") : g_held_bind_port);
  EmitB(std::move(line));
}

void HoldBind(std::string line, std::string port)
{
  FlushHeldBind(false);
  g_held_bind = std::move(line);
  g_held_bind_port = std::move(port);
}

void B(std::string line)
{
  if (!g_trace.active)
    return;
  FlushHeldBind(false);
  EmitB(std::move(line));
}

void EmitB(std::string line)
{
  line = Mask(std::move(line));
  if (g_trace.echo)
    std::printf("B %s\n", line.c_str());
  g_trace.behaviour = FnvStr(line, g_trace.behaviour);
  g_trace.behaviour = FnvStr("\n", g_trace.behaviour);
  ++g_trace.lines;
}

void L(std::string line)
{
  if (!g_trace.active)
    return;
  // The ephemeral source port is chosen by a clock-seeded generator.
  if (line.find("BindEphemeralPort:") != std::string::npos)
  {
    FlushHeldBind(true);
    return;
  }
  FlushHeldBind(false);
  line = Mask(std::move(line));
  if (g_trace.echo)
    std::printf("L %s\n", line.c_str());
  g_trace.logs = FnvStr(line, g_trace.logs);
  g_trace.logs = FnvStr("\n", g_trace.logs);
  ++g_trace.log_lines;
}

}  // namespace w43

// ================================================================================================
// Fake neighbour state
// ================================================================================================

namespace w43
{
namespace
{
std::vector<u8> g_ram(RAM_SIZE);
GCPadStatus g_pads[4];
int g_si_types[4] = {SerialInterface::SIDEVICE_AM_BASEBOARD, SerialInterface::SIDEVICE_NONE,
                     SerialInterface::SIDEVICE_NONE, SerialInterface::SIDEVICE_NONE};
std::string g_game_id = "GGGE8P";

struct PendingEvent
{
  CoreTiming::EventType* event;
  s64 cycles;
  u64 userdata;
};
std::map<std::string, std::unique_ptr<CoreTiming::EventType>> g_event_types;
std::vector<PendingEvent> g_pending_events;

u64 g_socket_seed = 1;
std::map<std::string, u64> g_socket_counters;

u64 g_tap_seed = 1;
std::map<std::string, u64> g_tap_counters;
std::function<void(std::string&&)> g_tap_callback;
}  // namespace

void BeginCase(u64 seed, const std::string& mask_dir, bool echo)
{
  g_trace = {};
  g_held_bind.clear();
  g_trace.active = true;
  g_trace.echo = echo;
  g_trace.mask_dir = mask_dir;
  std::ranges::fill(g_ram, u8{0});
  for (auto& pad : g_pads)
    pad = GCPadStatus{};
  g_socket_seed = Mix(seed ^ 0x50c);
  g_socket_counters.clear();
  g_tap_seed = Mix(seed ^ 0x7a9);
  g_tap_counters.clear();
  g_pending_events.clear();
}

CaseHashes EndCase()
{
  FlushHeldBind(false);
  g_trace.active = false;
  return {g_trace.behaviour, g_trace.logs, g_trace.lines, g_trace.log_lines};
}

u8* Ram()
{
  return g_ram.data();
}

void RamWriteBE32(u32 address, u32 value)
{
  address &= 0x3FFFFFFF;
  for (int i = 0; i < 4; ++i)
    g_ram.at(address + i) = static_cast<u8>(value >> (24 - 8 * i));
}

u32 RamReadBE32(u32 address)
{
  address &= 0x3FFFFFFF;
  u32 v = 0;
  for (int i = 0; i < 4; ++i)
    v = (v << 8) | g_ram.at(address + i);
  return v;
}

void RamWrite(u32 address, std::span<const u8> data)
{
  address &= 0x3FFFFFFF;
  std::ranges::copy(data, g_ram.begin() + address);
}

std::vector<u8> RamRead(u32 address, u32 size)
{
  address &= 0x3FFFFFFF;
  return {g_ram.begin() + address, g_ram.begin() + address + size};
}

u64 RamHash()
{
  return Fnv(g_ram);
}

void SetPad(int pad, const GCPadStatus& status)
{
  g_pads[pad & 3] = status;
}

void SetSIDeviceType(int channel, int si_device_type)
{
  g_si_types[channel & 3] = si_device_type;
}

void SetGameID(std::string id)
{
  g_game_id = std::move(id);
}

void FireScheduledEvents()
{
  auto pending = std::move(g_pending_events);
  g_pending_events.clear();
  for (const auto& ev : pending)
  {
    B(fmt::format("CT.fire {} cycles={} ud={}", *ev.event->name, ev.cycles, ev.userdata));
    ev.event->callback(Core::System::GetInstance(), ev.userdata, 0);
  }
}

std::size_t PendingEventCount()
{
  return g_pending_events.size();
}

void SetSocketSeed(u64 seed)
{
  g_socket_seed = seed;
  g_socket_counters.clear();
}

void SetTapSeed(u64 seed)
{
  g_tap_seed = seed;
  g_tap_counters.clear();
}

bool TapDeliver(std::string frame)
{
  if (!g_tap_callback)
    return false;
  B(fmt::format("TAP.deliver {}", Hex({reinterpret_cast<const u8*>(frame.data()), frame.size()})));
  g_tap_callback(std::move(frame));
  return true;
}

void TapForget()
{
  g_tap_callback = nullptr;
}

}  // namespace w43

using w43::B;

// ================================================================================================
// Logging, alerts, translation
// ================================================================================================

namespace Common::Log
{
void GenericLogFmtImpl(LogLevel level, LogType type, const char* file, int line,
                       fmt::string_view format, const fmt::format_args& args)
{
  (void)file;
  (void)line;
  const int lvl = static_cast<int>(level);
  const char c = (lvl >= 0 && lvl < 6) ? LOG_LEVEL_TO_CHAR[lvl] : '?';
  w43::L(fmt::format("{}{} {}", c, static_cast<int>(type), fmt::vformat(format, args)));
}
}  // namespace Common::Log

namespace Common
{
bool MsgAlertFmtImpl(bool yes_no, MsgType style, Log::LogType log_type, const char* file, int line,
                     fmt::string_view format, const fmt::format_args& args)
{
  (void)file;
  (void)line;
  const std::string msg = fmt::vformat(format, args);
  // An ASSERT's text names the source file, line and function, which differ between trees,
  // so only the fact that one fired is recorded, and in the log stream.
  if (msg.starts_with("Failed check:") || msg.find("\nSource: ") != std::string::npos)
  {
    w43::L("ASSERT");
    return true;
  }
  B(fmt::format("ALERT yesno={} style={} type={} {}", yes_no, static_cast<int>(style),
                static_cast<int>(log_type), msg));
  return true;
}

std::string GetStringT(const char* string)
{
  return string;
}
}  // namespace Common

// ================================================================================================
// Core::System and the subsystem managers it hands out
// ================================================================================================

struct Core::System::Parts
{
};

Core::System::System() = default;
Core::System::~System() = default;

namespace
{
// The managers are never constructed: the devices only call the out-of-line methods defined
// below, which ignore `this`.
alignas(64) unsigned char g_manager_storage[8][64];
template <typename T>
T& FakeManager(int index)
{
  return *reinterpret_cast<T*>(g_manager_storage[index]);
}
}  // namespace

Memory::MemoryManager& Core::System::GetMemory() const
{
  return FakeManager<Memory::MemoryManager>(0);
}
CoreTiming::CoreTimingManager& Core::System::GetCoreTiming() const
{
  return FakeManager<CoreTiming::CoreTimingManager>(1);
}
ExpansionInterface::ExpansionInterfaceManager& Core::System::GetExpansionInterface() const
{
  return FakeManager<ExpansionInterface::ExpansionInterfaceManager>(2);
}
SerialInterface::SerialInterfaceManager& Core::System::GetSerialInterface() const
{
  return FakeManager<SerialInterface::SerialInterfaceManager>(3);
}
SystemTimers::SystemTimersManager& Core::System::GetSystemTimers() const
{
  return FakeManager<SystemTimers::SystemTimersManager>(4);
}

// ---- Memory ------------------------------------------------------------------------------------

namespace Memory
{
std::span<u8> MemoryManager::GetSpanForAddress(u32 address) const
{
  const u32 physical = address & 0x3FFFFFFF;
  if (physical < w43::RAM_SIZE)
    return {w43::Ram() + physical, w43::RAM_SIZE - physical};
  B(fmt::format("MEM.badspan {:08x}", address));
  return {};
}

u8* MemoryManager::GetPointerForRange(u32 address, size_t size) const
{
  const auto span = GetSpanForAddress(address);
  if (span.data() == nullptr)
    return nullptr;
  if (span.size() < size)
  {
    B(fmt::format("MEM.badrange {:08x} {:x}", address, size));
    return nullptr;
  }
  return span.data();
}

void MemoryManager::CopyFromEmu(void* data, u32 address, size_t size) const
{
  if (size == 0)
    return;
  const u8* source = GetPointerForRange(address, size);
  if (source != nullptr)
    std::memcpy(data, source, size);
}

void MemoryManager::CopyToEmu(u32 address, const void* data, size_t size)
{
  if (size == 0)
    return;
  u8* target = GetPointerForRange(address, size);
  if (target != nullptr)
    std::memcpy(target, data, size);
}

void MemoryManager::Memset(u32 address, u8 value, size_t size)
{
  if (size == 0)
    return;
  u8* target = GetPointerForRange(address, size);
  if (target != nullptr)
    std::memset(target, value, size);
}

u8 MemoryManager::Read_U8(u32 address) const
{
  const u8* p = GetPointerForRange(address, 1);
  return p ? *p : 0;
}

u32 MemoryManager::Read_U32(u32 address) const
{
  const u8* p = GetPointerForRange(address, 4);
  return p ? (u32{p[0]} << 24) | (u32{p[1]} << 16) | (u32{p[2]} << 8) | p[3] : 0;
}

void MemoryManager::Write_U16(u16 var, u32 address)
{
  u8* p = GetPointerForRange(address, 2);
  if (p)
  {
    p[0] = static_cast<u8>(var >> 8);
    p[1] = static_cast<u8>(var);
  }
}

void MemoryManager::Write_U32(u32 var, u32 address)
{
  u8* p = GetPointerForRange(address, 4);
  if (p)
  {
    for (int i = 0; i < 4; ++i)
      p[i] = static_cast<u8>(var >> (24 - 8 * i));
  }
}

void MemoryManager::Write_U32_Swap(u32 var, u32 address)
{
  u8* p = GetPointerForRange(address, 4);
  if (p)
  {
    for (int i = 0; i < 4; ++i)
      p[i] = static_cast<u8>(var >> (8 * i));
  }
}
}  // namespace Memory

// ---- CoreTiming ----------------------------------------------------------------------------

namespace CoreTiming
{
EventType* CoreTimingManager::RegisterEvent(const std::string& name, TimedCallback callback)
{
  B("CT.register " + name);
  // Re-registering (Init after Shutdown) keeps the same EventType so pending events stay valid.
  auto& slot = w43::g_event_types[name];
  if (!slot)
    slot = std::make_unique<EventType>();
  slot->callback = std::move(callback);
  slot->name = &w43::g_event_types.find(name)->first;
  return slot.get();
}

void CoreTimingManager::RemoveEvent(EventType* event_type)
{
  B("CT.remove " + (event_type ? *event_type->name : std::string("null")));
  std::erase_if(w43::g_pending_events,
                [&](const w43::PendingEvent& ev) { return ev.event == event_type; });
}

void CoreTimingManager::ScheduleEvent(s64 cycles_into_future, EventType* event_type, u64 userdata,
                                      FromThread from)
{
  B(fmt::format("CT.schedule {} cycles={} ud={} from={}",
                event_type ? *event_type->name : std::string("null"), cycles_into_future, userdata,
                static_cast<int>(from)));
  if (event_type)
    w43::g_pending_events.push_back({event_type, cycles_into_future, userdata});
}
}  // namespace CoreTiming

// ---- EXI, SI, timers ---------------------------------------------------------------------------

namespace ExpansionInterface
{
void ExpansionInterfaceManager::UpdateInterrupts()
{
  B("EXI.UpdateInterrupts");
}

void ExpansionInterfaceManager::ScheduleUpdateInterrupts(CoreTiming::FromThread from,
                                                         int cycles_late)
{
  B(fmt::format("EXI.ScheduleUpdateInterrupts from={} late={}", static_cast<int>(from),
                cycles_late));
}

// The IEXIDevice base behaviour (base tree, EXI_Device.cpp), repeated here so the harness does
// not pull in every other EXI device.
IEXIDevice::IEXIDevice(Core::System& system) : m_system(system)
{
}

void IEXIDevice::ImmWrite(u32 data, u32 size)
{
  for (u32 index = 0; index < size; ++index)
  {
    u8 outgoing = static_cast<u8>(data >> (24 - 8 * index));
    TransferByte(outgoing);
  }
}

u32 IEXIDevice::ImmRead(u32 size)
{
  u32 word = 0;
  for (u32 index = 0; index < size; ++index)
  {
    u8 incoming = 0;
    TransferByte(incoming);
    word |= u32{incoming} << (24 - 8 * index);
  }
  return word;
}

void IEXIDevice::ImmReadWrite(u32&, u32)
{
}

void IEXIDevice::DMAWrite(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();
  for (u32 offset = 0; offset < size; ++offset)
  {
    u8 outgoing = memory.Read_U8(address + offset);
    TransferByte(outgoing);
  }
}

void IEXIDevice::DMARead(u32 address, u32 size)
{
  auto& memory = m_system.GetMemory();
  for (u32 offset = 0; offset < size; ++offset)
  {
    u8 incoming = 0;
    TransferByte(incoming);
    memory.Write_U8(incoming, address + offset);
  }
}

bool IEXIDevice::UseDelayedTransferCompletion() const
{
  return false;
}
bool IEXIDevice::IsPresent() const
{
  return false;
}
void IEXIDevice::SetCS(int)
{
}
void IEXIDevice::DoState(PointerWrap&)
{
}
bool IEXIDevice::IsInterruptSet()
{
  return false;
}
void IEXIDevice::TransferByte(u8&)
{
}
}  // namespace ExpansionInterface

namespace Memory
{
void MemoryManager::Write_U8(u8 var, u32 address)
{
  u8* p = GetPointerForRange(address, 1);
  if (p)
    *p = var;
}
}  // namespace Memory

namespace SerialInterface
{
SIDevices SerialInterfaceManager::GetDeviceType(int channel) const
{
  return static_cast<SIDevices>(w43::g_si_types[channel & 3]);
}

ISIDevice::ISIDevice(Core::System& system, SIDevices device_type, int device_number)
    : m_system(system), m_device_number(device_number), m_device_type(device_type)
{
}
ISIDevice::~ISIDevice() = default;
int ISIDevice::RunBuffer(u8*, int)
{
  return 0;
}
int ISIDevice::TransferInterval()
{
  return 0;
}
void ISIDevice::DoState(PointerWrap&)
{
}
void ISIDevice::OnEvent(u64, s64)
{
}
int ISIDevice::CreateStatusResponse(u32 si_device_id, u8* buffer)
{
  buffer[0] = static_cast<u8>(si_device_id >> 24);
  buffer[1] = static_cast<u8>(si_device_id >> 16);
  buffer[2] = static_cast<u8>(si_device_id >> 8);
  return 3;
}
}  // namespace SerialInterface

namespace SystemTimers
{
u32 SystemTimersManager::GetTicksPerSecond() const
{
  return 486000000;
}
}  // namespace SystemTimers

// ---- Pads, OSD, messages, HLE, SConfig -------------------------------------------------------

namespace Pad
{
GCPadStatus GetStatus(int pad_num)
{
  return w43::g_pads[pad_num & 3];
}
}  // namespace Pad

namespace OSD
{
void AddMessage(std::string message, u32 ms, u32 argb,
                const VideoCommon::CustomTextureData::ArraySlice::Level* icon)
{
  B(fmt::format("OSD.AddMessage ms={} argb={:08x} icon={} {}", ms, argb, icon != nullptr,
                message));
}
}  // namespace OSD

namespace Core
{
void DisplayMessage(std::string message, int time_in_ms)
{
  B(fmt::format("Core.DisplayMessage {} {}", time_in_ms, message));
}
}  // namespace Core

namespace HLE
{
void Patch(Core::System&, u32 pc, std::string_view func_name)
{
  B(fmt::format("HLE.Patch {:08x} {}", pc, func_name));
}
}  // namespace HLE

namespace
{
alignas(64) unsigned char g_sconfig_storage[sizeof(SConfig)];
}
SConfig* SConfig::s_instance = reinterpret_cast<SConfig*>(g_sconfig_storage);
const std::string SConfig::GetGameID() const
{
  return w43::g_game_id;
}

// ---- Config infos (defaults as in the base tree's MainSettings.cpp) ------------------------

namespace Config
{
static std::string HarnessDefaultTriforceRedirections()
{
  static constexpr std::string_view redirections[] = {
      "192.168.29.150:5000-5008=127.0.0.1:5000-5008 MarioKart Seat #1 Static",
      "192.168.29.151:5000-5008=127.0.0.1:5010-5018 MarioKart Seat #2 Static",
      "192.168.29.152:5000-5008=127.0.0.1:5020-5028 MarioKart Seat #3 Static",
      "192.168.29.153:5000-5008=127.0.0.1:5030-5038 MarioKart Seat #4 Static",
      "192.168.29.150=127.0.0.1:50000-50999 MarioKart Seat #1 Ephemeral",
      "192.168.29.151=127.0.0.1:51000-51999 MarioKart Seat #2 Ephemeral",
      "192.168.29.152=127.0.0.1:52000-52999 MarioKart Seat #3 Ephemeral",
      "192.168.29.153=127.0.0.1:53000-53999 MarioKart Seat #4 Ephemeral",
      "192.168.29.104-107=127.0.0.1 MarioKart namcam2",
      "192.168.11.0/24=127.0.0.1 CyCraft",
      "192.168.13.0/24=127.0.0.1 The Key of Avalon",
      "192.168.150.0/24=127.0.0.1 ALL.Net",
  };
  std::string out;
  for (const auto r : redirections)
  {
    if (!out.empty())
      out += ',';
    out += r;
  }
  return out;
}

const Info<std::string> MAIN_AGP_CART_A_PATH{{System::Main, "Core", "AgpCartAPath"}, ""};
const Info<std::string> MAIN_AGP_CART_B_PATH{{System::Main, "Core", "AgpCartBPath"}, ""};
const Info<std::string> MAIN_MODEM_TAPSERVER_DESTINATION{
    {System::Main, "Core", "MODEM_TAPSERVER_DESTINATION"}, "/tmp/dolphin-modem-tap"};
const Info<std::string> MAIN_TRIFORCE_IP_REDIRECTIONS{
    {System::Main, "Core", "TriforceIPRedirections"}, HarnessDefaultTriforceRedirections()};

const Info<std::string>& GetInfoForAGPCartPath(ExpansionInterface::Slot slot)
{
  return slot == ExpansionInterface::Slot::B ? MAIN_AGP_CART_B_PATH : MAIN_AGP_CART_A_PATH;
}
}  // namespace Config

// ================================================================================================
// DiscIO
// ================================================================================================

namespace
{
class FakeBlob final : public DiscIO::BlobReader
{
public:
  explicit FakeBlob(u64 size) : m_size(size) {}
  DiscIO::BlobType GetBlobType() const override { return DiscIO::BlobType::PLAIN; }
  std::unique_ptr<DiscIO::BlobReader> CopyReader() const override
  {
    B("BLOB.CopyReader");
    return std::make_unique<FakeBlob>(m_size);
  }
  u64 GetRawSize() const override { return m_size; }
  u64 GetDataSize() const override { return m_size; }
  DiscIO::DataSizeType GetDataSizeType() const override { return DiscIO::DataSizeType::Accurate; }
  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }
  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }
  bool Read(u64 offset, u64 size, u8* out_ptr) override
  {
    const bool ok = offset <= m_size && size <= m_size - offset;
    B(fmt::format("BLOB.Read {:x} {:x} ok={}", offset, size, ok));
    if (!ok)
      return false;
    for (u64 i = 0; i < size; ++i)
      out_ptr[i] = static_cast<u8>(w43::Mix(offset + i) >> 13);
    return true;
  }

private:
  u64 m_size;
};

class FakeVolumeImpl final : public DiscIO::Volume
{
public:
  explicit FakeVolumeImpl(u64 size) : m_blob(size) {}
  bool Read(u64, u64, u8*, const DiscIO::Partition&) const override { return false; }
  const DiscIO::FileSystem* GetFileSystem(const DiscIO::Partition&) const override
  {
    return nullptr;
  }
  const IOS::ES::TicketReader& GetTicket(const DiscIO::Partition&) const override
  {
    std::abort();
  }
  const IOS::ES::TMDReader& GetTMD(const DiscIO::Partition&) const override { std::abort(); }
  const std::vector<u8>& GetCertificateChain(const DiscIO::Partition&) const override
  {
    std::abort();
  }
  IOS::ES::TicketReader GetTicketWithFixedCommonKey() const override { std::abort(); }
  std::string GetGameID(const DiscIO::Partition&) const override { return "W43HRN"; }
  std::string GetGameTDBID(const DiscIO::Partition&) const override { return "W43HRN"; }
  std::string GetMakerID(const DiscIO::Partition&) const override { return "W4"; }
  std::optional<u16> GetRevision(const DiscIO::Partition&) const override { return 0; }
  std::string GetInternalName(const DiscIO::Partition&) const override { return "w43"; }
  std::vector<u32> GetBanner(u32*, u32*) const override { return {}; }
  std::string GetApploaderDate(const DiscIO::Partition&) const override { return {}; }
  DiscIO::Platform GetVolumeType() const override { return DiscIO::Platform::GameCubeDisc; }
  bool IsDatelDisc() const override { return false; }
  bool IsNKit() const override { return false; }
  DiscIO::Region GetRegion() const override { return DiscIO::Region::NTSC_J; }
  DiscIO::Country GetCountry(const DiscIO::Partition&) const override
  {
    return DiscIO::Country::Japan;
  }
  u32 GetSimulatedMemorySize() const override { return 0; }
  DiscIO::BlobType GetBlobType() const override { return DiscIO::BlobType::PLAIN; }
  u64 GetDataSize() const override { return m_blob.GetDataSize(); }
  DiscIO::DataSizeType GetDataSizeType() const override { return DiscIO::DataSizeType::Accurate; }
  u64 GetRawSize() const override { return m_blob.GetRawSize(); }
  const DiscIO::BlobReader& GetBlobReader() const override { return m_blob; }
  std::array<u8, 20> GetSyncHash() const override { return {}; }

private:
  FakeBlob m_blob;
};

std::unique_ptr<FakeVolumeImpl> g_volume;
}  // namespace

namespace DiscIO
{
std::unique_ptr<BlobReader> CreateCachedBlobReader(std::unique_ptr<BlobReader> reader)
{
  B("DISC.CreateCachedBlobReader");
  return reader;
}

// Referenced by Volume's inline defaults when an unoptimised build emits them.
const IOS::ES::TicketReader Volume::INVALID_TICKET{};
const IOS::ES::TMDReader Volume::INVALID_TMD{};
const std::vector<u8> Volume::INVALID_CERT_CHAIN{};
}  // namespace DiscIO

const DiscIO::Volume& w43::FakeVolume(u64 data_size)
{
  g_volume = std::make_unique<FakeVolumeImpl>(data_size);
  return *g_volume;
}

// ================================================================================================
// Tapserver connection (used by the original EXI/Modem/TAPServerModem.cpp)
// ================================================================================================

namespace ExpansionInterface
{
namespace
{
u64 TapOutcome(const char* kind)
{
  const u64 count = w43::g_tap_counters[kind]++;
  return w43::Mix(w43::g_tap_seed ^ w43::FnvStr(kind) ^ (count * 0x10001));
}
}  // namespace

TAPServerConnection::TAPServerConnection(std::string destination, RecvCallback recv_cb,
                                         std::size_t max_frame_size)
    : m_destination(std::move(destination)), m_on_frame(std::move(recv_cb)),
      m_max_frame_size(max_frame_size)
{
  B(fmt::format("TAP.ctor dest={} max={}", m_destination, m_max_frame_size));
  w43::g_tap_callback = m_on_frame;
}

bool TAPServerConnection::Activate()
{
  if (m_fd >= 0)
  {
    B("TAP.Activate (already) = true");
    return true;
  }
  const bool ok = TapOutcome("activate") % 4 != 0;
  if (ok)
    m_fd = 1;
  B(fmt::format("TAP.Activate = {}", ok));
  return ok;
}

void TAPServerConnection::Deactivate()
{
  B("TAP.Deactivate");
  m_fd = -1;
}

bool TAPServerConnection::IsActivated()
{
  B(fmt::format("TAP.IsActivated = {}", m_fd >= 0));
  return m_fd >= 0;
}

bool TAPServerConnection::RecvInit()
{
  B("TAP.RecvInit");
  return true;
}

void TAPServerConnection::RecvStart()
{
  B("TAP.RecvStart");
}

void TAPServerConnection::RecvStop()
{
  B("TAP.RecvStop");
}

bool TAPServerConnection::SendAndRemoveAllHDLCFrames(std::string* send_buf)
{
  B(fmt::format("TAP.SendAll {}",
                w43::Hex({reinterpret_cast<const u8*>(send_buf->data()), send_buf->size()})));
  if (TapOutcome("send") % 8 == 0)
  {
    B("TAP.SendAll = false");
    return false;
  }
  // Harness model: every complete 7E ... 7E frame at the front goes out, with anything before it.
  while (true)
  {
    const auto start = send_buf->find('\x7e');
    if (start == std::string::npos)
      break;
    const auto end = send_buf->find('\x7e', start + 1);
    if (end == std::string::npos)
      break;
    const std::string frame = send_buf->substr(start, end - start + 1);
    B(fmt::format("TAP.frame {}",
                  w43::Hex({reinterpret_cast<const u8*>(frame.data()), frame.size()})));
    send_buf->erase(0, end + 1);
  }
  B("TAP.SendAll = true");
  return true;
}

bool TAPServerConnection::SendFrame(const u8* frame, u32 size)
{
  B(fmt::format("TAP.SendFrame {}", w43::Hex({frame, size})));
  return true;
}
}  // namespace ExpansionInterface

// ================================================================================================
// Sockets (linked with -Wl,--wrap=<name>). Fake descriptors start at 1000; anything else goes to
// the real call (only regular files are expected there).
// ================================================================================================

extern "C" {
int __real_close(int fd);
int __real_ioctl(int fd, unsigned long request, ...);
}

namespace
{
struct FakeSocket
{
  int domain;
  int type;
};
std::map<int, FakeSocket> g_fake_sockets;
int g_next_fake_fd = 1000;

bool IsFake(int fd)
{
  return g_fake_sockets.contains(fd);
}

u64 SockOutcome(const char* kind)
{
  const u64 count = w43::g_socket_counters[kind]++;
  return w43::Mix(w43::g_socket_seed ^ w43::FnvStr(kind) ^ (count * 0x10001));
}

// `addr` must hold a full sockaddr_in.
std::string AddrTextWithPort(const sockaddr* addr, socklen_t len, const std::string& port_text)
{
  sockaddr_in in;
  std::memcpy(&in, addr, sizeof(in));
  const u8* ip = reinterpret_cast<const u8*>(&in.sin_addr);
  return fmt::format("fam={} {}.{}.{}.{}:{} len={}", in.sin_family, ip[0], ip[1], ip[2], ip[3],
                     port_text, len);
}

std::string AddrText(const sockaddr* addr, socklen_t len)
{
  if (addr == nullptr)
    return "null";
  if (len < static_cast<socklen_t>(sizeof(sockaddr_in)))
    return fmt::format("len={}", len);
  sockaddr_in in;
  std::memcpy(&in, addr, sizeof(in));
  const u16 port = ntohs(in.sin_port);
  // Ports from the IANA dynamic range are chosen at random by the code under test.
  return AddrTextWithPort(addr, len, port >= 49152 ? std::string("EPH") : fmt::format("{}", port));
}

int Fail(int err)
{
  errno = err;
  return -1;
}

void ResetFakeSockets()
{
  g_fake_sockets.clear();
  g_next_fake_fd = 1000;
}
}  // namespace

namespace w43
{
// Called by the harness before each case (fresh process anyway, but keeps numbering stable).
void ResetSockets()
{
  ResetFakeSockets();
}
}  // namespace w43

extern "C" {

int __wrap_socket(int domain, int type, int protocol)
{
  if (SockOutcome("socket") % 10 == 0)
  {
    B(fmt::format("SOCK.socket {} {} {} = -1 EMFILE", domain, type, protocol));
    return Fail(EMFILE);
  }
  const int fd = g_next_fake_fd++;
  g_fake_sockets[fd] = {domain, type};
  B(fmt::format("SOCK.socket {} {} {} = {}", domain, type, protocol, fd));
  return fd;
}

int __wrap_close(int fd)
{
  if (!IsFake(fd))
  {
    if (fd >= 1000 || fd < 0)
    {
      B(fmt::format("SOCK.close {} = -1 EBADF", fd));
      return Fail(EBADF);
    }
    return __real_close(fd);
  }
  g_fake_sockets.erase(fd);
  B(fmt::format("SOCK.close {} = 0", fd));
  return 0;
}

int __wrap_bind(int fd, const sockaddr* addr, socklen_t len)
{
  // The port is filled in later (see HoldBind): masked when the device logs it as ephemeral.
  std::string port;
  std::string held = AddrText(addr, len);
  if (addr != nullptr && len >= static_cast<socklen_t>(sizeof(sockaddr_in)))
  {
    sockaddr_in in;
    std::memcpy(&in, addr, sizeof(in));
    const u16 p = ntohs(in.sin_port);
    port = p >= 49152 ? std::string("EPH") : fmt::format("{}", p);
    held = AddrTextWithPort(addr, len, "{PORT}");
  }
  if (!IsFake(fd))
  {
    w43::HoldBind(fmt::format("SOCK.bind {} {} = -1 EBADF", fd, held), port);
    return Fail(EBADF);
  }
  if (SockOutcome("bind") % 6 == 0)
  {
    w43::HoldBind(fmt::format("SOCK.bind {} {} = -1 EADDRINUSE", fd, held), port);
    return Fail(EADDRINUSE);
  }
  w43::HoldBind(fmt::format("SOCK.bind {} {} = 0", fd, held), port);
  return 0;
}

int __wrap_connect(int fd, const sockaddr* addr, socklen_t len)
{
  const std::string text = AddrText(addr, len);
  if (!IsFake(fd))
  {
    B(fmt::format("SOCK.connect {} {} = -1 EBADF", fd, text));
    return Fail(EBADF);
  }
  const u64 o = SockOutcome("connect") % 10;
  if (o < 3)
  {
    B(fmt::format("SOCK.connect {} {} = 0", fd, text));
    return 0;
  }
  const int err = o < 8 ? EINPROGRESS : ECONNREFUSED;
  B(fmt::format("SOCK.connect {} {} = -1 {}", fd, text, err));
  return Fail(err);
}

int __wrap_listen(int fd, int backlog)
{
  if (!IsFake(fd))
  {
    B(fmt::format("SOCK.listen {} {} = -1 EBADF", fd, backlog));
    return Fail(EBADF);
  }
  if (SockOutcome("listen") % 10 == 0)
  {
    B(fmt::format("SOCK.listen {} {} = -1 EINVAL", fd, backlog));
    return Fail(EINVAL);
  }
  B(fmt::format("SOCK.listen {} {} = 0", fd, backlog));
  return 0;
}

int __wrap_accept(int fd, sockaddr* addr, socklen_t* len)
{
  if (!IsFake(fd))
  {
    B(fmt::format("SOCK.accept {} = -1 EBADF", fd));
    return Fail(EBADF);
  }
  const u64 o = SockOutcome("accept");
  if (o % 10 >= 7)
  {
    B(fmt::format("SOCK.accept {} = -1 EAGAIN", fd));
    return Fail(EAGAIN);
  }
  const int new_fd = g_next_fake_fd++;
  g_fake_sockets[new_fd] = g_fake_sockets[fd];
  static constexpr u8 ips[][4] = {{127, 0, 0, 1}, {10, 0, 0, 2}, {192, 168, 29, 150}};
  static constexpr u16 ports[] = {5000, 5012, 5031, 50123, 7777};
  sockaddr_in in{};
  in.sin_family = AF_INET;
  std::memcpy(&in.sin_addr, ips[(o >> 8) % 3], 4);
  in.sin_port = htons(ports[(o >> 16) % 5]);
  if (addr != nullptr && len != nullptr)
  {
    const socklen_t n = std::min<socklen_t>(*len, sizeof(in));
    std::memcpy(addr, &in, n);
    *len = sizeof(in);
  }
  B(fmt::format("SOCK.accept {} = {} {}", fd, new_fd,
                AddrText(reinterpret_cast<sockaddr*>(&in), sizeof(in))));
  return new_fd;
}

ssize_t __wrap_recv(int fd, void* buf, size_t len, int flags)
{
  if (!IsFake(fd))
  {
    B(fmt::format("SOCK.recv {} {} {} = -1 EBADF", fd, len, flags));
    return Fail(EBADF);
  }
  const u64 o = SockOutcome("recv");
  if (len == 0 || o % 20 < 3)
  {
    B(fmt::format("SOCK.recv {} {} {} = 0", fd, len, flags));
    return 0;
  }
  if (o % 20 < 8)
  {
    B(fmt::format("SOCK.recv {} {} {} = -1 EAGAIN", fd, len, flags));
    return Fail(EAGAIN);
  }
  const size_t n = std::min<size_t>(len, 1 + (o >> 8) % 64);
  auto* out = static_cast<u8*>(buf);
  for (size_t i = 0; i < n; ++i)
    out[i] = static_cast<u8>(w43::Mix(o + i) >> 7);
  B(fmt::format("SOCK.recv {} {} {} = {} {}", fd, len, flags, n, w43::Hex({out, n})));
  return static_cast<ssize_t>(n);
}

ssize_t __wrap_send(int fd, const void* buf, size_t len, int flags)
{
  const auto* in = static_cast<const u8*>(buf);
  const std::string data = fmt::format(
      "{:016x}", w43::Fnv({in, len}));
  if (!IsFake(fd))
  {
    B(fmt::format("SOCK.send {} {} {} {} = -1 EBADF", fd, len, flags, data));
    return Fail(EBADF);
  }
  const u64 o = SockOutcome("send") % 20;
  if (o < 3)
  {
    B(fmt::format("SOCK.send {} {} {} {} = -1 EPIPE", fd, len, flags, data));
    return Fail(EPIPE);
  }
  const size_t n = o < 6 ? len / 2 : len;
  B(fmt::format("SOCK.send {} {} {} {} = {}", fd, len, flags, data, n));
  return static_cast<ssize_t>(n);
}

int __wrap_setsockopt(int fd, int level, int name, const void* value, socklen_t len)
{
  const std::string text =
      fmt::format("{} {} {} {}", fd, level, name,
                  w43::Hex({static_cast<const u8*>(value), std::min<socklen_t>(len, 16)}));
  if (!IsFake(fd))
  {
    B(fmt::format("SOCK.setsockopt {} = -1 EBADF", text));
    return Fail(EBADF);
  }
  if (level == SOL_SOCKET && (name == SO_SNDTIMEO || name == SO_RCVTIMEO) &&
      len < static_cast<socklen_t>(sizeof(timeval)))
  {
    B(fmt::format("SOCK.setsockopt {} = -1 EINVAL", text));
    return Fail(EINVAL);
  }
  if (SockOutcome("setsockopt") % 10 == 0)
  {
    B(fmt::format("SOCK.setsockopt {} = -1 ENOPROTOOPT", text));
    return Fail(ENOPROTOOPT);
  }
  B(fmt::format("SOCK.setsockopt {} = 0", text));
  return 0;
}

int __wrap_getsockopt(int fd, int level, int name, void* value, socklen_t* len)
{
  if (!IsFake(fd))
  {
    B(fmt::format("SOCK.getsockopt {} {} {} = -1 EBADF", fd, level, name));
    return Fail(EBADF);
  }
  const u64 o = SockOutcome("getsockopt") % 20;
  if (o < 3)
  {
    B(fmt::format("SOCK.getsockopt {} {} {} = -1 ENOTSOCK", fd, level, name));
    return Fail(ENOTSOCK);
  }
  const int so_error = o < 8 ? ECONNREFUSED : 0;
  if (value != nullptr && len != nullptr && *len >= static_cast<socklen_t>(sizeof(int)))
  {
    std::memcpy(value, &so_error, sizeof(int));
    *len = sizeof(int);
  }
  B(fmt::format("SOCK.getsockopt {} {} {} = 0 value={}", fd, level, name, so_error));
  return 0;
}

int __wrap_poll(pollfd* fds, nfds_t nfds, int timeout)
{
  std::string text = fmt::format("n={} timeout={}", nfds, timeout);
  for (nfds_t i = 0; i < nfds; ++i)
    text += fmt::format(" [{} {:x}]", fds[i].fd, fds[i].events);
  const u64 o = SockOutcome("poll");
  if (o % 40 == 0)
  {
    B("SOCK.poll " + text + " = -1 EINTR");
    return Fail(EINTR);
  }
  if (o % 40 == 1)
  {
    B("SOCK.poll " + text + " = -1 ENOMEM");
    return Fail(ENOMEM);
  }
  int ready = 0;
  std::string result;
  for (nfds_t i = 0; i < nfds; ++i)
  {
    const u64 r = w43::Mix(o ^ (i * 0x1234567));
    short revents = 0;
    if (fds[i].fd < 0)
      revents = 0;
    else if (!IsFake(fds[i].fd))
      revents = POLLNVAL;
    else
    {
      revents = static_cast<short>(fds[i].events & static_cast<short>(r));
      if ((r >> 20) % 12 == 0)
        revents |= POLLERR;
    }
    fds[i].revents = revents;
    if (revents != 0)
      ++ready;
    result += fmt::format(" {:x}", revents);
  }
  B("SOCK.poll " + text + fmt::format(" = {}", ready) + result);
  return ready;
}

int __wrap_ioctl(int fd, unsigned long request, ...)
{
  va_list args;
  va_start(args, request);
  void* arg = va_arg(args, void*);
  va_end(args);
  if (!IsFake(fd))
  {
    if (fd >= 1000 || fd < 0)
    {
      B(fmt::format("SOCK.ioctl {} {:x} = -1 EBADF", fd, request));
      return Fail(EBADF);
    }
    return __real_ioctl(fd, request, arg);
  }
  int value = 0;
  if (request == FIONBIO && arg != nullptr)
    std::memcpy(&value, arg, sizeof(int));
  B(fmt::format("SOCK.ioctl {} {:x} {} = 0", fd, request, value));
  return 0;
}

}  // extern "C"
