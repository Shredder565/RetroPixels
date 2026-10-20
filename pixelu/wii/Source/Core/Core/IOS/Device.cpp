// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/IOS/Device.h"

#include <algorithm>
#include <string_view>
#include <utility>

#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SystemTimers.h"
#include "Core/IOS/IOS.h"
#include "Core/System.h"

namespace IOS::HLE
{
namespace
{
// Offsets of the argument words in an IPC request block.
constexpr u32 OFFSET_COMMAND = 0x00;
constexpr u32 OFFSET_FD = 0x08;
constexpr u32 OFFSET_ARG0 = 0x0c;
constexpr u32 OFFSET_ARG1 = 0x10;
constexpr u32 OFFSET_ARG2 = 0x14;
constexpr u32 OFFSET_ARG3 = 0x18;
constexpr u32 OFFSET_ARG4 = 0x1c;

std::string_view CommandName(IPCCommandType command)
{
  switch (command)
  {
  case IPC_CMD_READ:
    return "Read";
  case IPC_CMD_WRITE:
    return "Write";
  case IPC_CMD_SEEK:
    return "Seek";
  case IPC_CMD_IOCTL:
    return "IOCtl";
  case IPC_CMD_IOCTLV:
    return "IOCtlV";
  default:
    return "an unknown command";
  }
}

std::string HexDumpRange(Memory::MemoryManager& memory, u32 address, u32 size)
{
  return HexDump(memory.GetPointerForRange(address, size), size);
}
}  // namespace

Request::Request(Core::System& system, const u32 address_) : address(address_)
{
  const auto& memory = system.GetMemory();
  command = static_cast<IPCCommandType>(memory.Read_U32(address + OFFSET_COMMAND));
  fd = memory.Read_U32(address + OFFSET_FD);
}

OpenRequest::OpenRequest(Core::System& system, const u32 address_) : Request(system, address_)
{
  auto& memory = system.GetMemory();
  path = memory.GetString(memory.Read_U32(address + OFFSET_ARG0));
  flags = static_cast<OpenMode>(memory.Read_U32(address + OFFSET_ARG1));

  // Stamp the request with the credentials of the PPC side, when a kernel is running.
  if (const EmulationKernel* kernel = system.GetIOS(); kernel != nullptr)
  {
    uid = kernel->GetUidForPPC();
    gid = kernel->GetGidForPPC();
  }
}

ReadWriteRequest::ReadWriteRequest(Core::System& system, const u32 address_)
    : Request(system, address_)
{
  const auto& memory = system.GetMemory();
  buffer = memory.Read_U32(address + OFFSET_ARG0);
  size = memory.Read_U32(address + OFFSET_ARG1);
}

SeekRequest::SeekRequest(Core::System& system, const u32 address_) : Request(system, address_)
{
  const auto& memory = system.GetMemory();
  offset = memory.Read_U32(address + OFFSET_ARG0);
  mode = static_cast<SeekMode>(memory.Read_U32(address + OFFSET_ARG1));
}

IOCtlRequest::IOCtlRequest(Core::System& system, const u32 address_) : Request(system, address_)
{
  const auto& memory = system.GetMemory();
  request = memory.Read_U32(address + OFFSET_ARG0);
  buffer_in = memory.Read_U32(address + OFFSET_ARG1);
  buffer_in_size = memory.Read_U32(address + OFFSET_ARG2);
  buffer_out = memory.Read_U32(address + OFFSET_ARG3);
  buffer_out_size = memory.Read_U32(address + OFFSET_ARG4);
}

IOCtlVRequest::IOCtlVRequest(Core::System& system, const u32 address_) : Request(system, address_)
{
  const auto& memory = system.GetMemory();
  request = memory.Read_U32(address + OFFSET_ARG0);
  const u32 num_in = memory.Read_U32(address + OFFSET_ARG1);
  const u32 num_io = memory.Read_U32(address + OFFSET_ARG2);
  const u32 table = memory.Read_U32(address + OFFSET_ARG3);

  // The table is a packed array of (address, size) pairs: the in vectors, then the io vectors.
  // The count is a 32-bit sum, as on hardware.
  const u32 total = num_in + num_io;
  u32 entry = table;
  for (u32 index = 0; index < total; ++index, entry += 8)
  {
    const IOVector vector{memory.Read_U32(entry), memory.Read_U32(entry + 4)};
    (index < num_in ? in_vectors : io_vectors).push_back(vector);
  }
}

const IOCtlVRequest::IOVector* IOCtlVRequest::GetVector(size_t index) const
{
  if (index < in_vectors.size())
    return &in_vectors[index];
  index -= in_vectors.size();
  if (index < io_vectors.size())
    return &io_vectors[index];
  return nullptr;
}

bool IOCtlVRequest::HasNumberOfValidVectors(const size_t in_count, const size_t io_count) const
{
  if (in_vectors.size() != in_count || io_vectors.size() != io_count)
    return false;

  // A vector is usable when it is empty or points somewhere.
  const auto usable = [](const IOVector& v) { return v.address != 0 || v.size == 0; };
  return std::ranges::all_of(in_vectors, usable) && std::ranges::all_of(io_vectors, usable);
}

void IOCtlRequest::Log(std::string_view device_name, Common::Log::LogType type,
                       Common::Log::LogLevel verbosity) const
{
  GENERIC_LOG_FMT(type, verbosity, "{}: ioctl {:#x} on fd {} (in {:#x} bytes, out {:#x} bytes)",
                  device_name, request, fd, buffer_in_size, buffer_out_size);
}

void IOCtlRequest::Dump(Core::System& system, const std::string& description,
                        Common::Log::LogType type, Common::Log::LogLevel level) const
{
  auto& memory = system.GetMemory();
  Log("----- " + description, type, level);
  GENERIC_LOG_FMT(type, level, "input:\n{}", HexDumpRange(memory, buffer_in, buffer_in_size));
  GENERIC_LOG_FMT(type, level, "output:\n{}", HexDumpRange(memory, buffer_out, buffer_out_size));
}

void IOCtlRequest::DumpUnknown(Core::System& system, const std::string& description,
                               Common::Log::LogType type, Common::Log::LogLevel level) const
{
  Dump(system, "unhandled ioctl in " + description, type, level);
}

void IOCtlVRequest::Dump(Core::System& system, std::string_view description,
                         Common::Log::LogType type, Common::Log::LogLevel level) const
{
  auto& memory = system.GetMemory();
  GENERIC_LOG_FMT(type, level, "----- {}: ioctlv {:#x} on fd {} with {} in / {} io vectors",
                  description, request, fd, in_vectors.size(), io_vectors.size());

  for (size_t n = 0; n < in_vectors.size(); ++n)
  {
    const IOVector& v = in_vectors[n];
    GENERIC_LOG_FMT(type, level, "in #{} ({:#x} bytes):\n{}", n, v.size,
                    HexDumpRange(memory, v.address, v.size));
  }
  for (size_t n = 0; n < io_vectors.size(); ++n)
    GENERIC_LOG_FMT(type, level, "io #{} ({:#x} bytes)", n, io_vectors[n].size);
}

void IOCtlVRequest::DumpUnknown(Core::System& system, const std::string& description,
                                Common::Log::LogType type, Common::Log::LogLevel level) const
{
  Dump(system, "unhandled ioctlv in " + description, type, level);
}

Device::Device(Kernel& ios, std::string device_name, const DeviceType type)
    : m_ios(ios), m_name(std::move(device_name)), m_device_type(type)
{
}

void Device::DoState(PointerWrap& p)
{
  p.Do(m_name);
  p.Do(m_device_type);
  p.Do(m_is_active);
}

std::optional<IPCReply> Device::Open(const OpenRequest&)
{
  m_is_active = true;
  return IPCReply{IPC_SUCCESS};
}

std::optional<IPCReply> Device::Close(u32)
{
  m_is_active = false;
  return IPCReply{IPC_SUCCESS};
}

std::optional<IPCReply> Device::Unsupported(const Request& request)
{
  WARN_LOG_FMT(IOS, "{}: {} is not implemented by this device", m_name,
               CommandName(request.command));
  return IPCReply{IPC_EINVAL};
}
}  // namespace IOS::HLE
