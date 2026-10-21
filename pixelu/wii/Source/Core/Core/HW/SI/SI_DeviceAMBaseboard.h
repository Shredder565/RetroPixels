// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>
#include <memory>

#include "Core/HW/MagCard/MagneticCardReader.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/Triforce/IOPorts.h"

namespace Triforce
{
class JVSIOBoard;
class SerialDevice;
}  // namespace Triforce

namespace SerialInterface
{

// The Triforce (GC-AM) baseboard as seen from an SI port. Command 0x70 packs
// requests for the baseboard itself, its JVS-IO board and its two serial ports
// into one buffer; the answers come back in the same packed form, one request
// late.
class CSIDevice_AMBaseboard final : public ISIDevice
{
public:
  CSIDevice_AMBaseboard(Core::System& system, SIDevices device, int device_number);
  ~CSIDevice_AMBaseboard() override;

  int RunBuffer(u8* buffer, int request_length) override;

  DataResponse GetData(u32& hi, u32& low) override;

  void SendCommand(u32 command, u8 poll) override;

  void DoState(PointerWrap&) override;

private:
  static constexpr u32 RESPONSE_SIZE = SerialInterfaceManager::BUFFER_SIZE;

  int RunBaseboardCommand(u8* buffer, int request_length);

  std::array<std::array<u8, RESPONSE_SIZE>, 2> m_responses{};
  u8 m_response_index = 0;

  Triforce::IOPorts m_io_ports;
  std::unique_ptr<Triforce::JVSIOBoard> m_jvs;
  MagCard::MagneticCardReader::Settings m_card_settings;
  std::unique_ptr<Triforce::SerialDevice> m_serial_a;
  std::unique_ptr<Triforce::SerialDevice> m_serial_b;
};

}  // namespace SerialInterface
