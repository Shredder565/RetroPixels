// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <optional>
#include <string>

#include <QThread>

#include "UICommon/AutoUpdate.h"

// docs/autoupdate_overview.md describes the whole auto-update flow.

class QWidget;

// Checks for a new build on the configured update track. run() does the automatic check on this
// thread; CheckForUpdate() does a manual one on the caller's. When an update turns up, the user
// is asked on the parent's thread whether to install it. The object deletes itself once the
// thread finishes.
class Updater : public QThread, public AutoUpdateChecker
{
  Q_OBJECT
public:
  explicit Updater(QWidget* parent, std::string update_track, std::string hash_override);

  void run() override;
  void OnUpdateAvailable(const NewVersionInformation& info) override;
  void CheckForUpdate();

private:
  // Runs the update dialog on the parent's thread. Returns its exec() result (nothing if the
  // parent went away) and whether "update after closing" was ticked.
  std::optional<int> AskUser(const NewVersionInformation& info, bool* install_on_exit);
  void CloseParent();

  QWidget* m_parent;
  std::string m_update_track;
  std::string m_hash_override;
};
