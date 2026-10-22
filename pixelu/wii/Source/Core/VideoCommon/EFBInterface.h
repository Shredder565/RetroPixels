// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

enum class EFBReinterpretType;

// CPU access to the embedded framebuffer (the pixel engine's peek/poke window), plus the
// pixel-format reinterpretation the GX pipeline asks for when the EFB format changes.
class EFBInterfaceBase
{
public:
  virtual ~EFBInterfaceBase();

  virtual void ReinterpretPixelData(EFBReinterpretType convtype) = 0;

  virtual void PokeColor(u16 x, u16 y, u32 color) = 0;
  virtual void PokeDepth(u16 x, u16 y, u32 depth) = 0;

  // Values as the game reads them: colour in the PE's ARGB layout with the alpha read mode
  // applied, depth as a 24-bit (or Z16-encoded) value. 0 outside the EFB or with EFB access off.
  u32 PeekColor(u16 x, u16 y);
  u32 PeekDepth(u16 x, u16 y);

protected:
  // True when an access should be ignored (EFB access disabled or coordinates out of range).
  bool ShouldSkipAccess(u16 x, u16 y) const;

  virtual u32 PeekColorInternal(u16 x, u16 y) = 0;
  virtual u32 PeekDepthInternal(u16 x, u16 y) = 0;
};

// Peeks and pokes through the framebuffer manager on the video thread.
class HardwareEFBInterface final : public EFBInterfaceBase
{
  void ReinterpretPixelData(EFBReinterpretType convtype) override;

  void PokeColor(u16 x, u16 y, u32 color) override;
  void PokeDepth(u16 x, u16 y, u32 depth) override;

  u32 PeekColorInternal(u16 x, u16 y) override;
  u32 PeekDepthInternal(u16 x, u16 y) override;
};

extern std::unique_ptr<EFBInterfaceBase> g_efb_interface;
