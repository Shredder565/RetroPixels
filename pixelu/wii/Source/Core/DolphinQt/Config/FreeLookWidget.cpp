// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/FreeLookWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "Core/AchievementManager.h"
#include "Core/Config/FreeLookSettings.h"

#include "DolphinQt/Config/ConfigControls/ConfigBool.h"
#include "DolphinQt/Config/ConfigControls/ConfigChoice.h"
#include "DolphinQt/Config/Mapping/MappingWindow.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/Settings.h"

namespace
{
#ifdef USE_RETRO_ACHIEVEMENTS
// Free Look counts as a cheat, so hardcore mode locks the switch.
void LockForHardcore(QWidget* toggle)
{
  toggle->setEnabled(!AchievementManager::GetInstance().IsHardcoreModeActive());
}
#endif  // USE_RETRO_ACHIEVEMENTS
}  // namespace

FreeLookWidget::FreeLookWidget(QWidget* parent) : QWidget(parent)
{
  m_enabled = new ConfigBool(tr("Enable"), Config::FREE_LOOK_ENABLED);
  m_enabled->SetDescription(
      tr("Allows manipulation of the in-game camera.<br><br><dolphin_emphasis>If unsure, "
         "leave this unchecked.</dolphin_emphasis>"));
#ifdef USE_RETRO_ACHIEVEMENTS
  LockForHardcore(m_enabled);
#endif  // USE_RETRO_ACHIEVEMENTS
  m_configure_button = new NonDefaultQPushButton(tr("Configure Controller"));

  m_control_type = new ConfigChoice({tr("Six Axis"), tr("First Person"), tr("Orbital")},
                                    Config::FL1_CONTROL_TYPE);
  m_control_type->SetTitle(tr("Free Look Control Type"));
  m_control_type->SetDescription(tr(
      "Changes the in-game camera type during Free Look.<br><br>"
      "Six Axis: Offers full camera control on all axes, akin to moving a spacecraft in zero "
      "gravity. This is the most powerful Free Look option but is the most challenging to use.<br> "
      "<br>"
      "First Person: Controls the free camera similarly to a first person video game. The camera "
      "can rotate and travel, but roll is impossible. Easy to use, but limiting.<br><br>"
      "Orbital: Rotates the free camera around the original camera. Has no lateral movement, only "
      "rotation and you may zoom up to the camera's origin point."));

  auto* const help = new QLabel(tr("Free Look allows for manipulation of the in-game camera. "
                                   "Different camera types are available from the dropdown.<br><br>"
                                   "For detailed instructions, "
                                   "<a href=\"https://wiki.dolphin-emu.org/index.php?title=Free_Look\">"
                                   "refer to this page</a>."));
  help->setTextFormat(Qt::RichText);
  help->setWordWrap(true);
  help->setTextInteractionFlags(Qt::TextBrowserInteraction);
  help->setOpenExternalLinks(true);

  m_background_input = new ConfigBool(tr("Background Input"), Config::FREE_LOOK_BACKGROUND_INPUT);

  auto* const camera_row = new QHBoxLayout();
  camera_row->addWidget(new QLabel(tr("Camera 1")));
  camera_row->addWidget(m_control_type);
  camera_row->addWidget(m_configure_button);

  auto* const column = new QVBoxLayout();
  column->addWidget(m_enabled);
  column->addLayout(camera_row);
  column->addWidget(m_background_input);
  column->addWidget(help);
  setLayout(column);

  RefreshEnabledState();

  connect(m_configure_button, &QPushButton::clicked, this,
          &FreeLookWidget::OpenControllerMapping);
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, [this] {
    const QSignalBlocker quiet(this);
    RefreshEnabledState();
  });
}

void FreeLookWidget::OpenControllerMapping()
{
  if (QObject::sender() != m_configure_button)
    return;
  // Free Look has a single mapping, port 0.
  auto* const window = new MappingWindow(this, MappingWindow::Type::MAPPING_FREELOOK, 0);
  window->setAttribute(Qt::WA_DeleteOnClose, true);
  window->setWindowModality(Qt::WindowModality::WindowModal);
  window->show();
}

// The camera options only apply while Free Look is on.
void FreeLookWidget::RefreshEnabledState()
{
  const bool on = Config::Get(Config::FREE_LOOK_ENABLED);
#ifdef USE_RETRO_ACHIEVEMENTS
  LockForHardcore(m_enabled);
#endif  // USE_RETRO_ACHIEVEMENTS
  for (QWidget* const option :
       {static_cast<QWidget*>(m_control_type), static_cast<QWidget*>(m_configure_button),
        static_cast<QWidget*>(m_background_input)})
  {
    option->setEnabled(on);
  }
}
