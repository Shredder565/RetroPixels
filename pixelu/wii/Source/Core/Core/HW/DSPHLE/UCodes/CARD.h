// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

namespace DSP::HLE
{
class DSPHLE;

// The memory card unlock task. Its real job (the challenge computation for the card's unlock
// sequence) is not needed by the emulated cards, so any request just finishes the task.
class CARDUCode final : public UCodeInterface
{
public:
  CARDUCode(DSPHLE* dsphle, u32 crc);

  void Initialize() override;
  void HandleMail(u32 mail) override;
  void Update() override;
  void DoState(PointerWrap& p) override;

private:
  enum class Stage
  {
    AwaitingRequest,
    AwaitingAddress,
    AwaitingNextTask,
  };

  // Not advanced at the moment; saved for the savestate layout.
  Stage m_stage = Stage::AwaitingRequest;
};
}  // namespace DSP::HLE
