// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/Graphics/GraphicsPane.h"

#include <iterator>

#include <QDialogButtonBox>
#include <QEvent>
#include <QGroupBox>
#include <QLabel>
#include <QTabWidget>
#include <QVBoxLayout>

#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"

#include "DolphinQt/Config/Graphics/AdvancedWidget.h"
#include "DolphinQt/Config/Graphics/EnhancementsWidget.h"
#include "DolphinQt/Config/Graphics/GeneralWidget.h"
#include "DolphinQt/Config/Graphics/HacksWidget.h"
#include "DolphinQt/MainWindow.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"

#include "VideoCommon/VideoBackendBase.h"

GraphicsPane::GraphicsPane(MainWindow* main_window, Config::Layer* config_layer)
    : m_main_window(main_window), m_config_layer{config_layer}
{
  auto* const column = new QVBoxLayout{this};
  auto* const tabs = new QTabWidget;
  column->addWidget(tabs);

  // The tab widgets find this pane through their parent to reach the layer
  // and the backend signals.
  auto* const general = new GeneralWidget(this);
  QWidget* const pages[] = {general, new EnhancementsWidget(this), new HacksWidget(this),
                            new AdvancedWidget(this)};
  connect(general, &GeneralWidget::BackendChanged, this, &GraphicsPane::OnBackendChanged);

  QWidget* scrolled[std::size(pages)];
  for (size_t i = 0; i < std::size(pages); ++i)
    scrolled[i] = GetWrappedWidget(pages[i]);

  tabs->addTab(scrolled[0], tr("General"));
  tabs->addTab(scrolled[1], tr("Enhancements"));
  tabs->addTab(scrolled[2], tr("Hacks"));
  tabs->addTab(scrolled[3], tr("Advanced"));

  OnBackendChanged(QString::fromStdString(Config::Get(Config::MAIN_GFX_BACKEND)));
}

Config::Layer* GraphicsPane::GetConfigLayer()
{
  return m_config_layer;
}

void GraphicsPane::OnBackendChanged(const QString& backend_name)
{
  // A game's properties window has no main window; its backend info is
  // whatever the global settings last filled in.
  if (m_main_window != nullptr)
    VideoBackendBase::PopulateBackendInfo(m_main_window->GetWindowSystemInfo());

  emit BackendChanged(backend_name);
}
