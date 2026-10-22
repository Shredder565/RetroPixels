// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QByteArray>
#include <QDockWidget>
#include <QString>

#include "Common/CommonTypes.h"
#include "VideoCommon/VideoEvents.h"

class MemoryViewWidget;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QHideEvent;
class QLabel;
class QLayout;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QShowEvent;
class QSplitter;
class QWidget;

namespace Core
{
class System;
class CPUThreadGuard;
}  // namespace Core

class PPCSymbolDB;

// The "Memory" dock: a memory view plus a side bar to jump to addresses, type in and write
// values, search, change how memory is shown, configure memory breakpoints and browse labels.
class MemoryWidget : public QDockWidget
{
  Q_OBJECT
public:
  explicit MemoryWidget(Core::System& system, QWidget* parent = nullptr);
  ~MemoryWidget() override;

  void SetAddress(u32 address);
  void Update();
signals:
  void ShowCode(u32 address);
  void RequestWatch(QString name, u32 address);

private:
  // Address box plus offset box, resolved. address is only meaningful when both are valid.
  struct Target
  {
    u32 address = 0;
    bool address_ok = false;
    bool offset_ok = false;
  };

  void closeEvent(QCloseEvent*) override;
  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;

  // Side bar pieces, built in this order.
  QWidget* BuildAddressRow();
  QLayout* BuildValueRow();
  void BuildValueTypes();
  QWidget* BuildSearchBox();
  QWidget* BuildAddressSpaceBox();
  QWidget* BuildDisplayBox();
  QWidget* BuildBreakpointBox();
  void BuildLabelsBox();
  void BuildMenus(QWidget* sidebar);
  void Wire();

  void RestoreChoices();
  void StoreChoices();

  void AddressSpaceChosen();
  void DisplayChosen();
  void BreakpointLoggingChosen();
  void BreakpointKindChosen();

  void JumpToTyped();
  void FocusAddressBox();
  void PreviewValue();
  [[nodiscard]] QByteArray ValueBytes() const;
  [[nodiscard]] Target ResolveTarget() const;
  [[nodiscard]] bool ReportBadTarget(const Target& target);
  void WriteValue();
  void WriteFile();
  void Search(bool forward);

  void RefreshLabels();
  void JumpToLabel();
  void FillSymbolLists();
  void FillNoteList();

  void StartFrameUpdates();
  void StopFrameUpdates();

  Core::System& m_system;
  PPCSymbolDB& m_symbol_db;

  MemoryViewWidget* m_memory_view = nullptr;
  QSplitter* m_splitter = nullptr;

  QComboBox* m_address_box = nullptr;
  QLineEdit* m_offset_box = nullptr;
  QLineEdit* m_value_edit = nullptr;
  QCheckBox* m_hex_check = nullptr;
  QComboBox* m_value_type = nullptr;
  QLabel* m_value_preview = nullptr;
  QPushButton* m_write_button = nullptr;

  QPushButton* m_next_button = nullptr;
  QPushButton* m_previous_button = nullptr;
  QLabel* m_search_result = nullptr;

  QRadioButton* m_effective_radio = nullptr;
  QRadioButton* m_auxiliary_radio = nullptr;
  QRadioButton* m_physical_radio = nullptr;

  QComboBox* m_display_type = nullptr;
  QComboBox* m_alignment = nullptr;
  QComboBox* m_row_length = nullptr;
  QCheckBox* m_dual_check = nullptr;

  QRadioButton* m_bp_both_radio = nullptr;
  QRadioButton* m_bp_read_radio = nullptr;
  QRadioButton* m_bp_write_radio = nullptr;
  QCheckBox* m_bp_log_check = nullptr;

  QGroupBox* m_labels_box = nullptr;
  QLineEdit* m_label_filter = nullptr;
  QListWidget* m_symbol_list = nullptr;
  QListWidget* m_data_list = nullptr;
  QListWidget* m_note_list = nullptr;

  bool m_labels_visible = true;
  bool m_auto_update = true;
  Common::EventHook m_frame_hook;
};
