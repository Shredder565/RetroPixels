// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_UPNP

#include "Common/UPnP.h"

#include "Common/Logging/Log.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <miniupnpc.h>
#include <miniwget.h>
#include <string>
#include <thread>
#include <upnpcommands.h>
#include <upnperrors.h>

// All router communication happens on one worker thread at a time; the public functions only
// start and join it, so the state below is never touched concurrently.

namespace
{
struct Gateway
{
  UPNPUrls urls{};
  IGDdatas data{};
  std::array<char, 20> local_ip{};  // our address as seen on the way to the gateway
};

Gateway s_gateway;
u16 s_mapped_port = 0;  // 0 = nothing mapped
std::thread s_worker;

enum class DiscoveryState
{
  NotTried,
  Done,  // finished, whether or not a usable gateway was found
  Failed,
};
DiscoveryState s_discovery = DiscoveryState::NotTried;

using DeviceList = std::unique_ptr<UPNPDev, decltype(&freeUPNPDevlist)>;
using MallocedChars = std::unique_ptr<char, decltype(&std::free)>;

DeviceList Discover(int* error)
{
  constexpr int TIMEOUT_MS = 2000;
#if MINIUPNPC_API_VERSION >= 14
  return {upnpDiscover(TIMEOUT_MS, nullptr, nullptr, 0, 0, 2, error), freeUPNPDevlist};
#else
  return {upnpDiscover(TIMEOUT_MS, nullptr, nullptr, 0, 0, error), freeUPNPDevlist};
#endif
}

// Downloads a device description, also learning our local address. Null on failure.
MallocedChars FetchDescription(const char* url, int* size)
{
  int status = 200;
#if MINIUPNPC_API_VERSION >= 16
  void* const body = miniwget_getaddr(url, size, s_gateway.local_ip.data(),
                                      static_cast<int>(s_gateway.local_ip.size()), 0, &status);
#else
  void* const body = miniwget_getaddr(url, size, s_gateway.local_ip.data(),
                                      static_cast<int>(s_gateway.local_ip.size()), 0);
#endif
  MallocedChars result(static_cast<char*>(body), std::free);
  if (status != 200)
    result.reset();
  return result;
}

// Finds the internet gateway device once per run (a failed discovery is not retried).
bool EnsureGateway()
{
  if (s_discovery == DiscoveryState::Done)
    return true;
  if (s_discovery == DiscoveryState::Failed)
    return false;

  s_gateway.urls = {};
  s_gateway.data = {};

  int error = 0;
  const DeviceList devices = Discover(&error);
  if (!devices)
  {
    if (error == UPNPDISCOVER_SUCCESS)
      WARN_LOG_FMT(NETPLAY, "UPnP: no devices found.");
    else
      WARN_LOG_FMT(NETPLAY, "UPnP: device discovery failed: {}", strupnperror(error));
    s_discovery = DiscoveryState::Failed;
    return false;
  }

  bool found = false;
  for (UPNPDev* device = devices.get(); device != nullptr && !found; device = device->pNext)
  {
    if (std::strstr(device->st, "InternetGatewayDevice") == nullptr)
      continue;

    int size = 0;
    const MallocedChars description = FetchDescription(device->descURL, &size);
    if (!description)
    {
      WARN_LOG_FMT(NETPLAY, "UPnP: could not read the gateway description at {}.",
                   device->descURL);
      continue;
    }

    parserootdesc(description.get(), size, &s_gateway.data);
    GetUPNPUrls(&s_gateway.urls, &s_gateway.data, device->descURL, 0);
    NOTICE_LOG_FMT(NETPLAY, "UPnP: using the gateway at {}.", device->descURL);
    found = true;
  }

  if (!found)
    WARN_LOG_FMT(NETPLAY, "UPnP: none of the discovered devices is a usable gateway.");

  s_discovery = DiscoveryState::Done;
  return true;
}

// Mappings must be removed explicitly: some routers never expire them and eventually stop
// accepting new ones when their table is full.
void RemoveMapping(u16 port)
{
  const std::string port_text = std::to_string(port);
  UPNP_DeletePortMapping(s_gateway.urls.controlURL, s_gateway.data.first.servicetype,
                         port_text.c_str(), "UDP", nullptr);
}

bool AddMapping(const char* local_ip, u16 port)
{
  if (s_mapped_port != 0)
    RemoveMapping(s_mapped_port);

  const std::string port_text = std::to_string(port);
  const std::string description = std::string("dolphin-emu UDP on ") + local_ip;
  const int result = UPNP_AddPortMapping(s_gateway.urls.controlURL,
                                         s_gateway.data.first.servicetype, port_text.c_str(),
                                         port_text.c_str(), local_ip, description.c_str(), "UDP",
                                         nullptr, nullptr);
  if (result != 0)
    return false;

  s_mapped_port = port;
  return true;
}

void MapWorker(u16 port)
{
  const char* const local_ip = s_gateway.local_ip.data();
  if (EnsureGateway() && AddMapping(local_ip, port))
    NOTICE_LOG_FMT(NETPLAY, "UPnP: forwarded port {} to {}.", port, local_ip);
  else
    WARN_LOG_FMT(NETPLAY, "UPnP: could not forward port {} to {}.", port, local_ip);
}

void UnmapWorker()
{
  if (s_mapped_port != 0)
    RemoveMapping(s_mapped_port);
}

void JoinWorker()
{
  if (s_worker.joinable())
    s_worker.join();
}
}  // namespace

void Common::UPnP::TryPortmapping(u16 port)
{
  JoinWorker();
  s_worker = std::thread(&MapWorker, port);
}

void Common::UPnP::StopPortmapping()
{
  JoinWorker();
  s_worker = std::thread(&UnmapWorker);
  JoinWorker();
}

#endif
