// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// Callbacks from the emulation core into the front end.
//
// The core never talks to a window system or UI toolkit directly. Whatever
// program embeds it (the Qt front end in PixelU-Wii) defines every Host_*
// function below, and the core calls them when it needs the UI to do or answer
// something.

namespace HW::GBA
{
class Core;
}  // namespace HW::GBA

// Implemented by the front end for each emulated Game Boy Advance window.
class GBAHostInterface
{
public:
  virtual ~GBAHostInterface() = default;
  virtual void GameChanged() = 0;
  virtual void FrameEnded(std::span<const u32> video_buffer) = 0;
};

// Messages for Host_Message. The values start at 10 and must not change.
enum class HostMessageID
{
  WMUserStop = 10,
  WMUserJobDispatch,
};

// Queries.
std::vector<std::string> Host_GetPreferredLocales();
bool Host_UIBlocksControllerState();
bool Host_RendererHasFocus();
bool Host_RendererHasFullFocus();
bool Host_RendererIsFullscreen();
bool Host_TASInputHasFocus();

// Notifications and requests.
void Host_Message(HostMessageID id);
void Host_PPCSymbolsChanged();
void Host_PPCBreakpointsChanged();
void Host_RequestRenderWindowSize(int width, int height);
void Host_UpdateDisasmDialog();
void Host_JitCacheInvalidation();
void Host_JitProfileDataWiped();
void Host_UpdateTitle(const std::string& title);
void Host_YieldToUI();
void Host_TitleChanged();

// Discord rich presence.
void Host_UpdateDiscordClientID(const std::string& client_id = {});
bool Host_UpdateDiscordPresenceRaw(const std::string& details = {}, const std::string& state = {},
                                   const std::string& large_image_key = {},
                                   const std::string& large_image_text = {},
                                   const std::string& small_image_key = {},
                                   const std::string& small_image_text = {},
                                   const int64_t start_timestamp = 0,
                                   const int64_t end_timestamp = 0, const int party_size = 0,
                                   const int party_max = 0);

std::unique_ptr<GBAHostInterface> Host_CreateGBAHost(std::weak_ptr<HW::GBA::Core> core);
