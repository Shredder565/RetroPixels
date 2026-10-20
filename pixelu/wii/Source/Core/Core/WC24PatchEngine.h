// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

// Replaces the server names WiiConnect24 channels contact (for example with WiiLink's), from the
// [WC24Patch] section of the channel's game ini.
namespace WC24PatchEngine
{
// Whether a patch is for KD (the WC24 download daemon) or for the channel's own sockets.
enum class IsKD : bool;

struct NetworkPatch final
{
  std::string name;
  std::string source;
  std::string replacement;
  bool enabled = false;
  IsKD is_kd = IsKD{false};
};

void Reload();

// The replacement for the host `source`, if an enabled patch has one.
std::optional<std::string> GetNetworkPatch(std::string_view source, IsKD is_kd);
// For socket traffic: rewrites the HTTP "Host: " header of `source` (a request payload) when an
// enabled non-KD patch matches it, and returns the new payload.
std::optional<std::string> GetNetworkPatchByPayload(std::string_view source);
}  // namespace WC24PatchEngine
