// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <vector>

// One DSU (cemuhook protocol) server the client connects to.
struct DualShockUDPServer
{
  std::string description;
  std::string server_address;
  int server_port;
};

// The DSU server list as stored in the DSUClient "Entries" setting:
// "description:address:port;" per server.
namespace DualShockUDPSettings
{
constexpr char FIELD_SEPARATOR = ':';
constexpr char SERVER_SEPARATOR = ';';

std::vector<DualShockUDPServer> GetServers();

void AddServer(DualShockUDPServer server);

void ReplaceServer(size_t index, DualShockUDPServer server);

void RemoveServer(size_t index);

bool IsEnabled();

void SetEnabled(bool enabled);
}  // namespace DualShockUDPSettings
