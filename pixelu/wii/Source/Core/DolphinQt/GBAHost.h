// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef HAS_LIBMGBA

#include <span>

#include "Core/Host.h"

namespace HW::GBA
{
class Core;
}  // namespace HW::GBA

class GBAWidgetController;

// The core's view of one GBA window. It is called on emulation threads and forwards everything
// to a controller that lives on the GUI thread.
class GBAHost : public GBAHostInterface
{
public:
  explicit GBAHost(std::weak_ptr<HW::GBA::Core> core);
  ~GBAHost() override;
  void GameChanged() override;
  void FrameEnded(std::span<const u32> video_buffer) override;

private:
  GBAWidgetController* m_controller{};
  std::weak_ptr<HW::GBA::Core> m_core;
};
#endif  // HAS_LIBMGBA
