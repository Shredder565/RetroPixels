// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/HW/SI/SI_Device.h"

#include <cstring>
#include <memory>
#include <span>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/Swap.h"
#include "Core/HW/SI/SI_DeviceAMBaseboard.h"
#include "Core/HW/SI/SI_DeviceDanceMat.h"
#include "Core/HW/SI/SI_DeviceGBA.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/SI/SI_DeviceGBAEmu.h"
#endif
#include "Core/HW/SI/SI_DeviceGCAdapter.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/SI/SI_DeviceGCSteeringWheel.h"
#include "Core/HW/SI/SI_DeviceKeyboard.h"
#include "Core/HW/SI/SI_DeviceNull.h"
#include "Core/HW/SystemTimers.h"
#include "Core/System.h"

namespace SerialInterface
{
ISIDevice::ISIDevice(Core::System& system, SIDevices device_type, int device_number)
    : m_system(system), m_device_number(device_number), m_device_type(device_type)
{
}

ISIDevice::~ISIDevice() = default;

int ISIDevice::GetDeviceNumber() const
{
  return m_device_number;
}

SIDevices ISIDevice::GetDeviceType() const
{
  return m_device_type;
}

int ISIDevice::RunBuffer(u8* buffer, int request_length)
{
  // The base version only traces the request in debug builds.
#ifdef _DEBUG
  const std::span<const u8> request(buffer, static_cast<std::size_t>(request_length));
  DEBUG_LOG_FMT(SERIALINTERFACE, "Port {} request ({} bytes): {:02x}", m_device_number,
                request_length, fmt::join(request, " "));
#else
  (void)buffer;
  (void)request_length;
#endif
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

int SIDevice_GetGBATransferTime(const SystemTimers::SystemTimersManager& timers,
                                EBufferCommands cmd)
{
  // Joybus to a GBA: the GameCube side runs at 200 kbit/s, the GBA side at
  // 250 kbit/s, and both directions end with a stop bit (6.5 us + 14 us).
  constexpr u64 GC_BITRATE = 200000;
  constexpr u64 GBA_BITRATE = 250000;
  constexpr u64 STOP_BITS_NS = 6500 + 14000;

  u64 sent = 1;      // bytes the GameCube sends
  u64 received = 0;  // bytes the GBA answers with
  switch (cmd)
  {
  case EBufferCommands::CMD_RESET:
  case EBufferCommands::CMD_STATUS:
    received = 3;
    break;
  case EBufferCommands::CMD_READ_GBA:
    received = 5;
    break;
  case EBufferCommands::CMD_WRITE_GBA:
    sent = 5;
    received = 1;
    break;
  default:
    break;
  }

  const u64 tps = timers.GetTicksPerSecond();
  const u64 gba_side = received * 8 * tps / GBA_BITRATE;
  const u64 gc_side = sent * 8 * tps / GC_BITRATE;
  const u64 stop_bits = STOP_BITS_NS * tps / 1000000000;
  return static_cast<int>(gba_side + gc_side + stop_bits);
}

// True for devices built on the standard controller class, i.e. the ones whose
// input can come from a movie or NetPlay instead of a local device.
bool SIDevice_IsGCController(SIDevices type)
{
  return type == SIDEVICE_GC_CONTROLLER || type == SIDEVICE_WIIU_ADAPTER ||
         type == SIDEVICE_GC_TARUKONGA || type == SIDEVICE_DANCEMAT ||
         type == SIDEVICE_GC_STEERING || type == SIDEVICE_AM_BASEBOARD;
}

std::unique_ptr<ISIDevice> SIDevice_Create(Core::System& system, const SIDevices device,
                                           const int port_number)
{
  switch (device)
  {
  case SIDEVICE_GC_CONTROLLER:
    return std::make_unique<CSIDevice_GCController>(system, device, port_number);
  case SIDEVICE_WIIU_ADAPTER:
    return std::make_unique<CSIDevice_GCAdapter>(system, device, port_number);
  case SIDEVICE_DANCEMAT:
    return std::make_unique<CSIDevice_DanceMat>(system, device, port_number);
  case SIDEVICE_GC_STEERING:
    return std::make_unique<CSIDevice_GCSteeringWheel>(system, device, port_number);
  case SIDEVICE_GC_TARUKONGA:
    return std::make_unique<CSIDevice_TaruKonga>(system, device, port_number);
  case SIDEVICE_GC_GBA:
    return std::make_unique<CSIDevice_GBA>(system, device, port_number);
  case SIDEVICE_GC_GBA_EMULATED:
#ifdef HAS_LIBMGBA
    return std::make_unique<CSIDevice_GBAEmu>(system, device, port_number);
#else
    PanicAlertFmtT("Error: This build does not support emulated GBA controllers");
    return std::make_unique<CSIDevice_Null>(system, device, port_number);
#endif
  case SIDEVICE_GC_KEYBOARD:
    return std::make_unique<CSIDevice_Keyboard>(system, device, port_number);
  case SIDEVICE_AM_BASEBOARD:
    return std::make_unique<CSIDevice_AMBaseboard>(system, device, port_number);
  default:
    return std::make_unique<CSIDevice_Null>(system, device, port_number);
  }
}
}  // namespace SerialInterface
