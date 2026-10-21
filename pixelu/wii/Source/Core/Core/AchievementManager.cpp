// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#ifdef USE_RETRO_ACHIEVEMENTS

#include "Core/AchievementManager.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <thread>

#include <fmt/format.h>

#include "Common/BitUtils.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Common/WorkQueueThread.h"
#include "Core/AchievementApprovedHash.h"
#include "Core/ActionReplay.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"
#include "DiscIO/Blob.h"
#include "UICommon/DiscordPresence.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoEvents.h"

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
#include <libloaderapi.h>
#include <rcheevos/include/rc_client_raintegration.h>
#include <shlwapi.h>
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

namespace
{
// Every request to the RetroAchievements servers identifies the emulator build.
const Common::HttpRequest::Headers& RequestHeaders()
{
#ifdef ANDROID
  static const Common::HttpRequest::Headers headers{
      {"User-Agent", Common::GetUserAgentStr() + " (Android)"}};
#else
  static const Common::HttpRequest::Headers headers{{"User-Agent", Common::GetUserAgentStr()}};
#endif
  return headers;
}

using Clock = std::chrono::steady_clock;

// The colour of a message depends on whether the player is in hardcore mode.
u32 ModeColor(const rc_client_t* client)
{
  return rc_client_get_hardcore_enabled(client) ? OSD::Color::YELLOW : OSD::Color::CYAN;
}

void LongMessage(std::string text, u32 color, const AchievementManager::Badge* icon = nullptr)
{
  OSD::AddMessage(std::move(text), OSD::Duration::VERY_LONG, color, icon);
}
}  // namespace

AchievementManager& AchievementManager::GetInstance()
{
  static AchievementManager s_instance;
  return s_instance;
}

// ---------------------------------------------------------------------------------------------
// Start-up, login and shut-down

void AchievementManager::Init([[maybe_unused]] void* hwnd)
{
  LoadDefaultBadges();
  if (m_client != nullptr || !Config::Get(Config::RA_ENABLED))
    return;

  {
    std::lock_guard guard{m_lock};
    m_client = rc_client_create(MemoryPeeker, Request);
  }

  const std::string host = Config::Get(Config::RA_HOST_URL);
  if (!host.empty())
    rc_client_set_host(m_client, host.c_str());
  rc_client_set_event_handler(m_client, EventHandler);
  rc_client_enable_logging(m_client, RC_CLIENT_LOG_LEVEL_VERBOSE,
                           [](const char* text, const rc_client_t*) {
                             INFO_LOG_FMT(ACHIEVEMENTS, "{}", text);
                           });

  // Hardcore mode follows the setting, both now and whenever the configuration changes.
  m_config_changed_callback_id = Config::AddConfigChangedCallback([this] { SetHardcoreMode(); });
  SetHardcoreMode();

  m_queue.Reset("AchievementManagerQueue");
  m_image_queue.Reset("AchievementManagerImageQueue");

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  // The integration DLL sits next to the executable (RA_Integration-x64.dll or RA_Integration.dll
  // on x64, RA_Integration.dll elsewhere). Logging in happens once its loading has finished.
  rc_client_begin_load_raintegration(m_client, UTF8ToWString(File::GetExeDirectory()).c_str(),
                                     reinterpret_cast<HWND>(hwnd), "Dolphin",
                                     Common::GetScmDescStr().c_str(), LoadIntegrationCallback,
                                     nullptr);
#else
  if (HasAPIToken())
    Login("");
  INFO_LOG_FMT(ACHIEVEMENTS, "Achievement Manager Initialized");
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
}

picojson::value AchievementManager::LoadApprovedList()
{
  const std::string path =
      fmt::format("{}{}{}", File::GetSysDirectory(), DIR_SEP, ACHIEVEMENT_APPROVED_LIST_FILENAME);

  picojson::value list;
  std::string problem;
  if (!JsonFromFile(path, &list, &problem))
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to load approved game settings list {}",
                 ACHIEVEMENT_APPROVED_LIST_FILENAME);
    WARN_LOG_FMT(ACHIEVEMENTS, "Error: {}", problem);
    return {};
  }

  // The list only counts when it is exactly the one this build shipped with.
  auto sha1 = Common::SHA1::CreateContext();
  sha1->Update(list.serialize());
  const Common::SHA1::Digest found = sha1->Finish();
  if (found == ACHIEVEMENT_APPROVED_LIST_HASH)
    return list;

  WARN_LOG_FMT(ACHIEVEMENTS, "Failed to verify approved game settings list {}",
               ACHIEVEMENT_APPROVED_LIST_FILENAME);
  WARN_LOG_FMT(ACHIEVEMENTS, "Expected hash {}, found hash {}",
               Common::SHA1::DigestToString(ACHIEVEMENT_APPROVED_LIST_HASH),
               Common::SHA1::DigestToString(found));
  return {};
}

void AchievementManager::Login(const std::string& password)
{
  if (m_client == nullptr)
  {
    ERROR_LOG_FMT(
        ACHIEVEMENTS,
        "Attempted login to RetroAchievements server without achievement client initialized.");
    return;
  }

  const std::string username = Config::Get(Config::RA_USERNAME);
  if (!password.empty())
  {
    rc_client_begin_login_with_password(m_client, username.c_str(), password.c_str(),
                                        LoginCallback, nullptr);
    return;
  }
  // No password: reuse the token of an earlier session.
  const std::string token = Config::Get(Config::RA_API_TOKEN);
  rc_client_begin_login_with_token(m_client, username.c_str(), token.c_str(), LoginCallback,
                                   nullptr);
}

bool AchievementManager::HasAPIToken() const
{
  return !Config::Get(Config::RA_API_TOKEN).empty();
}

void AchievementManager::LoginCallback(int result, const char*, rc_client_t* client, void*)
{
  AchievementManager& manager = GetInstance();
  if (result != RC_OK)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to login {} to RetroAchievements server.",
                 Config::Get(Config::RA_USERNAME));
    Config::SetBaseOrCurrent(Config::RA_API_TOKEN, "");
    manager.update_event.Trigger({.failed_login_code = result});
    manager.login_event.Trigger(result);
    return;
  }

  const rc_client_user_t* account = nullptr;
  {
    std::lock_guard guard{manager.GetLock()};
    account = rc_client_get_user_info(client);
  }
  if (account == nullptr)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to retrieve user information from client.");
    manager.update_event.Trigger({.failed_login_code = RC_INVALID_STATE});
    manager.login_event.Trigger(RC_INVALID_STATE);
    return;
  }

  // The server may know the player under a different spelling; adopt it.
  const std::string typed_name = Config::Get(Config::RA_USERNAME);
  if (typed_name != account->username)
  {
    INFO_LOG_FMT(ACHIEVEMENTS, "Username alias {} -> {}.", typed_name, account->username);
    Config::SetBaseOrCurrent(Config::RA_USERNAME, account->username);
  }
  manager.login_event.Trigger(RC_OK);
  INFO_LOG_FMT(ACHIEVEMENTS, "Successfully logged in {} to RetroAchievements server.",
               account->username);

  std::lock_guard guard{manager.GetLock()};
  Config::SetBaseOrCurrent(Config::RA_API_TOKEN, account->token);
  manager.FetchPlayerBadge();
}

void AchievementManager::Logout()
{
  CloseGame();
  {
    std::lock_guard guard{m_lock};
    m_player_badge.width = 0;
    m_player_badge.height = 0;
    m_player_badge.data.clear();
    Config::SetBaseOrCurrent(Config::RA_API_TOKEN, "");
  }
  update_event.Trigger(UpdatedItems{.all = true});
  INFO_LOG_FMT(ACHIEVEMENTS, "Logged out from server.");
}

void AchievementManager::Shutdown()
{
  if (m_client == nullptr)
    return;

  CloseGame();
  m_queue.Shutdown();
  Config::RemoveConfigChangedCallback(m_config_changed_callback_id);

  // The stored token stays, so the next run logs in again by itself.
  std::lock_guard guard{m_lock};
  rc_client_destroy(m_client);
  m_client = nullptr;
  m_dll_found = false;
  INFO_LOG_FMT(ACHIEVEMENTS, "Achievement Manager shut down.");
}

// ---------------------------------------------------------------------------------------------
// Games and discs

static rc_hash_filereader MakeVolumeReader(void* (*open)(const char*),
                                           void (*seek)(void*, int64_t, int),
                                           int64_t (*tell)(void*),
                                           size_t (*read)(void*, void*, size_t),
                                           void (*close)(void*))
{
  rc_hash_filereader reader{};
  reader.open = open;
  reader.seek = seek;
  reader.tell = tell;
  reader.read = read;
  reader.close = close;
  return reader;
}

void AchievementManager::LoadGame(const DiscIO::Volume* volume)
{
  if (!Config::Get(Config::RA_ENABLED) || !HasAPIToken())
    return;
  if (m_client == nullptr)
  {
    ERROR_LOG_FMT(ACHIEVEMENTS,
                  "Attempted to load game achievements without achievement client initialized.");
    return;
  }

  if (volume == nullptr)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Software format unsupported by AchievementManager.");
    if (rc_client_get_game_info(m_client) == nullptr)
    {
      // Nothing is loaded yet: report a game without a hash to the client.
      rc_client_begin_load_game(m_client, "", LoadGameCallback, nullptr);
      return;
    }
    OSD::AddMessage("Unsupported media change; disabling achievements.", OSD::Duration::VERY_LONG,
                    OSD::Color::RED);
    CloseGame();
    return;
  }

  rc_client_set_unofficial_enabled(m_client, Config::Get(Config::RA_UNOFFICIAL_ENABLED));
  rc_client_set_encore_mode_enabled(m_client, Config::Get(Config::RA_ENCORE_ENABLED));
  rc_client_set_spectator_mode_enabled(m_client, Config::Get(Config::RA_SPECTATOR_ENABLED));

  {
    std::lock_guard guard{m_lock};
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
    // RAIntegration asks for a title; prefer the English one.
    const auto& titles = volume->GetLongNames();
    const auto english = titles.find(DiscIO::Language::English);
    if (english != titles.end())
      m_title_estimate = english->second;
    else
      m_title_estimate = titles.empty() ? std::string{} : titles.begin()->second;
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
    // The hasher reads the disc through its own copy of the volume (see FilereaderOpen).
    if (m_loading_volume == nullptr)
      m_loading_volume = DiscIO::CreateVolume(volume->GetBlobReader().CopyReader());
  }

  std::lock_guard reader_guard{m_filereader_lock};
  rc_hash_filereader reader = MakeVolumeReader(&FilereaderOpen, &FilereaderSeek, &FilereaderTell,
                                               &FilereaderRead, &FilereaderClose);
  rc_hash_init_custom_filereader(&reader);

  if (rc_client_get_game_info(m_client) != nullptr)
  {
    // A game is running already, so this is a disc swap.
    rc_client_begin_identify_and_change_media(m_client, "", nullptr, 0, ChangeMediaCallback,
                                              nullptr);
    return;
  }
  const u32 console = FindConsoleID(volume->GetVolumeType());
  rc_client_begin_identify_and_load_game(m_client, console, "", nullptr, 0, LoadGameCallback,
                                         nullptr);
}

void AchievementManager::ChangeDisc(const DiscIO::Volume* volume)
{
  if (volume != nullptr && volume->GetGameID() == SConfig::GetInstance().GetGameID())
  {
    INFO_LOG_FMT(ACHIEVEMENTS, "Inserting disc.");
    LoadGame(volume);
    return;
  }
  if (volume == nullptr)
    INFO_LOG_FMT(ACHIEVEMENTS, "Ejecting disc.");
  else
    INFO_LOG_FMT(ACHIEVEMENTS, "Inserting disc that doesn't belong to the running game.");
  LoadGame(nullptr);
}

bool AchievementManager::IsGameLoaded() const
{
  const rc_client_game_t* game = rc_client_get_game_info(m_client);
  return game != nullptr && game->id != 0;
}

std::string AchievementManager::CalculateHash(const std::string& file_path)
{
  AchievementManager& manager = GetInstance();
  manager.m_loading_volume = DiscIO::CreateVolume(file_path);

  rc_hash_filereader reader = MakeVolumeReader(&FilereaderOpen, &FilereaderSeek, &FilereaderTell,
                                               &FilereaderRead, &FilereaderClose);
  rc_hash_init_custom_filereader(&reader);

  char digest_text[33] = "0";
  const u32 console = FindConsoleID(manager.m_loading_volume->GetVolumeType());
  rc_hash_generate_from_file(digest_text, console, file_path.c_str());
  return std::string(digest_text);
}

u32 AchievementManager::FindConsoleID(const DiscIO::Platform& platform)
{
  if (platform == DiscIO::Platform::GameCubeDisc)
    return RC_CONSOLE_GAMECUBE;
  if (platform == DiscIO::Platform::WiiDisc || platform == DiscIO::Platform::WiiWAD)
    return RC_CONSOLE_WII;
  return RC_CONSOLE_UNKNOWN;
}

void AchievementManager::LoadGameCallback(int result, const char*, rc_client_t* client, void*)
{
  AchievementManager& manager = GetInstance();
  manager.m_loading_volume.reset();

  switch (result)
  {
  case RC_API_FAILURE:
    WARN_LOG_FMT(ACHIEVEMENTS, "Load data request rejected for old Dolphin version.");
    LongMessage("RetroAchievements no longer supports this version of Dolphin.", OSD::Color::RED);
    LongMessage("Please update Dolphin to a newer version.", OSD::Color::RED);
    return;
  case RC_LOGIN_REQUIRED:
  case RC_INVALID_CREDENTIALS:
  case RC_EXPIRED_TOKEN:
    WARN_LOG_FMT(ACHIEVEMENTS, "Invalid/expired RetroAchievements API token.");
    LongMessage(
        "You have been logged out from RetroAchievements due to invalid/expired credentials.",
        OSD::Color::RED);
    LongMessage("Please close the game to log back in before continuing.", OSD::Color::RED);
    Config::SetBaseOrCurrent(Config::RA_API_TOKEN, "");
    manager.update_event.Trigger(UpdatedItems{.failed_login_code = result});
    return;
  default:
    break;
  }

  const rc_client_game_t* game = rc_client_get_game_info(client);
  if (result != RC_OK)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to load data for current game.");
    LongMessage("Achievements are not supported for this title.", OSD::Color::RED);
  }
  else if (game == nullptr)
  {
    ERROR_LOG_FMT(ACHIEVEMENTS, "Failed to retrieve game information from client.");
    LongMessage("Failed to load achievements for this title.", OSD::Color::RED);
  }
  else
  {
    INFO_LOG_FMT(ACHIEVEMENTS, "Loaded data for game ID {}.", game->id);
    manager.m_display_welcome_message = true;
  }
  if (game == nullptr)
    return;

  manager.FetchGameBadges();
  manager.m_system.store(&Core::System::GetInstance(), std::memory_order_release);
  manager.update_event.Trigger({.all = true});
  // Backdate the last rich presence update so the next frame refreshes it.
  manager.m_last_rp_time = Clock::now() - std::chrono::minutes{2};

  std::lock_guard guard{manager.GetLock()};
  rc_client_leaderboard_list_t* boards =
      rc_client_create_leaderboard_list(client, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
  for (const rc_client_leaderboard_bucket_t& bucket :
       std::span(boards->buckets, boards->num_buckets))
  {
    for (const rc_client_leaderboard_t* board :
         std::span(bucket.leaderboards, bucket.num_leaderboards))
    {
      LeaderboardStatus status;
      status.name = board->title;
      status.description = board->description;
      manager.m_leaderboard_map.emplace(board->id, std::move(status));
    }
  }
  rc_client_destroy_leaderboard_list(boards);
}

void AchievementManager::ChangeMediaCallback(int result, const char* error_message,
                                             rc_client_t*, void*)
{
  GetInstance().m_loading_volume.reset();
  if (result == RC_OK)
    return;
  if (result == RC_HARDCORE_DISABLED)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Hardcore disabled. Unrecognized media inserted.");
    return;
  }
  const char* reason = error_message != nullptr ? error_message : rc_error_str(result);
  ERROR_LOG_FMT(ACHIEVEMENTS, "RetroAchievements media change failed: {}", reason);
}

void AchievementManager::CloseGame()
{
  m_queue.Cancel();
  m_image_queue.Cancel();
  {
    std::lock_guard guard{m_lock};
    m_active_challenges.clear();
    m_active_leaderboards.clear();
    m_game_badge.width = 0;
    m_game_badge.height = 0;
    m_game_badge.data.clear();
    m_unlocked_badges.clear();
    m_locked_badges.clear();
    m_leaderboard_map.clear();
    m_rich_presence.fill('\0');
    m_system.store(nullptr, std::memory_order_release);
    if (Config::Get(Config::RA_DISCORD_PRESENCE_ENABLED))
      Discord::UpdateDiscordPresence();
    if (rc_client_get_game_info(m_client) != nullptr)
      rc_client_unload_game(m_client);
    INFO_LOG_FMT(ACHIEVEMENTS, "Game closed.");
  }
  update_event.Trigger(UpdatedItems{.all = true});
}

void AchievementManager::DisplayWelcomeMessage()
{
  std::lock_guard guard{m_lock};
  m_display_welcome_message = false;
  const u32 color = ModeColor(m_client);

  LongMessage("", OSD::Color::GREEN, &GetGameBadge());
  const rc_client_game_t* game = rc_client_get_game_info(m_client);
  if (game == nullptr)
  {
    ERROR_LOG_FMT(ACHIEVEMENTS, "Attempting to welcome player to game not running.");
    return;
  }
  LongMessage(game->title, OSD::Color::GREEN);

  rc_client_user_game_summary_t progress;
  rc_client_get_user_game_summary(m_client, &progress);
  LongMessage(fmt::format("You have {}/{} achievements worth {}/{} points",
                          progress.num_unlocked_achievements, progress.num_core_achievements,
                          progress.points_unlocked, progress.points_core),
              color);
  if (progress.num_unsupported_achievements > 0)
  {
    LongMessage(fmt::format("{} achievements unsupported", progress.num_unsupported_achievements),
                OSD::Color::RED);
  }
  LongMessage(fmt::format("Hardcore mode is {}",
                          rc_client_get_hardcore_enabled(m_client) ? "ON" : "OFF"),
              color);
  LongMessage(fmt::format("Leaderboard submissions are {}",
                          rc_client_get_hardcore_enabled(m_client) ? "ON" : "OFF"),
              color);
}

// ---------------------------------------------------------------------------------------------
// Per-frame work, pausing and idling

void AchievementManager::DoFrame()
{
  if (!IsGameLoaded() && !m_dll_found)
    return;
  if (!Core::IsCPUThread())
    return;

  {
    std::lock_guard guard{m_lock};
    rc_client_do_frame(m_client);
  }

  // Rich presence is refreshed at most every ten seconds.
  const auto now = Clock::now();
  if (now - m_last_rp_time <= std::chrono::seconds{10})
    return;
  m_last_rp_time = now;
  rc_client_get_rich_presence_message(m_client, m_rich_presence.data(), RP_SIZE);
  update_event.Trigger(UpdatedItems{.rich_presence = true});
  if (Config::Get(Config::RA_DISCORD_PRESENCE_ENABLED))
    Discord::UpdateDiscordPresence();
}

bool AchievementManager::CanPause()
{
  if (!IsGameLoaded())
    return true;

  u32 frames_left = 0;
  const bool allowed = rc_client_can_pause(m_client, &frames_left);
  if (allowed)
    return true;

  const auto refresh_rate = Core::System::GetInstance().GetVideoInterface().GetTargetRefreshRate();
  LongMessage(fmt::format("RetroAchievements Hardcore Mode:\n"
                          "Cannot pause until another {:.2f} seconds have passed.",
                          static_cast<float>(frames_left) / refresh_rate),
              OSD::Color::RED);
  return false;
}

void AchievementManager::DoIdle()
{
  // While paused, rich presence is still recalculated once a second. The client reads emulated
  // memory for that, so the actual call runs as a host job.
  const auto still_idle = [this](Core::System* system) {
    return system != nullptr && Core::GetState(*system) == Core::State::Paused &&
           m_background_execution_allowed && m_client != nullptr && IsGameLoaded();
  };

  std::thread([this, still_idle] {
    for (;;)
    {
      Common::SleepCurrentThread(1000);
      {
        std::lock_guard guard{m_lock};
        if (!still_idle(m_system.load(std::memory_order_acquire)))
          return;
      }
      Core::QueueHostJob([this, still_idle](Core::System& system) {
        std::lock_guard guard{m_lock};
        if (still_idle(&system))
          rc_client_idle(m_client);
      });
    }
  }).detach();
}

void AchievementManager::SetBackgroundExecutionAllowed(bool allowed)
{
  m_background_execution_allowed = allowed;

  Core::System* const system = m_system.load(std::memory_order_acquire);
  if (system != nullptr && allowed && Core::GetState(*system) == Core::State::Paused)
    DoIdle();
}

std::recursive_mutex& AchievementManager::GetLock()
{
  return m_lock;
}

// ---------------------------------------------------------------------------------------------
// Hardcore mode and approved codes

void AchievementManager::SetHardcoreMode()
{
  const bool hardcore = Config::Get(Config::RA_HARDCORE_ENABLED);
  rc_client_set_hardcore_enabled(m_client, hardcore);
  if (!hardcore)
    return;
  // Hardcore forbids slow motion and free look.
  if (Config::Get(Config::MAIN_EMULATION_SPEED) < 1.0f)
    Config::SetBaseOrCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
  Config::SetBaseOrCurrent(Config::FREE_LOOK_ENABLED, false);
}

bool AchievementManager::IsHardcoreModeActive() const
{
  std::lock_guard guard{m_lock};
  if (!rc_client_get_hardcore_enabled(m_client))
    return false;
  // Before a game is known, hardcore is treated as active.
  if (rc_client_get_game_info(m_client) == nullptr)
    return true;
  return rc_client_is_processing_required(m_client);
}

static void HashValue(Common::SHA1::Context& sha1, const auto& value)
{
  sha1.Update(Common::BitCastToArray<u8>(value));
}

Common::SHA1::Digest AchievementManager::GetCodeHash(const PatchEngine::Patch& patch) const
{
  auto sha1 = Common::SHA1::CreateContext();
  HashValue(*sha1, static_cast<u64>(patch.entries.size()));
  for (const PatchEngine::PatchEntry& line : patch.entries)
  {
    HashValue(*sha1, line.type);
    HashValue(*sha1, line.address);
    HashValue(*sha1, line.value);
    HashValue(*sha1, line.comparand);
    HashValue(*sha1, line.conditional);
  }
  return sha1->Finish();
}

Common::SHA1::Digest AchievementManager::GetCodeHash(const Gecko::GeckoCode& code) const
{
  auto sha1 = Common::SHA1::CreateContext();
  HashValue(*sha1, static_cast<u64>(code.codes.size()));
  for (const Gecko::GeckoCode::Code& line : code.codes)
  {
    HashValue(*sha1, line.address);
    HashValue(*sha1, line.data);
  }
  return sha1->Finish();
}

Common::SHA1::Digest AchievementManager::GetCodeHash(const ActionReplay::ARCode& code) const
{
  auto sha1 = Common::SHA1::CreateContext();
  HashValue(*sha1, static_cast<u64>(code.ops.size()));
  for (const ActionReplay::AREntry& line : code.ops)
  {
    HashValue(*sha1, line.cmd_addr);
    HashValue(*sha1, line.value);
  }
  return sha1->Finish();
}

template <typename T>
bool AchievementManager::IsApprovedCode(const T& code, std::string_view game_id, u16 revision) const
{
  // An unverifiable list approves nothing.
  if (!m_ini_root->is<picojson::value::object>())
    return false;

  const std::string expected = Common::SHA1::DigestToString(GetCodeHash(code));
  for (const std::string& ini_name : ConfigLoaders::GetGameIniFilenames(game_id, revision))
  {
    // Entries are keyed by the ini name without its ".ini".
    const std::string key = ini_name.substr(0, ini_name.length() - 4);
    if (!m_ini_root->contains(key))
      continue;
    const picojson::value game_entry = m_ini_root->get(key);
    if (!game_entry.is<picojson::object>() || !game_entry.contains(code.name))
      continue;
    const picojson::value listed = game_entry.get(code.name);
    if (listed.template is<std::string>() && listed.template get<std::string>() == expected)
      return true;
  }
  return false;
}

template <typename T>
void AchievementManager::FilterApprovedIni(std::vector<T>& codes, std::string_view game_id,
                                           u16 revision) const
{
  if (codes.empty())
    return;

  std::lock_guard guard{m_lock};
  if (!IsHardcoreModeActive())
    return;

  if (!m_ini_root->is<picojson::value::object>())
  {
    codes.clear();
    return;
  }
  for (T& code : codes)
  {
    if (code.enabled && !IsApprovedCode(code, game_id, revision))
      code.enabled = false;
  }
}

template <typename T>
bool AchievementManager::ShouldCodeBeActivated(const T& code, std::string_view game_id,
                                               u16 revision) const
{
  if (!code.enabled)
    return false;
  if (!IsHardcoreModeActive())
    return true;
  if (!m_ini_root->is<picojson::value::object>())
    return false;

  INFO_LOG_FMT(ACHIEVEMENTS, "Verifying code {}", code.name);
  if (IsApprovedCode(code, game_id, revision))
    return true;

  LongMessage(fmt::format("Failed to verify code {} for game ID {}.", code.name, game_id),
              OSD::Color::RED);
  LongMessage("Disable hardcore mode to enable this code.", OSD::Color::RED);
  return false;
}

// Patches are only ever filtered as a whole list.
void AchievementManager::FilterApprovedPatches(std::vector<PatchEngine::Patch>& patches,
                                               std::string_view game_id, u16 revision) const
{
  FilterApprovedIni(patches, game_id, revision);
}

// Gecko codes
bool AchievementManager::IsApprovedGeckoCode(const Gecko::GeckoCode& code,
                                             std::string_view game_id, u16 revision) const
{
  return IsApprovedCode(code, game_id, revision);
}
bool AchievementManager::ShouldGeckoCodeBeActivated(const Gecko::GeckoCode& code,
                                                    std::string_view game_id, u16 revision) const
{
  return ShouldCodeBeActivated(code, game_id, revision);
}
void AchievementManager::FilterApprovedGeckoCodes(std::vector<Gecko::GeckoCode>& codes,
                                                  std::string_view game_id, u16 revision) const
{
  FilterApprovedIni(codes, game_id, revision);
}

// Action Replay codes
bool AchievementManager::IsApprovedARCode(const ActionReplay::ARCode& code,
                                          std::string_view game_id, u16 revision) const
{
  return IsApprovedCode(code, game_id, revision);
}
bool AchievementManager::ShouldARCodeBeActivated(const ActionReplay::ARCode& code,
                                                 std::string_view game_id, u16 revision) const
{
  return ShouldCodeBeActivated(code, game_id, revision);
}
void AchievementManager::FilterApprovedARCodes(std::vector<ActionReplay::ARCode>& codes,
                                               std::string_view game_id, u16 revision) const
{
  FilterApprovedIni(codes, game_id, revision);
}

void AchievementManager::SetSpectatorMode()
{
  rc_client_set_spectator_mode_enabled(m_client, Config::Get(Config::RA_SPECTATOR_ENABLED));
}

// ---------------------------------------------------------------------------------------------
// Queries for the UI

std::string_view AchievementManager::GetPlayerDisplayName() const
{
  const rc_client_user_t* account = HasAPIToken() ? rc_client_get_user_info(m_client) : nullptr;
  return account != nullptr ? std::string_view(account->display_name) : std::string_view("");
}

u32 AchievementManager::GetPlayerScore() const
{
  const rc_client_user_t* account = HasAPIToken() ? rc_client_get_user_info(m_client) : nullptr;
  return account != nullptr ? account->score : 0;
}

const AchievementManager::Badge& AchievementManager::GetPlayerBadge() const
{
  if (m_player_badge.data.empty())
    return m_default_player_badge;
  return m_player_badge;
}

std::string_view AchievementManager::GetGameDisplayName() const
{
  if (!IsGameLoaded())
    return "";
  return rc_client_get_game_info(m_client)->title;
}

rc_client_t* AchievementManager::GetClient()
{
  return m_client;
}

const AchievementManager::Badge& AchievementManager::GetGameBadge() const
{
  if (m_game_badge.data.empty())
    return m_default_game_badge;
  return m_game_badge;
}

const AchievementManager::Badge& AchievementManager::GetAchievementBadge(AchievementId id,
                                                                         bool locked) const
{
  const auto& fetched = locked ? m_locked_badges : m_unlocked_badges;
  if (const auto found = fetched.find(id); found != fetched.end() && !found->second.data.empty())
    return found->second;
  return locked ? m_default_locked_badge : m_default_unlocked_badge;
}

const AchievementManager::LeaderboardStatus*
AchievementManager::GetLeaderboardInfo(AchievementId leaderboard_id)
{
  const auto found = m_leaderboard_map.find(leaderboard_id);
  return found == m_leaderboard_map.end() ? nullptr : &found->second;
}

// Challenge indicators: the set shown, and whether it changed since the UI last looked.
const std::unordered_set<AchievementManager::AchievementId>&
AchievementManager::GetActiveChallenges() const
{
  return m_active_challenges;
}
bool AchievementManager::AreChallengesUpdated() const
{
  return m_challenges_updated;
}
void AchievementManager::ResetChallengesUpdated()
{
  m_challenges_updated = false;
}

AchievementManager::RichPresence AchievementManager::GetRichPresence() const
{
  return m_rich_presence;
}

std::vector<std::string> AchievementManager::GetActiveLeaderboards() const
{
  std::vector<std::string> shown;
  if (!Config::Get(Config::RA_LEADERBOARD_TRACKER_ENABLED))
    return shown;
  const size_t count = std::min<size_t>(MAX_DISPLAYED_LBOARDS, m_active_leaderboards.size());
  for (size_t i = 0; i < count; ++i)
    shown.emplace_back(m_active_leaderboards[i].display);
  return shown;
}

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
const rc_client_raintegration_menu_t* AchievementManager::GetDevelopmentMenu()
{
  return m_dll_found ? rc_client_raintegration_get_menu(m_client) : nullptr;
}

u32 AchievementManager::ActivateDevMenuItem(u32 menu_item_id)
{
  if (!m_dll_found)
    return 0;
  return static_cast<u32>(rc_client_raintegration_activate_menu_item(m_client, menu_item_id));
}
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

// ---------------------------------------------------------------------------------------------
// Savestates: a size followed by the client's serialized progress.

void AchievementManager::DoState(PointerWrap& p)
{
  if (m_client == nullptr || !Config::Get(Config::RA_ENABLED))
    return;

  // The mode is asked afresh at every step: a short buffer turns a load into a measure.
  size_t size = p.IsReadMode() ? 0 : rc_client_progress_size(m_client);
  p.Do(size);

  auto progress = std::make_unique<u8[]>(size);
  if (!p.IsReadMode())
  {
    const int status = rc_client_serialize_progress_sized(m_client, progress.get(), size);
    if (status != RC_OK)
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Failed serializing achievement client with error code {}",
                    status);
      return;
    }
  }

  p.DoArray(progress.get(), static_cast<u32>(size));

  if (p.IsReadMode())
  {
    const int status = rc_client_deserialize_progress_sized(m_client, progress.get(), size);
    if (status != RC_OK)
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Failed deserializing achievement client with error code {}",
                    status);
      return;
    }
    const size_t restored = rc_client_progress_size(m_client);
    if (restored != size)
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Loaded client size {} does not match size in state {}",
                    restored, size);
      return;
    }
  }
  p.DoMarker("AchievementManager");
}

// ---------------------------------------------------------------------------------------------
// The disc reader the hasher uses. It takes over the volume prepared by LoadGame/CalculateHash.

void* AchievementManager::FilereaderOpen(const char*)
{
  auto handle = std::make_unique<FilereaderState>();
  {
    AchievementManager& manager = GetInstance();
    std::lock_guard guard{manager.GetLock()};
    handle->volume = std::move(manager.GetLoadingVolume());
  }
  return handle->volume ? handle.release() : nullptr;
}

void AchievementManager::FilereaderSeek(void* file_handle, int64_t offset, int origin)
{
  auto* handle = static_cast<FilereaderState*>(file_handle);
  if (origin == SEEK_SET)
    handle->position = offset;
  else if (origin == SEEK_CUR)
    handle->position += offset;
  // SEEK_END is never requested.
}

int64_t AchievementManager::FilereaderTell(void* file_handle)
{
  return static_cast<const FilereaderState*>(file_handle)->position;
}

size_t AchievementManager::FilereaderRead(void* file_handle, void* buffer, size_t requested_bytes)
{
  auto* handle = static_cast<FilereaderState*>(file_handle);
  if (!handle->volume->Read(static_cast<u64>(handle->position), requested_bytes,
                            static_cast<u8*>(buffer), DiscIO::PARTITION_NONE))
  {
    return 0;
  }
  handle->position += requested_bytes;
  return requested_bytes;
}

void AchievementManager::FilereaderClose(void* file_handle)
{
  delete static_cast<FilereaderState*>(file_handle);
}

// ---------------------------------------------------------------------------------------------
// Badges

void AchievementManager::LoadDefaultBadges()
{
  std::lock_guard guard{m_lock};
  const std::string folder = File::GetSysDirectory() + DIR_SEP + RESOURCES_DIR + DIR_SEP;

  const auto load = [&folder](Badge* badge, std::string_view file, std::string_view what) {
    if (!badge->data.empty())
      return;
    if (!LoadPNGTexture(badge, fmt::format("{}{}", folder, file)))
      ERROR_LOG_FMT(ACHIEVEMENTS, "Default {} badge '{}' failed to load", what, file);
  };
  load(&m_default_player_badge, DEFAULT_PLAYER_BADGE_FILENAME, "player");
  load(&m_default_game_badge, DEFAULT_GAME_BADGE_FILENAME, "game");
  load(&m_default_unlocked_badge, DEFAULT_UNLOCKED_BADGE_FILENAME, "unlocked achievement");
  load(&m_default_locked_badge, DEFAULT_LOCKED_BADGE_FILENAME, "locked achievement");
}

void AchievementManager::FetchPlayerBadge()
{
  FetchBadge(
      &m_player_badge, RC_IMAGE_TYPE_USER,
      [](const AchievementManager& manager) -> std::string {
        const rc_client_user_t* account = rc_client_get_user_info(manager.m_client);
        return account != nullptr ? account->display_name : "";
      },
      {.player_icon = true});
}

void AchievementManager::FetchGameBadges()
{
  FetchBadge(
      &m_game_badge, RC_IMAGE_TYPE_GAME,
      [](const AchievementManager& manager) -> std::string {
        const rc_client_game_t* game = rc_client_get_game_info(manager.m_client);
        return game != nullptr ? game->badge_name : "";
      },
      {.game_icon = true});

  if (!rc_client_has_achievements(m_client))
    return;

  rc_client_achievement_list_t* list = nullptr;
  {
    std::lock_guard guard{m_lock};
    list = rc_client_create_achievement_list(m_client,
                                             RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
                                             RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  }
  for (const rc_client_achievement_bucket_t& bucket : std::span(list->buckets, list->num_buckets))
  {
    for (const rc_client_achievement_t* achievement :
         std::span(bucket.achievements, bucket.num_achievements))
    {
      const AchievementId id = achievement->id;
      // Both badge variants carry the same name; the image type picks the look.
      const BadgeNameFunction name_of = [id](const AchievementManager& manager) -> std::string {
        if (rc_client_get_achievement_info(manager.m_client, id) == nullptr)
          return "";
        return rc_client_get_achievement_info(manager.m_client, id)->badge_name;
      };
      FetchBadge(&m_unlocked_badges[id], RC_IMAGE_TYPE_ACHIEVEMENT, name_of,
                 {.achievements = {id}});
      FetchBadge(&m_locked_badges[id], RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED, name_of,
                 {.achievements = {id}});
    }
  }
  rc_client_destroy_achievement_list(list);
}

void AchievementManager::FetchBadge(Badge* badge, u32 badge_type, BadgeNameFunction function,
                                    UpdatedItems callback_data)
{
  const bool is_game_badge = badge_type == RC_IMAGE_TYPE_GAME;
  if (m_client == nullptr || !HasAPIToken())
  {
    update_event.Trigger(callback_data);
    if (m_display_welcome_message && is_game_badge)
      DisplayWelcomeMessage();
    return;
  }

  m_image_queue.Push([this, badge, badge_type, is_game_badge, name_of = std::move(function),
                      changed = std::move(callback_data)] {
    // The welcome message waits for the game badge, whatever happens to it.
    Common::ScopeGuard welcome([&] {
      if (m_display_welcome_message && is_game_badge)
        DisplayWelcomeMessage();
    });

    std::string wanted;
    {
      std::lock_guard guard{m_lock};
      wanted = name_of(*this);
    }
    if (wanted.empty())
      return;

    const std::string cache_file = fmt::format(
        "{}/badge-{}-{}.png", File::GetUserPath(D_RETROACHIEVEMENTSCACHE_IDX), badge_type,
        Common::SHA1::DigestToString(Common::SHA1::CalculateDigest(wanted)));

    Badge image;
    if (!LoadPNGTexture(&image, cache_file))
    {
      // Not cached yet: download it and keep a copy.
      const rc_api_fetch_image_request_t what = {.image_name = wanted.c_str(),
                                                 .image_type = badge_type};
      rc_api_request_t request;
      Common::HttpRequest http;
      if (rc_api_init_fetch_image_request(&request, &what) != RC_OK)
      {
        ERROR_LOG_FMT(ACHIEVEMENTS, "Invalid request for image {}.", wanted);
        return;
      }
      const auto download =
          http.Get(request.url, RequestHeaders(), Common::HttpRequest::AllowedReturnCodes::All);
      if (!download || download->empty())
      {
        WARN_LOG_FMT(ACHIEVEMENTS,
                     "RetroAchievements connection failed on image request.\n URL: {}",
                     request.url);
        rc_api_destroy_request(&request);
        update_event.Trigger(changed);
        return;
      }
      rc_api_destroy_request(&request);
      INFO_LOG_FMT(ACHIEVEMENTS, "Successfully downloaded badge id {}.", wanted);

      if (!LoadPNGTexture(&image, *download))
      {
        ERROR_LOG_FMT(ACHIEVEMENTS, "Badge '{}' failed to load", wanted);
        return;
      }

      // Write to a temporary name first so a broken write never leaves a bad cache entry.
      const std::string partial = fmt::format("{}.tmp", cache_file);
      File::IOFile out(partial, "wb");
      const bool stored = out.IsOpen() && out.WriteBytes(download->data(), download->size()) &&
                          out.Close() && File::Rename(partial, cache_file);
      if (!stored)
      {
        File::Delete(partial);
        WARN_LOG_FMT(ACHIEVEMENTS, "Failed to store badge '{}' to cache", wanted);
      }
    }

    std::lock_guard guard{m_lock};
    // The badge may have changed meanwhile (another game, another player).
    if (name_of(*this).empty() || wanted != name_of(*this))
    {
      INFO_LOG_FMT(ACHIEVEMENTS, "Requested outdated badge id {}.", wanted);
      return;
    }
    *badge = std::move(image);
    update_event.Trigger(changed);
    if (badge_type == RC_IMAGE_TYPE_ACHIEVEMENT &&
        m_active_challenges.contains(*changed.achievements.begin()))
    {
      m_challenges_updated = true;
    }
  });
}

// ---------------------------------------------------------------------------------------------
// Leaderboards

void AchievementManager::FetchBoardInfo(AchievementId leaderboard_id)
{
  // Two requests (the top four and the four around the player); each owns its copy of the id.
  rc_client_begin_fetch_leaderboard_entries(m_client, leaderboard_id, 1, 4,
                                            LeaderboardEntriesCallback, new u32(leaderboard_id));
  rc_client_begin_fetch_leaderboard_entries_around_user(
      m_client, leaderboard_id, 4, LeaderboardEntriesCallback, new u32(leaderboard_id));
}

void AchievementManager::LeaderboardEntriesCallback(int result, const char*,
                                                    rc_client_leaderboard_entry_list_t* list,
                                                    rc_client_t*, void* userdata)
{
  const std::unique_ptr<u32> board_id(static_cast<u32*>(userdata));
  if (result != RC_OK)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to fetch leaderboard entries.");
    return;
  }

  AchievementManager& manager = GetInstance();
  LeaderboardStatus& board = manager.m_leaderboard_map[*board_id];
  for (u32 i = 0; i < list->num_entries; ++i)
  {
    std::lock_guard guard{manager.GetLock()};
    const rc_client_leaderboard_entry_t& row = list->entries[i];
    LeaderboardEntry& entry = board.entries[row.index];
    entry.username.assign(row.user);
    std::memcpy(entry.score.data(), row.display, FORMAT_SIZE);
    entry.rank = row.rank;
    if (static_cast<int32_t>(i) == list->user_index)
      board.player_index = row.rank;
  }
  manager.update_event.Trigger({.leaderboards = {*board_id}});
}

// ---------------------------------------------------------------------------------------------
// Client events

void AchievementManager::EventHandler(const rc_client_event_t* event, rc_client_t* client)
{
  switch (event->type)
  {
  case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
    return HandleAchievementTriggeredEvent(event);
  case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
    return HandleLeaderboardStartedEvent(event);
  case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
    return HandleLeaderboardFailedEvent(event);
  case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
    return HandleLeaderboardSubmittedEvent(event);
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
    return HandleLeaderboardTrackerUpdateEvent(event);
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
    return HandleLeaderboardTrackerShowEvent(event);
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
    return HandleLeaderboardTrackerHideEvent(event);
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
    return HandleAchievementChallengeIndicatorShowEvent(event);
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
    return HandleAchievementChallengeIndicatorHideEvent(event);
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
    return HandleAchievementProgressIndicatorShowEvent(event);
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
    // On-screen messages expire by themselves.
    return;
  case RC_CLIENT_EVENT_GAME_COMPLETED:
    return HandleGameCompletedEvent(event, client);
  case RC_CLIENT_EVENT_RESET:
    return HandleResetEvent(event);
  case RC_CLIENT_EVENT_SERVER_ERROR:
    return HandleServerErrorEvent(event);
  default:
    INFO_LOG_FMT(ACHIEVEMENTS, "Event triggered of unhandled type {}", event->type);
    return;
  }
}

void AchievementManager::HandleAchievementTriggeredEvent(const rc_client_event_t* client_event)
{
  AchievementManager& manager = GetInstance();
  const rc_client_achievement_t& unlocked = *client_event->achievement;

  LongMessage(fmt::format("Unlocked: {} ({})", unlocked.title, unlocked.points),
              ModeColor(manager.m_client), &manager.GetAchievementBadge(unlocked.id, false));
  manager.update_event.Trigger(UpdatedItems{.achievements = {unlocked.id}});

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  // With RAIntegration an unlock may stay local; say so without going into the reason.
  const char* note = nullptr;
  switch (rc_client_raintegration_get_achievement_state(manager.m_client, unlocked.id))
  {
  case RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_LOCAL:
    note = "Local achievement; not submitted to site.";
    break;
  case RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_MODIFIED:
    note = "Modified achievement; not submitted to site.";
    break;
  case RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_INSECURE:
    note = "Achievement not submitted to site.";
    break;
  default:
    break;
  }
  if (note != nullptr)
    LongMessage(note, OSD::Color::GREEN);
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
}

void AchievementManager::HandleLeaderboardStartedEvent(const rc_client_event_t* client_event)
{
  const rc_client_leaderboard_t& board = *client_event->leaderboard;
  if (Config::Get(Config::RA_LEADERBOARD_TRACKER_ENABLED))
  {
    LongMessage(fmt::format("Attempting leaderboard: {} - {}", board.title, board.description),
                OSD::Color::GREEN);
  }
  GetInstance().FetchBoardInfo(board.id);
}

void AchievementManager::HandleLeaderboardFailedEvent(const rc_client_event_t* client_event)
{
  const rc_client_leaderboard_t& board = *client_event->leaderboard;
  if (Config::Get(Config::RA_LEADERBOARD_TRACKER_ENABLED))
    LongMessage(fmt::format("Failed leaderboard: {}", board.title), OSD::Color::RED);
  GetInstance().FetchBoardInfo(board.id);
}

void AchievementManager::HandleLeaderboardSubmittedEvent(const rc_client_event_t* client_event)
{
  AchievementManager& manager = GetInstance();
  const rc_client_leaderboard_t& board = *client_event->leaderboard;
  if (Config::Get(Config::RA_LEADERBOARD_TRACKER_ENABLED))
  {
    LongMessage(fmt::format("Scored {} on leaderboard: {}", board.tracker_value, board.title),
                OSD::Color::YELLOW);
  }
  manager.FetchBoardInfo(board.id);
  manager.update_event.Trigger(UpdatedItems{.leaderboards = {board.id}});
}

void AchievementManager::HandleLeaderboardTrackerUpdateEvent(const rc_client_event_t* client_event)
{
  const rc_client_leaderboard_tracker_t& update = *client_event->leaderboard_tracker;
  for (rc_client_leaderboard_tracker_t& shown : GetInstance().m_active_leaderboards)
  {
    if (shown.id == update.id)
      std::strncpy(shown.display, update.display, RC_CLIENT_LEADERBOARD_DISPLAY_SIZE);
  }
}

void AchievementManager::HandleLeaderboardTrackerShowEvent(const rc_client_event_t* client_event)
{
  GetInstance().m_active_leaderboards.push_back(*client_event->leaderboard_tracker);
}

void AchievementManager::HandleLeaderboardTrackerHideEvent(const rc_client_event_t* client_event)
{
  const u32 hidden = client_event->leaderboard_tracker->id;
  const auto is_hidden = [hidden](const rc_client_leaderboard_tracker_t& shown) {
    return shown.id == hidden;
  };
  std::erase_if(GetInstance().m_active_leaderboards, is_hidden);
}

void AchievementManager::HandleAchievementChallengeIndicatorShowEvent(
    const rc_client_event_t* client_event)
{
  AchievementManager& manager = GetInstance();
  if (manager.m_active_challenges.insert(client_event->achievement->id).second)
    manager.m_challenges_updated = true;
  manager.update_event.Trigger(UpdatedItems{.rich_presence = true});
}

void AchievementManager::HandleAchievementChallengeIndicatorHideEvent(
    const rc_client_event_t* client_event)
{
  AchievementManager& manager = GetInstance();
  if (manager.m_active_challenges.erase(client_event->achievement->id) != 0)
    manager.m_challenges_updated = true;
  manager.update_event.Trigger(UpdatedItems{.rich_presence = true});
}

void AchievementManager::HandleAchievementProgressIndicatorShowEvent(
    const rc_client_event_t* client_event)
{
  AchievementManager& manager = GetInstance();
  const rc_client_achievement_t& achievement = *client_event->achievement;
  manager.update_event.Trigger(UpdatedItems{.achievements = {achievement.id}});
  if (!Config::Get(Config::RA_PROGRESS_ENABLED))
    return;

  // Progress messages are short; do not stack them.
  const auto now = Clock::now();
  if (now - manager.m_last_progress_message < std::chrono::milliseconds{OSD::Duration::SHORT})
    return;
  OSD::AddMessage(fmt::format("{} {}", achievement.title, achievement.measured_progress),
                  OSD::Duration::SHORT, OSD::Color::GREEN,
                  &manager.GetAchievementBadge(achievement.id, false));
  manager.m_last_progress_message = now;
}

void AchievementManager::HandleGameCompletedEvent(const rc_client_event_t*, rc_client_t* client)
{
  const rc_client_user_t* player = rc_client_get_user_info(client);
  const rc_client_game_t* game = rc_client_get_game_info(client);
  if (player == nullptr || game == nullptr)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Received Game Completed event when game not running.");
    return;
  }
  const bool hardcore = rc_client_get_hardcore_enabled(client);
  LongMessage(fmt::format("Congratulations, {}! You have {} {}", player->display_name,
                          hardcore ? "mastered" : "completed", game->title),
              hardcore ? OSD::Color::YELLOW : OSD::Color::CYAN, &GetInstance().GetGameBadge());
}

void AchievementManager::HandleResetEvent(const rc_client_event_t*)
{
  INFO_LOG_FMT(ACHIEVEMENTS, "Reset requested by Achievement Manager");
  Core::Stop(Core::System::GetInstance());
}

void AchievementManager::HandleServerErrorEvent(const rc_client_event_t* client_event)
{
  ERROR_LOG_FMT(ACHIEVEMENTS, "RetroAchievements server error: {} {}",
                client_event->server_error->api, client_event->server_error->error_message);
}

// ---------------------------------------------------------------------------------------------
// What the client needs from the emulator: HTTP and memory.

void AchievementManager::Request(const rc_api_request_t* request,
                                 rc_client_server_callback_t callback, void* callback_data,
                                 rc_client_t*)
{
  // The request only lives for this call, so copy what the worker needs.
  GetInstance().m_queue.Push([url = std::string(request->url),
                              body = std::string(request->post_data), callback, callback_data] {
    Common::HttpRequest http;
    const Common::HttpRequest::Response reply =
        body.empty() ?
            http.Get(url, RequestHeaders(), Common::HttpRequest::AllowedReturnCodes::All) :
            http.Post(url, body, RequestHeaders(), Common::HttpRequest::AllowedReturnCodes::All);

    rc_api_server_response_t response;
    if (reply && !reply->empty())
    {
      response.body = reinterpret_cast<const char*>(reply->data());
      response.body_length = reply->size();
      response.http_status_code = http.GetLastResponseCode();
    }
    else
    {
      static constexpr char failure_text[] = "Failed HTTP request.";
      response.body = failure_text;
      response.body_length = sizeof(failure_text);
      response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
    }
    callback(&response, callback_data);
  });
}

u32 AchievementManager::MemoryPeeker(u32 address, u8* buffer, u32 num_bytes, rc_client_t*)
{
  if (buffer == nullptr)
    return 0;

  auto& system = Core::System::GetInstance();
  Core::CPUThreadGuard guard(system);
  // Stops at the first unreadable byte and reports how far it got.
  for (u32 done = 0; done < num_bytes; ++done)
  {
    const auto byte = system.GetMMU().HostTryRead<u8>(guard, address + done,
                                                      PowerPC::RequestedAddressSpace::Physical);
    if (!byte)
      return done;
    buffer[done] = byte->value;
  }
  return num_bytes;
}

// ---------------------------------------------------------------------------------------------
// RAIntegration (Windows only)

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
void AchievementManager::LoadIntegrationCallback(int result, const char* error_message,
                                                 rc_client_t*, void*)
{
  AchievementManager& manager = GetInstance();
  if (result == RC_OK)
  {
    INFO_LOG_FMT(ACHIEVEMENTS, "RAIntegration.dll found.");
    manager.m_dll_found = true;
    rc_client_set_allow_background_memory_reads(manager.m_client, 0);
    rc_client_raintegration_set_event_handler(manager.m_client, RAIntegrationEventHandler);
    rc_client_raintegration_set_write_memory_function(manager.m_client, MemoryPoker);
    rc_client_raintegration_set_get_game_name_function(manager.m_client,
                                                       GameTitleEstimateHandler);
    manager.dev_menu_update_event.Trigger();
  }
  else if (result == RC_MISSING_VALUE)
  {
    // No DLL; carry on without it.
    INFO_LOG_FMT(ACHIEVEMENTS, "RAIntegration.dll not found.");
  }
  else
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to load RAIntegration.dll. {}", error_message);
  }

  if (manager.HasAPIToken())
    manager.Login("");
  INFO_LOG_FMT(ACHIEVEMENTS, "Achievement Manager Initialized");
}

void AchievementManager::RAIntegrationEventHandler(const rc_client_raintegration_event_t* event,
                                                   rc_client_t*)
{
  switch (event->type)
  {
  case RC_CLIENT_RAINTEGRATION_EVENT_MENU_CHANGED:
  case RC_CLIENT_RAINTEGRATION_EVENT_MENUITEM_CHECKED_CHANGED:
    GetInstance().dev_menu_update_event.Trigger();
    break;
  case RC_CLIENT_RAINTEGRATION_EVENT_PAUSE:
    Core::QueueHostJob([](Core::System& system) { Core::SetState(system, Core::State::Paused); });
    break;
  case RC_CLIENT_RAINTEGRATION_EVENT_HARDCORE_CHANGED:
    Config::SetBaseOrCurrent(Config::RA_HARDCORE_ENABLED,
                             !Config::Get(Config::RA_HARDCORE_ENABLED));
    break;
  default:
    WARN_LOG_FMT(ACHIEVEMENTS, "Unsupported raintegration event. {}", event->type);
    break;
  }
}

void AchievementManager::MemoryPoker(u32 address, u8* buffer, u32 num_bytes, rc_client_t*)
{
  if (buffer == nullptr)
    return;
  Core::System* const system = GetInstance().m_system.load(std::memory_order_acquire);
  if (system == nullptr)
    return;
  Core::CPUThreadGuard guard(*system);
  system->GetMemory().CopyToEmu(address, buffer, num_bytes);
}

void AchievementManager::GameTitleEstimateHandler(char* buffer, u32 buffer_size, rc_client_t*)
{
  AchievementManager& manager = GetInstance();
  std::lock_guard guard{manager.m_lock};
  std::strncpy(buffer, manager.m_title_estimate.c_str(), static_cast<size_t>(buffer_size));
}
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

#endif  // USE_RETRO_ACHIEVEMENTS
