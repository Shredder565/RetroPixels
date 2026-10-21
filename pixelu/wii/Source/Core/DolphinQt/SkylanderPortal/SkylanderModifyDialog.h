// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "Common/CommonTypes.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"

class QVBoxLayout;
class QDialogButtonBox;

// Edits the saved values of the figure in one portal slot. The dialog shows itself when there is
// something to edit; otherwise it only explains why not.
class SkylanderModifyDialog : public QDialog
{
public:
  explicit SkylanderModifyDialog(QWidget* parent = nullptr, u8 slot = 0);

private:
  QString DisplayName() const;
  void PopulateSkylanderOptions(QVBoxLayout* layout);
  bool PopulateTrophyOptions(QVBoxLayout* layout);
  // Closes only once a save went through, then makes the game re-read the figure.
  void accept() override;

  bool m_saved = false;
  u8 m_portal_slot;
  IOS::HLE::USB::FigureData m_values;
  IOS::HLE::USB::SkylanderFigure* m_figure;
  QDialogButtonBox* m_buttons;
};
