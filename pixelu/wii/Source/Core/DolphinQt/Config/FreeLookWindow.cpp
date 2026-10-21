// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/FreeLookWindow.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>

#include "DolphinQt/Config/FreeLookWidget.h"
#include "DolphinQt/Config/HardcoreWarningWidget.h"

FreeLookWindow::FreeLookWindow(QWidget* parent) : QDialog(parent)
{
  auto* const column = new QVBoxLayout();

#ifdef USE_RETRO_ACHIEVEMENTS
  m_hc_warning = new HardcoreWarningWidget(this);
#endif  // USE_RETRO_ACHIEVEMENTS
  m_button_box = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(m_button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);

#ifdef USE_RETRO_ACHIEVEMENTS
  column->addWidget(m_hc_warning);
#endif  // USE_RETRO_ACHIEVEMENTS
  column->addWidget(new FreeLookWidget(this));
  column->addWidget(m_button_box);
  setLayout(column);

#ifdef USE_RETRO_ACHIEVEMENTS
  connect(m_hc_warning, &HardcoreWarningWidget::OpenAchievementSettings, this,
          &FreeLookWindow::OpenAchievementSettings);
#endif  // USE_RETRO_ACHIEVEMENTS

  setWindowTitle(tr("Free Look Settings"));
}
