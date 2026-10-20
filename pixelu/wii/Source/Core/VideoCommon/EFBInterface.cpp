// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "VideoCommon/EFBInterface.h"

#include <algorithm>
#include <memory>

#include "Common/MsgHandler.h"

#include "Core/ConfigManager.h"
#include "Core/System.h"

#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/PixelEngine.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/VideoConfig.h"

std::unique_ptr<EFBInterfaceBase> g_efb_interface;

namespace
{
// The host keeps colours as RGBA in memory order; the pixel engine uses ARGB in a register.
// Converting either way swaps the red and blue bytes.
constexpr u32 SwapRedBlue(u32 value)
{
  const u32 red_to_low = (value >> 16) & 0x000000FF;
  const u32 blue_to_high = (value << 16) & 0x00FF0000;
  return (value & 0xFF00FF00) | red_to_low | blue_to_high;
}

// Host depth is a float fraction of 2^24, flipped when the backend has no reversed depth range.
float HostDepthFromZ24(u32 z24)
{
  const float fraction = static_cast<float>(z24 & 0xFFFFFF) / 16777216.0f;
  return g_backend_info.bSupportsReversedDepthRange ? fraction : 1.0f - fraction;
}
}  // namespace

EFBInterfaceBase::~EFBInterfaceBase() = default;

bool EFBInterfaceBase::ShouldSkipAccess(u16 x, u16 y) const
{
  if (!g_ActiveConfig.bEFBAccessEnable)
    return true;
  return x >= EFB_WIDTH || y >= EFB_HEIGHT;
}

u32 EFBInterfaceBase::PeekColor(u16 x, u16 y)
{
  if (ShouldSkipAccess(x, y))
    return 0;

  const u32 color = PeekColorInternal(x, y);

  // The PE alpha read setting (GX_PokeAlphaRead) decides what the alpha byte reads as.
  using Mode = PixelEngine::AlphaReadMode;
  const Mode mode = Core::System::GetInstance().GetPixelEngine().GetAlphaReadMode();
  switch (mode)
  {
  case Mode::ReadNone:
    return color;
  case Mode::ReadFF:
    return color | 0xFF000000;
  case Mode::Read00:
    break;
  default:
    PanicAlertFmt("Invalid PE alpha read mode: {}", static_cast<u16>(mode));
    break;
  }
  return color & 0x00FFFFFF;
}

u32 EFBInterfaceBase::PeekDepth(u16 x, u16 y)
{
  return ShouldSkipAccess(x, y) ? 0 : PeekDepthInternal(x, y);
}

void HardwareEFBInterface::ReinterpretPixelData(EFBReinterpretType convtype)
{
  g_framebuffer_manager->ReinterpretPixelData(convtype);
}

u32 HardwareEFBInterface::PeekColorInternal(u16 x, u16 y)
{
  const u32 host_rgba = AsyncRequests::GetInstance()->PushBlockingEvent([&] {
    INCSTAT(g_stats.this_frame.num_efb_peeks);
    return g_framebuffer_manager->PeekEFBColor(x, y);
  });
  u32 argb = SwapRedBlue(host_rgba);

  // The host buffer is always RGBA8; reduce the value to what the emulated format can hold.
  // Only RGBA6 has an alpha channel; the other formats read alpha as opaque.
  switch (bpmem.zcontrol.pixel_format)
  {
  case PixelFormat::RGBA6_Z24:
    return RGBA8ToRGBA6ToRGBA8(argb);
  case PixelFormat::RGB565_Z16:
    argb = RGBA8ToRGB565ToRGBA8(argb);
    break;
  default:
    break;
  }
  return argb | 0xFF000000;
}

u32 HardwareEFBInterface::PeekDepthInternal(u16 x, u16 y)
{
  float depth = AsyncRequests::GetInstance()->PushBlockingEvent([&] {
    INCSTAT(g_stats.this_frame.num_efb_peeks);
    return g_framebuffer_manager->PeekEFBDepth(x, y);
  });
  if (!g_backend_info.bSupportsReversedDepthRange)
    depth = 1.0f - depth;

  const u32 z24 = std::clamp<u32>(static_cast<u32>(depth * 16777216.0f), 0, 0xFFFFFF);

  // The host EFB always has 24-bit depth (multisampled Z16 is not emulated), so in RGB565_Z16
  // mode the value is encoded into the game's 16-bit Z format on the way out.
  if (bpmem.zcontrol.pixel_format == PixelFormat::RGB565_Z16)
    return CompressZ16(z24, bpmem.zcontrol.zformat);
  return z24;
}

void HardwareEFBInterface::PokeColor(u16 x, u16 y, u32 poke_data)
{
  if (ShouldSkipAccess(x, y))
    return;

  // Alpha is written as given, whatever the EFB format.
  const u32 rgba = SwapRedBlue(poke_data);
  AsyncRequests::GetInstance()->PushEvent([x, y, rgba] {
    INCSTAT(g_stats.this_frame.num_efb_pokes);
    g_framebuffer_manager->PokeEFBColor(x, y, rgba);
  });
}

void HardwareEFBInterface::PokeDepth(u16 x, u16 y, u32 poke_data)
{
  if (ShouldSkipAccess(x, y))
    return;

  const float depth = HostDepthFromZ24(poke_data);
  AsyncRequests::GetInstance()->PushEvent([x, y, depth] {
    INCSTAT(g_stats.this_frame.num_efb_pokes);
    g_framebuffer_manager->PokeEFBDepth(x, y, depth);
  });
}
