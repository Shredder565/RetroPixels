// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <deque>
#include <span>
#include <vector>

#include "Common/CommonTypes.h"

class PointerWrap;

namespace Triforce
{

// Something plugged into one of the Triforce baseboard's serial ports (A or
// B). The baseboard pushes bytes in and pulls the device's answers out; a
// subclass parses the incoming bytes in Update().
class SerialDevice
{
public:
  SerialDevice() = default;
  virtual ~SerialDevice() = default;

  SerialDevice(const SerialDevice&) = delete;
  SerialDevice& operator=(const SerialDevice&) = delete;
  SerialDevice(SerialDevice&&) = delete;
  SerialDevice& operator=(SerialDevice&&) = delete;

  void WriteRxBytes(std::span<const u8> bytes);

  std::size_t GetTxByteCount() const { return m_outbound.size(); }

  // `bytes` must not be larger than GetTxByteCount().
  void TakeTxBytes(std::span<u8> bytes);

  virtual void Update() = 0;

  virtual void DoState(PointerWrap& p);

protected:
  std::span<const u8> GetRxByteSpan() const { return m_inbound; }

  void ConsumeRxBytes(std::size_t count);

  void WriteTxByte(u8 byte) { m_outbound.push_back(byte); }
  void WriteTxBytes(std::span<const u8> bytes);

  // Moves everything `other` has queued for the baseboard to the end of our
  // own queue, leaving `other` with nothing to send.
  void PassThroughTxBytes(SerialDevice& other);

private:
  // Baseboard -> device. Kept contiguous so packets can be parsed in place;
  // devices usually consume all of it at once.
  std::vector<u8> m_inbound;

  // Device -> baseboard. The baseboard may drain it a few bytes at a time.
  std::deque<u8> m_outbound;
};

}  // namespace Triforce
