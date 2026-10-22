// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/NetPlayIndex.h"

#include <chrono>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/Common.h"
#include "Common/CommonTypes.h"
#include "Common/HttpRequest.h"
#include "Common/Version.h"

#include "Core/Config/NetplaySettings.h"

namespace
{
using Codes = Common::HttpRequest::AllowedReturnCodes;

// The index server only answers requests carrying this header.
const Common::HttpRequest::Headers INDEX_HEADERS = {{"X-Is-Dolphin", "1"}};

constexpr auto KEEP_ALIVE_INTERVAL = std::chrono::seconds(5);

std::string IndexRoot()
{
  return Config::Get(Config::NETPLAY_INDEX_URL);
}

std::optional<picojson::value> ParseJson(std::span<const u8> body)
{
  const char* cursor = reinterpret_cast<const char*>(body.data());
  const char* const end = cursor + body.size();

  picojson::value parsed;
  if (!picojson::parse(parsed, cursor, end).empty())
    return std::nullopt;
  return parsed;
}

// Outcome of a request whose reply only needs an "OK" status: nullopt on success, otherwise the
// error code to report.
std::optional<std::string> ReplyError(const Common::HttpRequest::Response& reply,
                                      std::optional<picojson::value>* json_out = nullptr)
{
  if (!reply)
    return "NO_RESPONSE";

  std::optional<picojson::value> json = ParseJson(*reply);
  if (!json)
    return "BAD_JSON";

  std::string status = json->get("status").to_str();
  if (status != "OK")
    return status;

  if (json_out != nullptr)
    *json_out = std::move(json);
  return std::nullopt;
}

// The password scrambling works on raw bytes, but strings hold chars.
u8 Byte(char c)
{
  return static_cast<u8>(c);
}

char Char(u32 byte)
{
  return static_cast<char>(static_cast<u8>(byte));
}

// Sum of the bytes, modulo 256.
u8 Checksum(std::string_view bytes)
{
  u8 sum = 0;
  for (const char c : bytes)
    sum = static_cast<u8>(sum + Byte(c));
  return sum;
}
}  // namespace

NetPlayIndex::NetPlayIndex() = default;

NetPlayIndex::~NetPlayIndex()
{
  if (!m_secret.empty())
    Remove();
  else
    StopKeepAlive();  // A thread that ended after a server error still needs joining.
}

std::optional<std::vector<NetPlaySession>>
NetPlayIndex::List(const std::map<std::string, std::string>& filters)
{
  Common::HttpRequest http;

  // Filter values are escaped, filter names are used as given.
  std::string url = IndexRoot() + "/v0/list";
  char separator = '?';
  for (const auto& [name, value] : filters)
  {
    url += fmt::format("{}{}={}", separator, name, http.EscapeComponent(value));
    separator = '&';
  }

  const Common::HttpRequest::Response reply = http.Get(url, INDEX_HEADERS, Codes::All);
  if (!reply)
  {
    m_last_error = "NO_RESPONSE";
    return std::nullopt;
  }

  const std::optional<picojson::value> json = ParseJson(*reply);
  if (!json)
  {
    m_last_error = "BAD_JSON";
    return std::nullopt;
  }

  if (std::string status = json->get("status").to_str(); status != "OK")
  {
    m_last_error = std::move(status);
    return std::nullopt;
  }

  std::vector<NetPlaySession> sessions;
  for (const picojson::value& entry : json->get("sessions").get<picojson::array>())
  {
    const picojson::value& name = entry.get("name");
    const picojson::value& region = entry.get("region");
    const picojson::value& method = entry.get("method");
    const picojson::value& game = entry.get("game");
    const picojson::value& server_id = entry.get("server_id");
    const picojson::value& password = entry.get("password");
    const picojson::value& players = entry.get("player_count");
    const picojson::value& port = entry.get("port");
    const picojson::value& in_game = entry.get("in_game");
    const picojson::value& version = entry.get("version");

    // Skip entries with any field missing or of the wrong type.
    const bool strings_ok = name.is<std::string>() && region.is<std::string>() &&
                            method.is<std::string>() && server_id.is<std::string>() &&
                            game.is<std::string>() && version.is<std::string>();
    const bool others_ok =
        password.is<bool>() && players.is<double>() && port.is<double>() && in_game.is<bool>();
    if (!strings_ok || !others_ok)
      continue;

    NetPlaySession& session = sessions.emplace_back();
    session.name = name.to_str();
    session.region = region.to_str();
    session.method = method.to_str();
    session.server_id = server_id.to_str();
    session.game_id = game.to_str();
    session.version = version.to_str();
    session.player_count = static_cast<int>(players.get<double>());
    session.port = static_cast<int>(port.get<double>());
    session.has_password = password.get<bool>();
    session.in_game = in_game.get<bool>();
  }

  return sessions;
}

bool NetPlayIndex::Add(const NetPlaySession& session)
{
  Common::HttpRequest http;
  const std::string url =
      fmt::format("{}/v0/session/add?name={}&region={}&game={}&password={:d}&method={}&server_id={}"
                  "&in_game={:d}&port={}&player_count={}&version={}",
                  IndexRoot(), http.EscapeComponent(session.name),
                  http.EscapeComponent(session.region), http.EscapeComponent(session.game_id),
                  session.has_password, session.method, session.server_id, session.in_game,
                  session.port, session.player_count, Common::GetScmDescStr());

  std::optional<picojson::value> json;
  if (std::optional<std::string> error =
          ReplyError(http.Get(url, INDEX_HEADERS, Codes::All), &json))
  {
    m_last_error = std::move(*error);
    return false;
  }

  m_secret = json->get("secret").to_str();
  m_game = session.game_id;
  m_player_count = session.player_count;
  m_in_game = session.in_game;

  // Replace any keep-alive thread left from an earlier session.
  StopKeepAlive();
  m_stop_keep_alive.Reset();
  m_keep_alive = std::thread([this] { KeepAlive(); });
  return true;
}

void NetPlayIndex::StopKeepAlive()
{
  m_stop_keep_alive.Set();
  if (m_keep_alive.joinable())
    m_keep_alive.join();
}

void NetPlayIndex::KeepAlive()
{
  while (!m_stop_keep_alive.WaitFor(KEEP_ALIVE_INTERVAL))
  {
    Common::HttpRequest http;
    const std::string url =
        fmt::format("{}/v0/session/active?secret={}&player_count={}&game={}&in_game={:d}",
                    IndexRoot(), m_secret, m_player_count, http.EscapeComponent(m_game), m_in_game);
    const Common::HttpRequest::Response reply = http.Get(url, INDEX_HEADERS, Codes::All);

    // A missed report is simply retried next time.
    if (!reply)
      continue;

    if (std::optional<std::string> error = ReplyError(reply))
    {
      // The server no longer knows the session.
      m_last_error = std::move(*error);
      m_secret.clear();
      m_error_callback();
      return;
    }
  }
}

void NetPlayIndex::Remove()
{
  if (m_secret.empty())
    return;

  StopKeepAlive();

  // Best effort: the server drops stale sessions on its own anyway.
  Common::HttpRequest http;
  http.Get(IndexRoot() + "/v0/session/remove?secret=" + m_secret, INDEX_HEADERS, Codes::All);
  m_secret.clear();
}

void NetPlayIndex::SetInGame(bool in_game)
{
  m_in_game = in_game;
}

void NetPlayIndex::SetPlayerCount(int player_count)
{
  m_player_count = player_count;
}

void NetPlayIndex::SetGame(std::string game)
{
  m_game = std::move(game);
}

const std::string& NetPlayIndex::GetLastError() const
{
  return m_last_error;
}

bool NetPlayIndex::HasActiveSession() const
{
  return !m_secret.empty();
}

void NetPlayIndex::SetErrorCallback(std::function<void()> callback)
{
  m_error_callback = std::move(callback);
}

std::vector<std::pair<std::string, std::string>> NetPlayIndex::GetRegions()
{
  return {
      {"EA", _trans("East Asia")},     {"CN", _trans("China")},         {"EU", _trans("Europe")},
      {"NA", _trans("North America")}, {"SA", _trans("South America")}, {"OC", _trans("Oceania")},
      {"AF", _trans("Africa")},
  };
}

// Scrambled form: the server ID plus a one-byte checksum; byte n is XORed with password byte
// (n mod password length), then n is added; each result byte is written as two letters 'A'-'P'
// (high nibble first). The position counter is 8 bits wide and wraps after 256 bytes.

bool NetPlaySession::EncryptID(std::string_view password)
{
  if (password.empty())
    return false;

  std::string plain = server_id;
  plain.push_back(static_cast<char>(Checksum(plain)));

  std::string scrambled;
  scrambled.reserve(plain.size() * 2);
  u8 position = 0;
  for (const char c : plain)
  {
    const u8 mixed =
        static_cast<u8>((Byte(c) ^ Byte(password[position % password.size()])) + position);
    scrambled.push_back(Char(u32{'A'} + (mixed >> 4)));
    scrambled.push_back(Char(u32{'A'} + (mixed & 0x0F)));
    ++position;
  }

  server_id = std::move(scrambled);
  return true;
}

std::optional<std::string> NetPlaySession::DecryptID(std::string_view password) const
{
  // Needs a password and a non-empty, even number of letters.
  if (password.empty() || server_id.empty() || server_id.size() % 2 != 0)
    return std::nullopt;

  std::string plain;
  plain.reserve(server_id.size() / 2);
  u8 position = 0;
  for (std::size_t i = 0; i < server_id.size(); i += 2)
  {
    // Letters outside 'A'-'P' are not rejected; they decode to whatever bits they give.
    const u32 high = static_cast<u32>(server_id[i] - 'A');
    const u32 low = static_cast<u32>(server_id[i + 1] - 'A');
    const u8 mixed = static_cast<u8>((high << 4) | low);
    plain.push_back(
        Char(static_cast<u8>(mixed - position) ^ Byte(password[position % password.size()])));
    ++position;
  }

  const u8 stored_checksum = Byte(plain.back());
  plain.pop_back();
  if (Checksum(plain) != stored_checksum)
    return std::nullopt;
  return plain;
}
