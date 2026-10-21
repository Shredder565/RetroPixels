// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Config/SettingsWindow.h"

#include <utility>

#include <QApplication>
#include <QColor>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QListWidget>
#include <QPalette>
#include <QStackedWidget>
#include <QTabWidget>
#include <QVBoxLayout>

#include "DolphinQt/Config/ControllersPane.h"
#include "DolphinQt/Config/Graphics/GraphicsPane.h"
#include "DolphinQt/MainWindow.h"
#include "DolphinQt/QtUtils/QtUtils.h"
#include "DolphinQt/QtUtils/WrapInScrollArea.h"
#include "DolphinQt/Settings.h"
#include "DolphinQt/Settings/AdvancedPane.h"
#include "DolphinQt/Settings/AudioPane.h"
#include "DolphinQt/Settings/GameCubePane.h"
#include "DolphinQt/Settings/GeneralPane.h"
#include "DolphinQt/Settings/InterfacePane.h"
#include "DolphinQt/Settings/OnScreenDisplayPane.h"
#include "DolphinQt/Settings/PathPane.h"
#include "DolphinQt/Settings/TriforcePane.h"
#include "DolphinQt/Settings/WiiPane.h"

namespace
{
// Page list look: no frame, the alternate base colour behind it, vertical
// padding per row (horizontal padding is done with spaces in the names, as
// Qt ignores padding-left without a border), the highlight kept when the
// list loses focus, and no focus rectangle.
QString PageListStyleSheet(int row_padding)
{
  return QStringLiteral("QListWidget { border: 0; background: %1; } "
                        "QListWidget::item { padding-top: %2px; padding-bottom: %2px; } "
                        "QListWidget::item:selected { background: palette(highlight); "
                        "color: palette(highlighted-text); } "
                        "* { outline: none; } ")
      .arg(QStringLiteral("palette(alternate-base)"))
      .arg(row_padding);
}
}  // namespace

StackedSettingsWindow::StackedSettingsWindow(QWidget* parent) : QDialog{parent}
{
  // No frame line under the title bar on some Linux desktops.
  setStyleSheet(QStringLiteral("QDialog { border: none; }"));

  auto* const columns = new QHBoxLayout{this};
  // Row padding is half of the default layout margin, read before the
  // margins are removed.
  const int row_padding = columns->contentsMargins().left() / 2;
  columns->setContentsMargins(QMargins{});
  columns->setSpacing(0);

  m_page_list = new QListWidget;
  // As wide as the names need and no wider.
  m_page_list->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Minimum);
  m_page_list->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
  m_page_list->setStyleSheet(PageListStyleSheet(row_padding));
  RecolourPageList();
  columns->addWidget(m_page_list);

  auto* const right = new QVBoxLayout;
  columns->addLayout(right);

  m_pages = new QStackedWidget;
  right->addWidget(m_pages);

  // The frame pads the Close button away from the window edge.
  auto* const close_frame = new QFrame;
  auto* const close_grid = new QGridLayout{close_frame};
  auto* const buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  right->addWidget(close_frame);
  close_grid->addWidget(buttons);

  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_page_list, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
}

void StackedSettingsWindow::AddPane(QWidget* widget, const QString& name)
{
  m_pages->addWidget(widget);
  m_page_list->addItem(QStringLiteral("  %1  ").arg(name));
}

void StackedSettingsWindow::AddWrappedPane(QWidget* widget, const QString& name)
{
  AddPane(GetWrappedWidget(widget), name);
}

void StackedSettingsWindow::OnDoneCreatingPanes()
{
  ActivatePane(0);
  QtUtils::AdjustSizeWithinScreen(this);
}

void StackedSettingsWindow::ActivatePane(int index)
{
  m_page_list->setCurrentRow(index);
}

void StackedSettingsWindow::changeEvent(QEvent* event)
{
  QDialog::changeEvent(event);

  const QEvent::Type kind = event->type();
  const bool system_theme =
      kind == QEvent::ApplicationPaletteChange || kind == QEvent::ThemeChange;

  // A system theme switch restyles the whole application once (setPalette
  // below sends this window another change event).
  if (system_theme && !m_in_theme_change)
  {
    m_in_theme_change = true;
    Settings& settings = Settings::Instance();
    settings.ApplyStyle();
    setPalette(qApp->palette());
    settings.TriggerThemeChanged();
    m_in_theme_change = false;
  }

  if (system_theme || kind == QEvent::PaletteChange || kind == QEvent::StyleChange)
    RecolourPageList();
}

// Takes the list's colours from the application palette, using the active
// highlight for inactive windows too.
void StackedSettingsWindow::RecolourPageList()
{
  if (m_page_list == nullptr)
    return;

  const QPalette app = qApp->palette();
  const QColor highlight = app.color(QPalette::Active, QPalette::Highlight);
  const QColor highlight_text = app.color(QPalette::Active, QPalette::HighlightedText);

  QPalette colours = m_page_list->palette();
  for (const QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive})
  {
    for (const QPalette::ColorRole role : {QPalette::Base, QPalette::AlternateBase})
      colours.setColor(group, role, app.color(group, role));
    colours.setColor(group, QPalette::Highlight, highlight);
    colours.setColor(group, QPalette::HighlightedText, highlight_text);
  }
  m_page_list->setPalette(colours);
}

SettingsWindow::SettingsWindow(MainWindow* parent) : StackedSettingsWindow{parent}
{
  setWindowTitle(tr("Settings"));

  // Keep in step with SettingsWindowPaneIndex.
  AddWrappedPane(new GeneralPane(), tr("General"));
  AddPane(new GraphicsPane(parent, nullptr), tr("Graphics"));
  AddWrappedPane(new ControllersPane(), tr("Controllers"));
  AddWrappedPane(new InterfacePane(), tr("Interface"));
  AddWrappedPane(new OnScreenDisplayPane(), tr("On-Screen Display"));
  AddWrappedPane(new AudioPane(), tr("Audio"));
  AddWrappedPane(new PathPane(), tr("Paths"));
  AddWrappedPane(new GameCubePane(parent), tr("GameCube"));
  AddWrappedPane(new WiiPane(), tr("Wii"));
  AddWrappedPane(new TriforcePane(), tr("Triforce"));
  AddWrappedPane(new AdvancedPane(), tr("Advanced"));

  OnDoneCreatingPanes();
}

// Closing the window is when the settings are written to disk.
void SettingsWindow::closeEvent(QCloseEvent*)
{
  Config::Save();
}

void SettingsWindow::SelectPane(SettingsWindowPaneIndex pane)
{
  ActivatePane(static_cast<int>(pane));
}
