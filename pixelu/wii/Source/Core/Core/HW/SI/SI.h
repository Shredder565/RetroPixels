// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

// The serial interface (SI) at 0x0C006400 talks to the four controller ports.
// Each port has an output word (the command sent on every poll), two input
// words (the last poll reply) and a shared 128-byte buffer used for one-off
// "communication RAM" transfers such as status, origin or GBA link commands.

#include <array>
#include <memory>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace Core
{
class System;
}
namespace CoreTiming
{
struct EventType;
}
namespace MMIO
{
class Mapping;
}

namespace SerialInterface
{
class ISIDevice;
enum SIDevices : int;

enum
{
  MAX_SI_CHANNELS = 0x04
};

class SerialInterfaceManager
{
public:
  explicit SerialInterfaceManager(Core::System& system);
  SerialInterfaceManager(const SerialInterfaceManager&) = delete;
  SerialInterfaceManager(SerialInterfaceManager&&) = delete;
  SerialInterfaceManager& operator=(const SerialInterfaceManager&) = delete;
  SerialInterfaceManager& operator=(SerialInterfaceManager&&) = delete;
  ~SerialInterfaceManager();

  void Init();
  void Shutdown();
  void DoState(PointerWrap& p);

  void RegisterMMIO(MMIO::Mapping* mmio, u32 base);

  void ScheduleEvent(int device_number, s64 cycles_into_future, u64 userdata = 0);
  void RemoveEvent(int device_number);

  void UpdateDevices();
  u32 GetInLength() const { return (m_comcsr >> 8) & 0x7f; }

  void RemoveDevice(int device_number);
  void AddDevice(SIDevices device, int device_number);
  void AddDevice(std::unique_ptr<ISIDevice> device);

  SIDevices GetDeviceType(int channel) const;

  u32 GetPollXLines();

  static constexpr u32 BUFFER_SIZE = 128;

private:
  // One controller port. The three words are exposed directly over MMIO.
  struct Port
  {
    u32 out = 0;    // SICxOUTBUF: command byte and two parameters
    u32 in_hi = 0;  // SICxINBUFH: bits 31/30 are the error status/latch flags
    u32 in_lo = 0;  // SICxINBUFL
    std::unique_ptr<ISIDevice> device;
    // After an unplug the port refuses another device change for one second.
    bool unplug_cooldown = false;
  };

  void FlagNoResponse(u32 port);
  void RefreshInterrupt();
  void RaiseTransferComplete();
  void SwapDevice(SIDevices device, int port);
  void ProcessTransfer(s64 cycles_late);

  void WriteComCsr(u32 value);
  void WriteStatus(u32 value);
  u32 ReadInputWord(u32 port, bool high);

  static void OnTransferEvent(Core::System& system, u64 userdata, s64 cycles_late);
  static void OnCooldownEvent(Core::System& system, u64 userdata, s64 cycles_late);

  Core::System& m_system;

  CoreTiming::EventType* m_cooldown_event = nullptr;
  CoreTiming::EventType* m_transfer_event = nullptr;
  std::array<CoreTiming::EventType*, MAX_SI_CHANNELS> m_port_events{};

  std::array<Port, MAX_SI_CHANNELS> m_ports;

  // Global registers, kept as raw words (they are saved and mapped as such).
  u32 m_poll = 0;        // SIPOLL
  u32 m_comcsr = 0;      // SICOMCSR
  u32 m_status = 0;      // SISR
  u32 m_exi_clock = 0;   // SIEXILK
  std::array<u8, BUFFER_SIZE> m_buffer{};
};
}  // namespace SerialInterface
