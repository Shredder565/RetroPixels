// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <QDialog>

#include "Common/Event.h"
#include "Common/Flag.h"
#include "UICommon/NetPlayIndex.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;
class QTableWidget;

// Lists the sessions published on the NetPlay index server and lets the user join one.
// Queries run on a worker thread: every filter change hands it a fresh filter set, and results
// come back to the GUI thread through queued signals.
class NetPlayBrowser : public QDialog
{
  Q_OBJECT
public:
  explicit NetPlayBrowser(QWidget* parent = nullptr);
  ~NetPlayBrowser() override;

  void accept() override;
signals:
  void Join();
  void UpdateStatusRequested(const QString& status);
  void UpdateListRequested(std::vector<NetPlaySession> sessions);

private:
  using FilterMap = std::map<std::string, std::string>;

  void CreateWidgets();
  void ConnectWidgets();
  QWidget* CreateFilterBox();

  FilterMap CurrentFilters() const;
  void Refresh();
  void RefreshLoop();
  void UpdateList();

  void OnSelectionChanged();

  void OnUpdateStatusRequested(const QString& status);
  void OnUpdateListRequested(std::vector<NetPlaySession> sessions);

  void SaveSettings() const;
  void RestoreSettings();

  QComboBox* m_region_combo = nullptr;
  QLabel* m_status_label = nullptr;
  QPushButton* m_button_refresh = nullptr;
  QTableWidget* m_table_widget = nullptr;
  QDialogButtonBox* m_button_box = nullptr;
  QLineEdit* m_edit_name = nullptr;
  QLineEdit* m_edit_game_id = nullptr;
  QCheckBox* m_check_hide_incompatible = nullptr;
  QCheckBox* m_check_hide_ingame = nullptr;

  QRadioButton* m_radio_all = nullptr;
  QRadioButton* m_radio_private = nullptr;
  QRadioButton* m_radio_public = nullptr;

  std::vector<NetPlaySession> m_sessions;

  std::thread m_refresh_thread;
  std::optional<FilterMap> m_refresh_filters;
  std::mutex m_refresh_filters_mutex;
  Common::Flag m_refresh_run;
  Common::Event m_refresh_event;
};

Q_DECLARE_METATYPE(std::vector<NetPlaySession>)
