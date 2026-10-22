// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "Common/Event.h"

// One entry of the public NetPlay session list.
struct NetPlaySession
{
  std::string name;
  std::string region;
  std::string method;
  std::string server_id;
  std::string game_id;
  std::string version;

  int player_count = 0;
  int port = 0;

  bool has_password = false;
  bool in_game = false;

  // Password-protected sessions publish their server ID scrambled with the password, so only
  // players who know it can connect. This is obfuscation, not real encryption.
  bool EncryptID(std::string_view password);
  std::optional<std::string> DecryptID(std::string_view password) const;
};

// Client for the NetPlay session index server (Config::NETPLAY_INDEX_URL). A host adds its
// session, and a background thread then reports its state every few seconds until it is removed.
class NetPlayIndex
{
public:
  explicit NetPlayIndex();
  ~NetPlayIndex();

  std::optional<std::vector<NetPlaySession>>
  List(const std::map<std::string, std::string>& filters = {});

  // Region codes with their (translatable) names.
  static std::vector<std::pair<std::string, std::string>> GetRegions();

  bool Add(const NetPlaySession& session);
  void Remove();

  bool HasActiveSession() const;

  void SetPlayerCount(int player_count);
  void SetInGame(bool in_game);
  void SetGame(std::string game);

  // The server's status text for the last failure, or NO_RESPONSE / BAD_JSON.
  const std::string& GetLastError() const;

  // Called from the background thread when the server drops the session.
  void SetErrorCallback(std::function<void()> callback);

private:
  void KeepAlive();
  void StopKeepAlive();

  std::string m_secret;
  std::string m_game;
  int m_player_count = 0;
  bool m_in_game = false;

  std::string m_last_error;
  std::thread m_keep_alive;
  Common::Event m_stop_keep_alive;

  std::function<void()> m_error_callback = nullptr;
};
