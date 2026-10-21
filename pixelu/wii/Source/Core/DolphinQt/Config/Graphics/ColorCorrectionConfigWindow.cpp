// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Graphics/ColorCorrectionConfigWindow.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QRadioButton>

#include "Core/Config/GraphicsSettings.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/ConfigControls/ConfigFloatSlider.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"

namespace
{
// Keeps a value label in step with its slider.
void ShowSliderValue(QLabel* label, const ConfigFloatSlider* slider, const char* format)
{
  label->setText(QString::asprintf(format, slider->GetValue()));
}
}  // namespace

ColorCorrectionConfigWindow::ColorCorrectionConfigWindow(QWidget* parent) : QDialog(parent)
{
  setWindowTitle(tr("Color Correction Configuration"));

  QGroupBox* const color_space = BuildColorSpaceGroup();
  QGroupBox* const gamma = BuildGammaGroup();
  QGroupBox* const hdr = BuildHdrGroup();
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Close);

  auto* const column = new QVBoxLayout(this);
  column->setAlignment(Qt::AlignTop);
  column->addWidget(color_space);
  column->addWidget(gamma);
  column->addWidget(hdr);
  column->addStretch();
  column->addWidget(m_button_box);

  WrapInScrollArea(this, column);
  adjustSize();

  LinkControls();
}

QGroupBox* ColorCorrectionConfigWindow::BuildColorSpaceGroup()
{
  static const char correct_help[] = QT_TR_NOOP(
      "Converts the colors from the color spaces that GC/Wii were meant to work with to "
      "sRGB/Rec.709.<br><br>"
      "There's no way of knowing what exact color space games were meant for, given there were "
      "multiple standards and most games didn't acknowledge them, so it's not correct to assume a "
      "format from the game disc region. Just pick the one that looks more natural to you, or "
      "match it with the region the game was developed in.<br><br>"
      "HDR output is required to show all the colors from the PAL and NTSC-J color "
      "spaces.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");

  auto* const box = new QGroupBox(tr("Color Space"));
  auto* const grid = new QGridLayout();
  grid->setColumnStretch(1, 1);
  box->setLayout(grid);

  m_correct_color_space =
      new ConfigBool(tr("Correct Color Space"), Config::GFX_CC_CORRECT_COLOR_SPACE);
  grid->addWidget(m_correct_color_space, 0, 0, 1, 2);
  m_correct_color_space->SetDescription(tr(correct_help));

  // In the order of the ColorCorrectionRegion values.
  const QStringList regions{tr("NTSC-M (SMPTE 170M)"), tr("NTSC-J (ARIB TR-B9)"), tr("PAL (EBU)")};
  m_game_color_space = new ConfigChoice(regions, Config::GFX_CC_GAME_COLOR_SPACE);
  grid->addWidget(new QLabel(tr("Game Color Space:")), 1, 0);
  grid->addWidget(m_game_color_space, 1, 1);
  m_game_color_space->setEnabled(m_correct_color_space->isChecked());

  return box;
}

QGroupBox* ColorCorrectionConfigWindow::BuildGammaGroup()
{
  static const char game_gamma_help[] = QT_TR_NOOP(
      "NTSC-M and NTSC-J target gamma ~2.2. PAL targets gamma ~2.8.<br>"
      "None of the two were necessarily followed by games or TVs.<br>"
      "2.35 is a good generic value for all regions.<br><br>"
      "If a game allows you to chose a gamma value, match it here.<br><br>"
      "<dolphin_emphasis>If unsure, leave this at 2.35.</dolphin_emphasis>");
  static const char correct_gamma_help[] = QT_TR_NOOP(
      "Converts the gamma from what the game targeted to what your current SDR display "
      "targets.<br>"
      "Monitors often target sRGB. TVs often target 2.2.<br><br>"
      "<dolphin_emphasis>If unsure, leave this unchecked.</dolphin_emphasis>");

  auto* const box = new QGroupBox(tr("Gamma"));
  auto* const grid = new QGridLayout();
  box->setLayout(grid);

  m_game_gamma = new ConfigFloatSlider(Config::GFX_CC_GAME_GAMMA_MIN, Config::GFX_CC_GAME_GAMMA_MAX,
                                       Config::GFX_CC_GAME_GAMMA, 0.01f);
  grid->addWidget(new QLabel(tr("Game Gamma:")), 0, 0);
  grid->addWidget(m_game_gamma, 0, 1);
  m_game_gamma->SetTitle(tr("Game Gamma"));
  m_game_gamma->SetDescription(tr(game_gamma_help));
  m_game_gamma_value = new QLabel();
  grid->addWidget(m_game_gamma_value, 0, 2);

  m_correct_gamma = new ConfigBool(tr("Correct SDR Gamma"), Config::GFX_CC_CORRECT_GAMMA);
  grid->addWidget(m_correct_gamma, 1, 0, 1, 2);
  m_correct_gamma->SetDescription(tr(correct_gamma_help));

  // The display's gamma: sRGB, or a custom power curve.
  auto* const target = new QGroupBox(tr("SDR Display Gamma Target"));
  auto* const target_grid = new QGridLayout();
  target->setLayout(target_grid);

  m_sdr_display_target_srgb = new QRadioButton(tr("sRGB"));
  m_sdr_display_target_custom = new QRadioButton(tr("Custom:"));
  m_sdr_display_custom_gamma =
      new ConfigFloatSlider(Config::GFX_CC_DISPLAY_GAMMA_MIN, Config::GFX_CC_DISPLAY_GAMMA_MAX,
                            Config::GFX_CC_SDR_DISPLAY_CUSTOM_GAMMA, 0.01f);
  m_sdr_display_custom_gamma_value = new QLabel();

  target_grid->addWidget(m_sdr_display_target_srgb, 0, 0);
  target_grid->addWidget(m_sdr_display_target_custom, 1, 0);
  target_grid->addWidget(m_sdr_display_custom_gamma, 1, 1);
  target_grid->addWidget(m_sdr_display_custom_gamma_value, 1, 2);
  grid->addWidget(target, 2, 0, 1, 3);

  const bool correcting = m_correct_gamma->isChecked();
  m_sdr_display_target_srgb->setEnabled(correcting);
  m_sdr_display_target_srgb->setChecked(Config::Get(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB));
  m_sdr_display_target_custom->setEnabled(correcting);
  m_sdr_display_target_custom->setChecked(!m_sdr_display_target_srgb->isChecked());
  RefreshGammaTargets();

  ShowSliderValue(m_game_gamma_value, m_game_gamma, "%.2f");
  ShowSliderValue(m_sdr_display_custom_gamma_value, m_sdr_display_custom_gamma, "%.2f");

  return box;
}

QGroupBox* ColorCorrectionConfigWindow::BuildHdrGroup()
{
  static const char paper_white_help[] = QT_TR_NOOP(
      "Controls the base luminance of a paper white surface in nits. Useful for adjusting to "
      "different environmental lighting conditions when using a HDR display.<br><br>"
      "HDR output is required for this setting to take effect.<br><br>"
      "<dolphin_emphasis>If unsure, leave this at 203.</dolphin_emphasis>");

  auto* const box = new QGroupBox(tr("HDR"));
  auto* const grid = new QGridLayout();
  box->setLayout(grid);

  m_hdr_paper_white_nits = new ConfigFloatSlider(Config::GFX_CC_HDR_PAPER_WHITE_NITS_MIN,
                                                 Config::GFX_CC_HDR_PAPER_WHITE_NITS_MAX,
                                                 Config::GFX_CC_HDR_PAPER_WHITE_NITS, 1.f);
  grid->addWidget(new QLabel(tr("HDR Paper White Nits:")), 0, 0);
  grid->addWidget(m_hdr_paper_white_nits, 0, 1);
  m_hdr_paper_white_nits->SetTitle(tr("HDR Paper White Nits"));
  m_hdr_paper_white_nits->SetDescription(tr(paper_white_help));
  m_hdr_paper_white_nits_value = new QLabel();
  grid->addWidget(m_hdr_paper_white_nits_value, 0, 2);
  ShowSliderValue(m_hdr_paper_white_nits_value, m_hdr_paper_white_nits, "%.0f");

  return box;
}

// The custom gamma slider only matters while correcting towards a custom
// target.
void ColorCorrectionConfigWindow::RefreshGammaTargets()
{
  m_sdr_display_custom_gamma->setEnabled(m_correct_gamma->isChecked() &&
                                         m_sdr_display_target_custom->isChecked());
}

void ColorCorrectionConfigWindow::LinkControls()
{
  connect(m_correct_color_space, &QCheckBox::toggled, this,
          [this] { m_game_color_space->setEnabled(m_correct_color_space->isChecked()); });

  connect(m_game_gamma, &ConfigFloatSlider::valueChanged, this,
          [this] { ShowSliderValue(m_game_gamma_value, m_game_gamma, "%.2f"); });

  // Game gamma stays editable: it also feeds the colour space conversion.
  // The SDR targets stay editable with HDR output too, since this window
  // cannot tell whether HDR is in use.
  connect(m_correct_gamma, &QCheckBox::toggled, this, [this] {
    const bool correcting = m_correct_gamma->isChecked();
    m_sdr_display_target_srgb->setEnabled(correcting);
    m_sdr_display_target_custom->setEnabled(correcting);
    RefreshGammaTargets();
  });

  connect(m_sdr_display_target_srgb, &QRadioButton::toggled, this, [this] {
    const bool srgb = m_sdr_display_target_srgb->isChecked();
    Config::SetBaseOrCurrent(Config::GFX_CC_SDR_DISPLAY_GAMMA_SRGB, srgb);
    m_sdr_display_custom_gamma->setEnabled(!srgb);
  });

  connect(m_sdr_display_custom_gamma, &ConfigFloatSlider::valueChanged, this, [this] {
    ShowSliderValue(m_sdr_display_custom_gamma_value, m_sdr_display_custom_gamma, "%.2f");
  });

  connect(m_hdr_paper_white_nits, &ConfigFloatSlider::valueChanged, this,
          [this] { ShowSliderValue(m_hdr_paper_white_nits_value, m_hdr_paper_white_nits, "%.0f"); });

  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
