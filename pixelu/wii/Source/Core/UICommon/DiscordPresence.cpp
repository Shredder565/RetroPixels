// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "UICommon/DiscordPresence.h"

#include <string>

#include <fmt/format.h>

#include "Core/Config/NetplaySettings.h"
#include "Core/Config/UISettings.h"
#include "Core/ConfigManager.h"

#ifdef USE_DISCORD_PRESENCE
#include <chrono>

#include <discord_rpc.h>

#include "Common/CommonTypes.h"
#include "Common/Hash.h"

#include "Core/AchievementManager.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/System.h"
#endif

namespace Discord
{
#ifdef USE_DISCORD_PRESENCE
namespace
{
constexpr char LOGO_IMAGE[] = "dolphin_logo";
constexpr char LOGO_TEXT[] = "Dolphin is an emulator for the GameCube and the Wii.";
constexpr int FULL_PARTY = 4;

Handler* s_handler = nullptr;
// Set once Discord reports the signed-in user; used as the default NetPlay nickname.
const char* s_discord_name = "";
bool s_custom_app = false;

s64 SecondsSinceEpoch()
{
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

// Shown as "elapsed" in Discord; reset when a new game starts.
s64 s_session_start = SecondsSinceEpoch();

bool Enabled()
{
  return Config::Get(Config::MAIN_USE_DISCORD_PRESENCE);
}

void OnReady(const DiscordUser* user)
{
  s_discord_name = user->username;
}

void OnJoinRequest(const DiscordUser* user)
{
  if (s_handler == nullptr)
    return;

  const std::string tag = fmt::format("{}#{}", user->username, user->discriminator);
  s_handler->DiscordJoinRequest(user->userId, tag, user->avatar);
}

// The secret is "<SecretType number>\n<payload>". The payload is "<host>:<port>" for a direct
// connection or a traversal room code.
void OnJoin(const char* join_secret)
{
  if (s_handler == nullptr)
    return;

  if (Config::IsDefaultValue(Config::NETPLAY_NICKNAME))
    Config::SetCurrent(Config::NETPLAY_NICKNAME, s_discord_name);

  const std::string secret(join_secret);
  const std::string type_text = secret.substr(0, secret.find('\n'));
  const std::size_t payload_start = type_text.size() + 1;
  const auto type = static_cast<SecretType>(std::stol(type_text));

  if (type == SecretType::IPAddress)
  {
    // SetCurrent rather than SetBaseOrCurrent: a joined address should not be saved.
    Config::SetCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, "direct");

    // Everything up to the last ':' is the host (no ':' at all: the whole payload).
    const std::size_t host_length = secret.find_last_of(':') - payload_start;
    const std::string host = secret.substr(payload_start, host_length);
    Config::SetCurrent(Config::NETPLAY_ADDRESS, host);

    const std::size_t after_host = payload_start + host.size();
    if (secret[after_host] == ':')
    {
      const unsigned long port = std::stoul(secret.substr(after_host + 1));
      Config::SetCurrent(Config::NETPLAY_CONNECT_PORT, static_cast<u16>(port));
    }
  }
  else if (type == SecretType::RoomID)
  {
    Config::SetCurrent(Config::NETPLAY_TRAVERSAL_CHOICE, "traversal");
    Config::SetCurrent(Config::NETPLAY_HOST_CODE, secret.substr(payload_start));
  }
  else
  {
    return;
  }

  s_handler->DiscordJoin();
}

std::string CoverArtUrl()
{
  const SConfig& config = SConfig::GetInstance();
  const std::string region =
      config.GetGameTDBImageRegionCode(Core::System::GetInstance().IsWii(), config.m_region);
  return fmt::format("https://discord.dolphin-emu.org/cover-art/{}/{}.png", region,
                     config.GetGameTDBID());
}
}  // namespace
#endif

Handler::~Handler() = default;

void Init()
{
#ifdef USE_DISCORD_PRESENCE
  if (!Enabled())
    return;

  DiscordEventHandlers callbacks{};
  callbacks.ready = OnReady;
  callbacks.joinRequest = OnJoinRequest;
  callbacks.joinGame = OnJoin;
  Discord_Initialize(DEFAULT_CLIENT_ID.c_str(), &callbacks, 1, nullptr);
  UpdateDiscordPresence();
#endif
}

void UpdateClientID(const std::string& new_client)
{
#ifdef USE_DISCORD_PRESENCE
  if (!Enabled())
    return;

  s_custom_app = new_client != DEFAULT_CLIENT_ID;
  Shutdown();

  // The default application gets its event callbacks back through Init; a custom one has none.
  if (s_custom_app)
    Discord_Initialize(new_client.c_str(), nullptr, 0, nullptr);
  else
    Init();
#else
  (void)new_client;
#endif
}

void CallPendingCallbacks()
{
#ifdef USE_DISCORD_PRESENCE
  if (Enabled())
    Discord_RunCallbacks();
#endif
}

void InitNetPlayFunctionality(Handler& handler)
{
#ifdef USE_DISCORD_PRESENCE
  s_handler = &handler;
#else
  (void)handler;
#endif
}

bool UpdateDiscordPresenceRaw(const std::string& details, const std::string& state,
                              const std::string& large_image_key,
                              const std::string& large_image_text,
                              const std::string& small_image_key,
                              const std::string& small_image_text, const int64_t start_timestamp,
                              const int64_t end_timestamp, const int party_size,
                              const int party_max)
{
#ifdef USE_DISCORD_PRESENCE
  // Raw presence is for homebrew that brought its own application; the default one is ours.
  if (!Enabled() || !s_custom_app)
    return false;

  DiscordRichPresence presence{};
  presence.details = details.c_str();
  presence.state = state.c_str();
  presence.largeImageKey = large_image_key.c_str();
  presence.largeImageText = large_image_text.c_str();
  presence.smallImageKey = small_image_key.c_str();
  presence.smallImageText = small_image_text.c_str();
  presence.startTimestamp = start_timestamp;
  presence.endTimestamp = end_timestamp;
  presence.partySize = party_size;
  presence.partyMax = party_max;
  Discord_UpdatePresence(&presence);
  return true;
#else
  (void)details, (void)state, (void)large_image_key, (void)large_image_text;
  (void)small_image_key, (void)small_image_text, (void)start_timestamp, (void)end_timestamp;
  (void)party_size, (void)party_max;
  return false;
#endif
}

void UpdateDiscordPresence(int party_size, SecretType type, const std::string& secret,
                           const std::string& current_game, bool reset_timer)
{
#ifdef USE_DISCORD_PRESENCE
  if (!Enabled())
    return;

  // Homebrew may have switched to its own application; return to ours first.
  if (s_custom_app)
    UpdateClientID(DEFAULT_CLIENT_ID);

  const SConfig& config = SConfig::GetInstance();
  const std::string& title = current_game.empty() ? config.GetTitleDescription() : current_game;
  const std::string cover = config.GetGameTDBID().empty() ? std::string() : CoverArtUrl();

  if (reset_timer)
    s_session_start = SecondsSinceEpoch();

  // Every string the presence points at must outlive the Discord_UpdatePresence call below.
  DiscordRichPresence presence{};
  presence.details = title.empty() ? "Not in-game" : title.c_str();
  presence.startTimestamp = s_session_start;

  // With cover art the art is the large image and the logo the small one; without, the logo is
  // the large image.
  if (cover.empty())
  {
    presence.largeImageKey = LOGO_IMAGE;
    presence.largeImageText = LOGO_TEXT;
  }
  else
  {
    presence.largeImageKey = cover.c_str();
    presence.largeImageText = title.c_str();
    presence.smallImageKey = LOGO_IMAGE;
    presence.smallImageText = LOGO_TEXT;
  }

  std::string achievement_status;
  if (party_size > 0)
  {
    // A full party is still joinable, as a spectator, so it gets no maximum.
    presence.partySize = party_size;
    if (party_size < FULL_PARTY)
    {
      presence.state = "In a party";
      presence.partyMax = FULL_PARTY;
    }
    else
    {
      presence.state = "In a full party";
    }
  }
#ifdef USE_RETRO_ACHIEVEMENTS
  else if (Config::Get(Config::RA_ENABLED) && Config::Get(Config::RA_DISCORD_PRESENCE_ENABLED))
  {
    // The state text is limited to 127 characters.
    achievement_status = AchievementManager::GetInstance().GetRichPresence().data();
    if (achievement_status.size() >= 128)
    {
      achievement_status.resize(124);
      achievement_status += "...";
    }
    presence.state = achievement_status.c_str();
  }
#endif

  // The party ID is a checksum of the secret, so everyone joining via the same secret matches.
  std::string party_id;
  std::string join_secret;
  if (type != SecretType::Empty)
  {
    party_id = std::to_string(
        Common::HashAdler32(reinterpret_cast<const u8*>(secret.data()), secret.size()));
    join_secret = fmt::format("{}\n{}", static_cast<int>(type), secret);
  }
  presence.partyId = party_id.c_str();
  presence.joinSecret = join_secret.c_str();

  Discord_UpdatePresence(&presence);
#else
  (void)party_size, (void)type, (void)secret, (void)current_game, (void)reset_timer;
#endif
}

std::string CreateSecretFromIPAddress(const std::string& ip_address, int port)
{
  return fmt::format("{}:{}", ip_address, port);
}

void Shutdown()
{
#ifdef USE_DISCORD_PRESENCE
  if (!Enabled())
    return;

  Discord_ClearPresence();
  Discord_Shutdown();
#endif
}

void SetDiscordPresenceEnabled(bool enabled)
{
  if (Config::Get(Config::MAIN_USE_DISCORD_PRESENCE) == enabled)
    return;

  // Leave the old state cleanly, then enter the new one.
  if (Config::Get(Config::MAIN_USE_DISCORD_PRESENCE))
    Shutdown();
  Config::SetBase(Config::MAIN_USE_DISCORD_PRESENCE, enabled);
  if (Config::Get(Config::MAIN_USE_DISCORD_PRESENCE))
    Init();
}

}  // namespace Discord
