// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <array>

#include "Common/CommonTypes.h"
#include "Common/IOFile.h"
#include "Core/HW/EXI/EXI_Device.h"

namespace Core
{
class System;
}

namespace ExpansionInterface
{
// Raised by the media board when a board command completes.
void GenerateInterrupt(int flag);

// The Triforce baseboard's EXI side: battery-backed memory (kept in a file)
// and the interrupt line the media board uses.
class CEXIBaseboard final : public IEXIDevice
{
public:
  explicit CEXIBaseboard(Core::System& system);
  ~CEXIBaseboard() override;

  void SetCS(int cs) override;
  bool IsInterruptSet() override;
  bool IsPresent() const override;
  void DoState(PointerWrap& p) override;
  void DMAWrite(u32 addr, u32 size) override;
  void DMARead(u32 addr, u32 size) override;

protected:
  void TransferByte(u8& byte) override;

private:
  u8 RunCommand();
  u8 ContinueCommand(u8 byte);

  u32 m_position = 0;
  std::array<u8, 4> m_command{};
  u16 m_backup_offset = 0;
  u32 m_dma_offset = 0;
  u32 m_dma_length = 0;
  File::IOFile m_backup;
};
}  // namespace ExpansionInterface
