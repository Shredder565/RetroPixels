// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>
#include <string_view>

// Asks the update server whether a newer build exists on an update track, and hands a found
// update to the separate updater program. A front end derives from this to present the result.
// (Only builds configured with AUTOUPDATE on Windows can update themselves.)
class AutoUpdateChecker
{
public:
  enum class CheckType
  {
    Automatic,
    Manual,
  };

  // Queries the server; calls OnUpdateAvailable when an update exists. A manual check also
  // reports "no update" and connection problems to the user. Only one check runs at a time.
  void CheckForUpdate(std::string_view update_track, std::string_view hash_override,
                      CheckType check_type);

  static bool SystemSupportsAutoUpdates();

  struct NewVersionInformation
  {
    // The new build's display name (such as 5.0-1234) and its commit hash.
    std::string new_shortrev;
    std::string new_hash;

    // Every change since the running build, as HTML.
    std::string changelog_html;

    // Handed through to the updater.
    std::string this_manifest_url;
    std::string next_manifest_url;
    std::string content_store_url;
  };

  enum class RestartMode
  {
    NO_RESTART_AFTER_UPDATE = 0,
    RESTART_AFTER_UPDATE,
  };

  // Launches the updater, which waits for this process to exit before replacing files.
  void TriggerUpdate(const NewVersionInformation& info, RestartMode restart_mode);

protected:
  virtual void OnUpdateAvailable(const NewVersionInformation& info) = 0;
};
