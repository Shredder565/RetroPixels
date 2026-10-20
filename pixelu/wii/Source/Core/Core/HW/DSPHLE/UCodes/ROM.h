// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

namespace DSP::HLE
{
class DSPHLE;

// The DSP boot ROM. It greets the CPU with 0x8071FEED and then waits for the 0x80F3xxxx
// parameter mails describing the ucode to load; the last one starts that ucode.
class ROMUCode final : public UCodeInterface
{
public:
  ROMUCode(DSPHLE* dsphle, u32 crc);

  void Initialize() override;
  void HandleMail(u32 mail) override;
  void Update() override;
  void DoState(PointerWrap& p) override;

private:
  // Boot parameters, saved as raw bytes in this order.
  struct BootParams
  {
    u32 ram_address;
    u32 length;
    u32 imem_address;
    u32 dmem_length;
    u32 start_pc;
  };
  static_assert(sizeof(BootParams) == 20);

  void StartLoadedUCode();

  BootParams m_params{};
  // Not used by the ROM; kept for the savestate layout.
  int m_unused_step_count = 0;
  // The 0x80F3xxxx mail naming the parameter that the next mail carries, or 0.
  u32 m_pending_parameter = 0;
};
}  // namespace DSP::HLE
