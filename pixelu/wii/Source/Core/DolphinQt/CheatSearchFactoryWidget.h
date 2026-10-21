// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QWidget>

#include "Core/CheatSearch.h"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QRadioButton;

// The "Start New Cheat Search" tab: picks the memory ranges and the data type, then hands a
// fresh search session to the cheats manager.
class CheatSearchFactoryWidget : public QWidget
{
  Q_OBJECT
public:
  explicit CheatSearchFactoryWidget();
  ~CheatSearchFactoryWidget() override;

signals:
  void NewSessionCreated(const Cheats::CheatSearchSessionBase& session);

private:
  QWidget* BuildAddressSpaceGroup();
  QWidget* BuildDataTypeGroup();
  void UpdateCustomRangeControls();
  void CreateSession();

  QRadioButton* m_standard_space = nullptr;
  QRadioButton* m_custom_space = nullptr;

  QRadioButton* m_virtual_addresses = nullptr;
  QRadioButton* m_physical_addresses = nullptr;
  QRadioButton* m_effective_addresses = nullptr;

  QLineEdit* m_range_start = nullptr;
  QLineEdit* m_range_end = nullptr;

  QComboBox* m_data_type = nullptr;
  QCheckBox* m_aligned = nullptr;

  QPushButton* m_new_search = nullptr;
};
