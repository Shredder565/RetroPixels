// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/CommonControllersWidget.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "Core/Config/MainSettings.h"

#include "DolphinQt/Config/ControllerInterface/ControllerInterfaceWindow.h"
#include "DolphinQt/Config/SDLHints/SDLHintsWindow.h"
#include "DolphinQt/QtUtils/NonDefaultQPushButton.h"
#include "DolphinQt/QtUtils/SignalBlocking.h"
#include "DolphinQt/Settings.h"

CommonControllersWidget::CommonControllersWidget(QWidget* parent) : QWidget(parent)
{
  // i18n: "Common" here means shared by every controller type.
  auto* const box = new QGroupBox(tr("Common"));
  auto* const box_column = new QVBoxLayout();
  m_background_input = new QCheckBox(tr("Background Input"));
  m_input_sources_button = new NonDefaultQPushButton(tr("Alternate Input Sources"));
  m_sdl_hints_button = new NonDefaultQPushButton(tr("SDL Controller Settings"));

  box_column->addWidget(m_background_input);
  box_column->addWidget(m_input_sources_button);
  box_column->addWidget(m_sdl_hints_button);
  box->setLayout(box_column);

  auto* const outer = new QVBoxLayout;
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setAlignment(Qt::AlignTop);
  outer->addWidget(box);
  setLayout(outer);

  Reload();

  connect(m_background_input, &QCheckBox::toggled, this,
          &CommonControllersWidget::StoreBackgroundInput);
  connect(m_input_sources_button, &QPushButton::clicked, this,
          &CommonControllersWidget::OpenModal<ControllerInterfaceWindow>);
  connect(m_sdl_hints_button, &QPushButton::clicked, this,
          &CommonControllersWidget::OpenModal<SDLHintsWindow>);
  connect(&Settings::Instance(), &Settings::ConfigChanged, this, &CommonControllersWidget::Reload);
}

// Both windows are window-modal and delete themselves when closed.
template <class Window>
void CommonControllersWidget::OpenModal()
{
  auto* const window = new Window(this);
  window->setAttribute(Qt::WA_DeleteOnClose, true);
  window->setWindowModality(Qt::WindowModality::WindowModal);
  window->show();
}

void CommonControllersWidget::Reload()
{
  const bool background = Config::Get(Config::MAIN_INPUT_BACKGROUND_INPUT);
  SignalBlocking(m_background_input)->setChecked(background);
}

void CommonControllersWidget::StoreBackgroundInput()
{
  Config::SetBaseOrCurrent(Config::MAIN_INPUT_BACKGROUND_INPUT, m_background_input->isChecked());
  Config::Save();
}
