// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Discord Rich Presence: shows the running game and lets friends join NetPlay sessions. Every
// call is a no-op unless the build has USE_DISCORD_PRESENCE and the user enabled the feature.
namespace Discord
{
// Discord application ID the presence is published under (it selects the app name and images).
const std::string DEFAULT_CLIENT_ID = "455712169795780630";

// Receives NetPlay join events.
class Handler
{
public:
  virtual ~Handler();
  virtual void DiscordJoin() = 0;
  virtual void DiscordJoinRequest(const char* id, const std::string& discord_tag,
                                  const char* avatar) = 0;
};

// What a join secret carries; the number is the secret's first line.
enum class SecretType
{
  Empty,
  IPAddress,
  RoomID,
};

void Init();
void InitNetPlayFunctionality(Handler& handler);
void CallPendingCallbacks();
// Switches to another application ID (for homebrew that sets its own); empty or the default ID
// returns to the default application.
void UpdateClientID(const std::string& new_client = {});
// Publishes caller-supplied presence; only allowed while a custom application ID is active.
bool UpdateDiscordPresenceRaw(const std::string& details = {}, const std::string& state = {},
                              const std::string& large_image_key = {},
                              const std::string& large_image_text = {},
                              const std::string& small_image_key = {},
                              const std::string& small_image_text = {},
                              const int64_t start_timestamp = 0, const int64_t end_timestamp = 0,
                              const int party_size = 0, const int party_max = 0);
void UpdateDiscordPresence(int party_size = 0, SecretType type = SecretType::Empty,
                           const std::string& secret = {}, const std::string& current_game = {},
                           const bool reset_timer = false);
// "<address>:<port>".
std::string CreateSecretFromIPAddress(const std::string& ip_address, int port);
void Shutdown();
void SetDiscordPresenceEnabled(bool enabled);
}  // namespace Discord
