// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DualShockUDPSettings.h"

#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/StringUtil.h"
#include "InputCommon/ControllerInterface/DualShockUDPClient/DualShockUDPClient.h"

namespace DSU = ciface::DualShockUDPClient::Settings;

namespace
{
std::string Encode(const DualShockUDPServer& server)
{
  using DualShockUDPSettings::FIELD_SEPARATOR;
  using DualShockUDPSettings::SERVER_SEPARATOR;
  return fmt::format("{}{}{}{}{}{}", server.description, FIELD_SEPARATOR, server.server_address,
                     FIELD_SEPARATOR, server.server_port, SERVER_SEPARATOR);
}

// Older versions stored a single server in Address/Port. Move such a server
// to the end of the list (named "DS4") and clear the old keys.
void ImportLegacyServer()
{
  const std::string address = Config::Get(DSU::SERVER_ADDRESS);
  const int port = Config::Get(DSU::SERVER_PORT);
  if (address.empty() || port == 0)
    return;

  const std::string list = Config::Get(DSU::SERVERS);
  Config::SetBaseOrCurrent(DSU::SERVERS, list + Encode(DualShockUDPServer{"DS4", address, port}));
  Config::SetBase(DSU::SERVER_ADDRESS, "");
  Config::SetBase(DSU::SERVER_PORT, 0);
}

// The stored entries, split at the server separator. The text after the
// last separator (normally empty) counts as an entry too.
std::vector<std::string> StoredEntries()
{
  ImportLegacyServer();
  return SplitString(Config::Get(DSU::SERVERS), DualShockUDPSettings::SERVER_SEPARATOR);
}

// Rebuilds the list from entries, each followed by a separator, with
// the entry at `index` replaced by `replacement` (or dropped when there is
// none).
void StoreEntries(const std::vector<std::string>& entries, size_t index,
                  const DualShockUDPServer* replacement)
{
  std::string list;
  for (size_t i = 0; i < entries.size(); ++i)
  {
    if (i != index)
      list += entries[i] + DualShockUDPSettings::SERVER_SEPARATOR;
    else if (replacement != nullptr)
      list += Encode(*replacement);
  }
  Config::SetBaseOrCurrent(DSU::SERVERS, list);
}
}  // namespace

namespace DualShockUDPSettings
{
std::vector<DualShockUDPServer> GetServers()
{
  std::vector<DualShockUDPServer> servers;
  for (const std::string& entry : StoredEntries())
  {
    // Entries without description, address and port are skipped, as are
    // ports that are not numbers.
    const std::vector<std::string> fields = SplitString(entry, FIELD_SEPARATOR);
    if (fields.size() < 3)
      continue;
    int port;
    if (!TryParse(fields[2], &port))
      continue;
    servers.push_back(DualShockUDPServer{fields[0], fields[1], port});
  }
  return servers;
}

void AddServer(DualShockUDPServer server)
{
  ImportLegacyServer();
  Config::SetBaseOrCurrent(DSU::SERVERS, Config::Get(DSU::SERVERS) + Encode(server));
}

void ReplaceServer(size_t index, DualShockUDPServer server)
{
  StoreEntries(StoredEntries(), index, &server);
}

void RemoveServer(size_t index)
{
  StoreEntries(StoredEntries(), index, nullptr);
}

bool IsEnabled()
{
  return Config::Get(DSU::SERVERS_ENABLED);
}

void SetEnabled(bool enabled)
{
  Config::SetBaseOrCurrent(DSU::SERVERS_ENABLED, enabled);
}
}  // namespace DualShockUDPSettings
