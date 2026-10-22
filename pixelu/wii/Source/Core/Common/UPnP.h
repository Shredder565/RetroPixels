// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#ifdef USE_UPNP

#include "Common/CommonTypes.h"

// UDP port forwarding on the local router through UPnP (for hosting netplay).
namespace Common::UPnP
{
// Asks the router, on a background thread, to forward UDP `port` to this machine. Replaces an
// earlier mapping made here.
void TryPortmapping(u16 port);
// Removes the mapping made by TryPortmapping(), waiting for it to finish.
void StopPortmapping();
}  // namespace Common::UPnP

#endif
