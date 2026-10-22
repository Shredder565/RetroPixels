// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Core/HW/DSPHLE/UCodes/UCodes.h"

namespace Memory
{
class MemoryManager;
}

namespace DSP::HLE
{
class DSPHLE;

// The JoyBoot key exchange used to send a program to a GBA. `address` points to the request
// block in RAM:
//   +0  challenge read from the GBA (little-endian)
//   +4  palette of the logo animation the GBA BIOS shows (0-6)
//   +8  speed/direction of that animation (-4..4, low byte significant)
//   +12 length of the program to send
//   +16 where to write the two result words
// The results are the session key and the encoded transfer parameters.
void ProcessGBACrypto(Memory::MemoryManager& memory, u32 address);

// The GBA link task: one crypto request, then back to the task manager.
class GBAUCode final : public UCodeInterface
{
public:
  GBAUCode(DSPHLE* dsphle, u32 crc);

  void Initialize() override;
  void HandleMail(u32 mail) override;
  void Update() override;
  void DoState(PointerWrap& p) override;

private:
  static constexpr u32 REQUEST_MAIL = 0xabba0000;

  enum class Stage
  {
    AwaitingRequest,
    AwaitingAddress,
    AwaitingNextTask,
  };

  Stage m_stage = Stage::AwaitingRequest;
};
}  // namespace DSP::HLE
