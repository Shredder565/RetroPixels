// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

// dolphin-tool links Core but has no user interface; these are the host hooks Core expects,
// answering "nothing to show, no focus, no UI".

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Core/Host.h"

std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}

bool Host_UIBlocksControllerState()
{
  return false;
}

bool Host_RendererHasFocus()
{
  return false;
}

bool Host_RendererHasFullFocus()
{
  return false;
}

bool Host_RendererIsFullscreen()
{
  return false;
}

bool Host_TASInputHasFocus()
{
  return false;
}

bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
                                   const std::string&, const std::string&, const std::string&,
                                   const int64_t, const int64_t, const int, const int)
{
  return false;
}

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core>)
{
  return nullptr;
}

// Notifications with nothing to update.
void Host_Message(HostMessageID) {}
void Host_UpdateTitle(const std::string&) {}
void Host_UpdateDiscordClientID(const std::string&) {}
void Host_UpdateDisasmDialog() {}
void Host_PPCSymbolsChanged() {}
void Host_PPCBreakpointsChanged() {}
void Host_JitCacheInvalidation() {}
void Host_JitProfileDataWiped() {}
void Host_RequestRenderWindowSize(int, int) {}
void Host_YieldToUI() {}
void Host_TitleChanged() {}
