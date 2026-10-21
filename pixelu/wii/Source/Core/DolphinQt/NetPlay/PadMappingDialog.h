// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "Core/NetPlayProto.h"

class QCheckBox;
class QGridLayout;
class QComboBox;
class QDialogButtonBox;

namespace NetPlay
{
class Player;
}

// Host-side grid that assigns a player to each GameCube port, GBA slot and Wii Remote.
// exec() loads the server's current assignment; the getters return what the user picked.
class PadMappingDialog : public QDialog
{
  Q_OBJECT
public:
  explicit PadMappingDialog(QWidget* widget);

  int exec() override;

  NetPlay::PadMappingArray GetGCPadArray();
  NetPlay::GBAConfigArray GetGBAArray();
  NetPlay::PadMappingArray GetWiimoteArray();

private:
  void CreateWidgets();
  void ConnectWidgets();
  void FillPlayerChoices(const QStringList& choices);
  NetPlay::PlayerId PlayerAt(int combo_index) const;

  void OnMappingChanged();

  NetPlay::PadMappingArray m_pad_mapping;
  NetPlay::GBAConfigArray m_gba_config;
  NetPlay::PadMappingArray m_wii_mapping;

  QGridLayout* m_main_layout = nullptr;
  std::array<QComboBox*, 4> m_gc_boxes{};
  std::array<QCheckBox*, 4> m_gba_boxes{};
  std::array<QComboBox*, 4> m_wii_boxes{};
  std::vector<const NetPlay::Player*> m_players;
  QDialogButtonBox* m_button_box = nullptr;
};
