// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "Core/Config/NetplaySettings.h"

namespace Config
{
namespace
{
Location NetPlayKey(std::string_view key)
{
  return Location{System::Main, "NetPlay", std::string(key)};
}

// Hosting, connecting and listening all default to the same UDP port.
constexpr u16 STANDARD_NETPLAY_PORT = 2626;
}  // namespace

// Traversal (NAT punching) server and the public lobby.
const Info<std::string> NETPLAY_TRAVERSAL_SERVER{NetPlayKey("TraversalServer"),
                                                 "stun.dolphin-emu.org"};
const Info<u16> NETPLAY_TRAVERSAL_PORT{NetPlayKey("TraversalPort"), 6262};
const Info<u16> NETPLAY_TRAVERSAL_PORT_ALT{NetPlayKey("TraversalPortAlt"), 6226};
const Info<std::string> NETPLAY_TRAVERSAL_CHOICE{NetPlayKey("TraversalChoice"), "direct"};
const Info<std::string> NETPLAY_INDEX_URL{NetPlayKey("IndexServer"),
                                          "https://lobby.dolphin-emu.org"};

const Info<bool> NETPLAY_USE_INDEX{NetPlayKey("UseIndex"), false};
const Info<std::string> NETPLAY_INDEX_NAME{NetPlayKey("IndexName"), ""};
const Info<std::string> NETPLAY_INDEX_REGION{NetPlayKey("IndexRegion"), ""};
const Info<std::string> NETPLAY_INDEX_PASSWORD{NetPlayKey("IndexPassword"), ""};

const Info<std::string> NETPLAY_HOST_CODE{NetPlayKey("HostCode"), "00000000"};

// Direct connections.
const Info<u16> NETPLAY_HOST_PORT{NetPlayKey("HostPort"), STANDARD_NETPLAY_PORT};
const Info<std::string> NETPLAY_ADDRESS{NetPlayKey("Address"), "127.0.0.1"};
const Info<u16> NETPLAY_CONNECT_PORT{NetPlayKey("ConnectPort"), STANDARD_NETPLAY_PORT};
const Info<u16> NETPLAY_LISTEN_PORT{NetPlayKey("ListenPort"), STANDARD_NETPLAY_PORT};

const Info<std::string> NETPLAY_NICKNAME{NetPlayKey("Nickname"), "Player"};
const Info<bool> NETPLAY_USE_UPNP{NetPlayKey("UseUPNP"), false};

const Info<bool> NETPLAY_ENABLE_QOS{NetPlayKey("EnableQoS"), true};

// Upload throttling for save and code synchronisation (in kilobits per second).
const Info<bool> NETPLAY_ENABLE_CHUNKED_UPLOAD_LIMIT{NetPlayKey("EnableChunkedUploadLimit"),
                                                     false};
const Info<u32> NETPLAY_CHUNKED_UPLOAD_LIMIT{NetPlayKey("ChunkedUploadLimit"), 3000};

// Input buffering, in frames.
const Info<u32> NETPLAY_BUFFER_SIZE{NetPlayKey("BufferSize"), 5};
const Info<u32> NETPLAY_CLIENT_BUFFER_SIZE{NetPlayKey("BufferSizeClient"), 1};

// Session options chosen by the host.
const Info<bool> NETPLAY_SAVEDATA_LOAD{NetPlayKey("SyncSaves"), true};
const Info<bool> NETPLAY_SAVEDATA_WRITE{NetPlayKey("WriteSaveData"), true};
const Info<bool> NETPLAY_SAVEDATA_SYNC_ALL_WII{NetPlayKey("SyncAllWiiSaves"), false};
const Info<bool> NETPLAY_SYNC_CODES{NetPlayKey("SyncCodes"), true};
const Info<bool> NETPLAY_RECORD_INPUTS{NetPlayKey("RecordInputs"), false};
const Info<bool> NETPLAY_STRICT_SETTINGS_SYNC{NetPlayKey("StrictSettingsSync"), false};
const Info<std::string> NETPLAY_NETWORK_MODE{NetPlayKey("NetworkMode"), "fixeddelay"};
const Info<bool> NETPLAY_GOLF_MODE_OVERLAY{NetPlayKey("GolfModeOverlay"), true};
const Info<bool> NETPLAY_HIDE_REMOTE_GBAS{NetPlayKey("HideRemoteGBAs"), false};

}  // namespace Config
