// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include "Common/CommonTypes.h"

class PointerWrap;
namespace Core
{
class System;
}
namespace SystemTimers
{
class SystemTimersManager;
}

namespace SerialInterface
{
// Error flags a port can report.
constexpr u32 SI_ERROR_NO_RESPONSE = 0x0008;  // nothing plugged in
constexpr u32 SI_ERROR_UNKNOWN = 0x0040;      // device not recognised
constexpr u32 SI_ERROR_BUSY = 0x0080;         // detection still running

// Device type word (the top three bytes are the status reply).
constexpr u32 SI_TYPE_MASK = 0x18000000;
constexpr u32 SI_TYPE_GC = 0x08000000;

// GameCube controller subtype flags.
constexpr u32 SI_GC_NOMOTOR = 0x20000000;  // no rumble motor
constexpr u32 SI_GC_STANDARD = 0x01000000;

// Device IDs returned by the status command.
enum TSIDevices : u32
{
  SI_NONE = SI_ERROR_NO_RESPONSE,
  SI_N64_MIC = 0x00010000,
  SI_N64_KEYBOARD = 0x00020000,
  SI_N64_MOUSE = 0x02000000,
  SI_N64_CONTROLLER = 0x05000000,
  SI_GBA = 0x00040000,
  SI_GC_CONTROLLER = (SI_TYPE_GC | SI_GC_STANDARD),
  SI_GC_KEYBOARD = (SI_TYPE_GC | 0x00200000),
  SI_GC_STEERING = SI_TYPE_GC,
  SI_DANCEMAT = (SI_TYPE_GC | SI_GC_STANDARD | 0x00000300),
  SI_AM_BASEBOARD = 0x10110800  // the low bits carry the DIP switches
};

// First byte of a communication-buffer transfer.
enum class EBufferCommands : u8
{
  CMD_STATUS = 0x00,
  CMD_READ_GBA = 0x14,
  CMD_WRITE_GBA = 0x15,
  CMD_SET_GAME_ID = 0x1d,
  CMD_DIRECT = 0x40,
  CMD_ORIGIN = 0x41,
  CMD_RECALIBRATE = 0x42,
  CMD_DIRECT_KB = 0x54,
  CMD_AM_BASEBOARD = 0x70,
  CMD_RESET = 0xFF
};

// Command byte of a port's output word (sent on every poll).
enum class EDirectCommands : u8
{
  CMD_FORCE = 0x30,
  CMD_WRITE = 0x40,
  CMD_POLL = 0x54
};

// Output word layout: command in bits 16..23, parameters below it.
union UCommand
{
  u32 hex = 0;
  struct
  {
    u32 parameter1 : 8;
    u32 parameter2 : 8;
    u32 command : 8;
    u32 : 8;
  };
  UCommand() = default;
  UCommand(u32 value) : hex{value} {}
};

// What can be plugged into a port (config values; several share a device ID).
enum SIDevices : int
{
  SIDEVICE_NONE,
  SIDEVICE_N64_MIC,
  SIDEVICE_N64_KEYBOARD,
  SIDEVICE_N64_MOUSE,
  SIDEVICE_N64_CONTROLLER,
  SIDEVICE_GC_GBA,
  SIDEVICE_GC_CONTROLLER,
  SIDEVICE_GC_KEYBOARD,
  SIDEVICE_GC_STEERING,
  SIDEVICE_DANCEMAT,
  SIDEVICE_GC_TARUKONGA,
  SIDEVICE_AM_BASEBOARD,
  SIDEVICE_WIIU_ADAPTER,
  SIDEVICE_GC_GBA_EMULATED,
  // Number of entries, for range checks.
  SIDEVICE_COUNT,
};

enum class DataResponse
{
  NoData,
  Success,
  ErrorNoResponse,
};

class ISIDevice
{
public:
  ISIDevice(Core::System& system, SIDevices device_type, int device_number);
  virtual ~ISIDevice();

  int GetDeviceNumber() const;
  SIDevices GetDeviceType() const;

  // Handles a communication-buffer transfer. Returns the reply length, 0 if
  // the reply is not ready yet (the SI asks again after TransferInterval()
  // cycles) or -1 if the device does not answer.
  virtual int RunBuffer(u8* buffer, int request_length);

  virtual int TransferInterval();

  // Fills the two input words for a poll.
  virtual DataResponse GetData(u32& hi, u32& low) = 0;

  // Receives the port's output word (when the game sets SISR.WR).
  virtual void SendCommand(u32 command, u8 poll) = 0;

  virtual void DoState(PointerWrap& p);

  // Called for events scheduled through SerialInterfaceManager::ScheduleEvent.
  virtual void OnEvent(u64 userdata, s64 cycles_late);

protected:
  // Writes the top three bytes of `si_device_id`, big-endian.
  static int CreateStatusResponse(u32 si_device_id, u8* buffer);

  Core::System& m_system;

  int m_device_number;
  SIDevices m_device_type;
};

int SIDevice_GetGBATransferTime(const SystemTimers::SystemTimersManager& timers,
                                EBufferCommands cmd);
bool SIDevice_IsGCController(SIDevices type);

std::unique_ptr<ISIDevice> SIDevice_Create(Core::System& system, SIDevices device, int port_number);
}  // namespace SerialInterface
