// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QDialog>

#include "Common/CommonTypes.h"
#include "Core/CheatSearch.h"
#include "DolphinQt/GameList/GameListModel.h"
#include "VideoCommon/VideoEvents.h"

class ARCodeWidget;
class GeckoCodeWidget;
class CheatSearchFactoryWidget;
class QDialogButtonBox;
class QHideEvent;
class QShowEvent;
class PartiallyClosableTabWidget;

namespace Core
{
enum class State;
class System;
}  // namespace Core

// Tools > Cheats Manager: the AR and Gecko code lists of the running game, plus one tab per
// memory search the user starts.
class CheatsManager : public QDialog
{
  Q_OBJECT
public:
  explicit CheatsManager(Core::System& system, QWidget* parent = nullptr);
  ~CheatsManager() override;

signals:
  void OpenGeneralSettings();
#ifdef USE_RETRO_ACHIEVEMENTS
  void OpenAchievementSettings();
#endif  // USE_RETRO_ACHIEVEMENTS
  void ShowMemory(u32 address);
  void RequestWatch(QString name, u32 address);

protected:
  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;

private:
  void BuildTabs();
  void Connect();
  void OnEmulationState(Core::State state);
  void OnFieldEnd();
  void AddSearchTab(const Cheats::CheatSearchSessionBase& session);
  void CloseTab(int index);
  void FollowGame(Core::State state);
  void RefreshAllSearches();

  // The game whose codes the AR and Gecko tabs show (empty while nothing runs).
  std::string m_game_id;
  std::string m_game_tdb_id;
  u16 m_revision = 0;

  Core::System& m_system;

  QDialogButtonBox* m_buttons = nullptr;
  PartiallyClosableTabWidget* m_tabs = nullptr;
  ARCodeWidget* m_ar_codes = nullptr;
  GeckoCodeWidget* m_gecko_codes = nullptr;
  CheatSearchFactoryWidget* m_new_search = nullptr;

  // Set while the window is shown, to refresh the visible search results once per field.
  Common::EventHook m_field_end_hook;
};
