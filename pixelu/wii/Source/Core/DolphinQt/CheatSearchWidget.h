// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Common/CommonTypes.h"
#include "Core/ActionReplay.h"
#include "Core/CheatSearch.h"

namespace Core
{
class System;
}

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

// What the user typed about an address in the results table.
struct CheatSearchTableUserData
{
  std::string m_description;
};

// One "Cheat Search" tab: narrows a search session down step by step and shows the addresses
// that are left, with their last and current values.
class CheatSearchWidget : public QWidget
{
  Q_OBJECT
public:
  explicit CheatSearchWidget(Core::System& system,
                             std::unique_ptr<Cheats::CheatSearchSessionBase> session,
                             QWidget* parent = nullptr);
  ~CheatSearchWidget() override;

  enum class UpdateSource
  {
    User,
    Auto,
  };

  bool UpdateTableAllCurrentValues(UpdateSource source);
  void UpdateTableVisibleCurrentValues(UpdateSource source);

signals:
  void ActionReplayCodeGenerated(const ActionReplay::ARCode& ar_code);
  void RequestWatch(QString name, u32 address);
  void ShowMemory(u32 address);

private:
  QString DescribeSession() const;
  void BuildLayout();
  void ConnectSignals();

  void RunScan();
  void ReportScanCounts(bool had_results, size_t before, size_t after, size_t accessible);
  void ResetResults();
  void OnItemEdited(QTableWidgetItem* item);
  void ShowResultsMenu();
  void AddBreakpoints(std::string_view kind);
  void OnFilterTypeChanged();
  void OnHexDisplayToggled();
  void GenerateCodes();
  void WriteToSelection();

  bool RefreshRows(const Core::CPUThreadGuard& guard, size_t begin, size_t end,
                   UpdateSource source);
  void ShowCurrentValue(QTableWidgetItem* item);
  void ShowCurrentValues(size_t begin, size_t end);
  void RebuildTable();
  int FirstVisibleRow() const;
  int EndOfVisibleRows() const;
  size_t ShownRowCount() const;
  std::vector<const QTableWidgetItem*> SelectedRowItems() const;

  Core::System& m_system;

  // The search itself. Its values are the "Last Value" column and what "last value" compares
  // against; they only change on a scan or a reset.
  std::unique_ptr<Cheats::CheatSearchSessionBase> m_session;

  // The "Current Value" column, by address.
  std::unordered_map<u32, std::string> m_current_values;

  // Descriptions by address. Kept across scans and resets on purpose.
  std::unordered_map<u32, CheatSearchTableUserData> m_user_data;

  QComboBox* m_compare_type = nullptr;
  QComboBox* m_filter_type = nullptr;
  QLineEdit* m_value = nullptr;
  QLabel* m_status = nullptr;
  QLabel* m_counts = nullptr;
  QPushButton* m_scan_button = nullptr;
  QPushButton* m_refresh_button = nullptr;
  QPushButton* m_reset_button = nullptr;
  QCheckBox* m_parse_hex = nullptr;
  QCheckBox* m_show_hex = nullptr;
  QCheckBox* m_auto_refresh = nullptr;
  QTableWidget* m_table = nullptr;
};
