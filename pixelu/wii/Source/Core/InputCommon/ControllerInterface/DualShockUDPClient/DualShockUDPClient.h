// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include "Common/CommonTypes.h"
#include "Common/Config/ConfigInfo.h"
#include "InputCommon/ControllerInterface/InputBackend.h"

// Client for DSU ("cemuhook") servers, which stream controllers (with motion data) over UDP.
namespace ciface::DualShockUDPClient
{
constexpr char DEFAULT_SERVER_ADDRESS[] = "127.0.0.1";
constexpr u16 DEFAULT_SERVER_PORT = 26760;

namespace Settings
{
// The single server of older configurations; moved into SERVERS on startup.
extern const Config::Info<std::string> SERVER_ADDRESS;
extern const Config::Info<int> SERVER_PORT;

// "description:address:port;" for each server.
extern const Config::Info<std::string> SERVERS;
extern const Config::Info<bool> SERVERS_ENABLED;
}  // namespace Settings

std::unique_ptr<ciface::InputBackend> CreateInputBackend(ControllerInterface* controller_interface);

}  // namespace ciface::DualShockUDPClient
