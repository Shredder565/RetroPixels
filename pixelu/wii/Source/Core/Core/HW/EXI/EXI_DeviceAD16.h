// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Core/HW/EXI/EXI_Device.h"

class PointerWrap;

namespace ExpansionInterface
{
// The AD16, a development diagnostics board with one 32-bit register. A command byte (0x00 read
// ID, 0xa0 write, 0xa2 read) is followed by the register's four bytes.
class CEXIAD16 : public IEXIDevice
{
public:
  explicit CEXIAD16(Core::System& system);
  void SetCS(int cs) override;
  bool IsPresent() const override;
  void DoState(PointerWrap& p) override;

private:
  void TransferByte(u8& byte) override;

  // Saved in this order.
  u32 m_position = 0;
  u32 m_command = 0;
  std::array<u8, 4> m_register{};
};
}  // namespace ExpansionInterface
