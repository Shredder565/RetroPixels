// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

class ConfigBool;
class ConfigChoice;
class ConfigFloatSlider;
class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QRadioButton;
class QWidget;

// The colour correction options opened from Graphics > Enhancements:
// colour space, gamma and HDR paper white.
class ColorCorrectionConfigWindow final : public QDialog
{
  Q_OBJECT
public:
  explicit ColorCorrectionConfigWindow(QWidget* parent);

private:
  QGroupBox* BuildColorSpaceGroup();
  QGroupBox* BuildGammaGroup();
  QGroupBox* BuildHdrGroup();
  void LinkControls();
  void RefreshGammaTargets();

  ConfigBool* m_correct_color_space;
  ConfigChoice* m_game_color_space;
  ConfigFloatSlider* m_game_gamma;
  QLabel* m_game_gamma_value;
  ConfigBool* m_correct_gamma;
  QRadioButton* m_sdr_display_target_srgb;
  QRadioButton* m_sdr_display_target_custom;
  ConfigFloatSlider* m_sdr_display_custom_gamma;
  QLabel* m_sdr_display_custom_gamma_value;
  ConfigFloatSlider* m_hdr_paper_white_nits;
  QLabel* m_hdr_paper_white_nits_value;
  QDialogButtonBox* m_button_box;
};
