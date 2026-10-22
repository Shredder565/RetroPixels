// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

namespace DSP::HLE
{
class DSPHLE;

// The small program run when the CPU clears the DSP init bit. It only says hello.
class INITUCode final : public UCodeInterface
{
public:
  INITUCode(DSPHLE* dsphle, u32 crc);

  void Initialize() override;
  void HandleMail(u32 mail) override;
  void Update() override;
  void DoState(PointerWrap& p) override;
};
}  // namespace DSP::HLE
