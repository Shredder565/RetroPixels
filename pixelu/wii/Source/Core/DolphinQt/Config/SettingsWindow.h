// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

class QStackedWidget;
class QListWidget;
class MainWindow;
class QEvent;

// A dialog with a list of page names on the left; picking one shows that
// page on the right. A Close button sits under the pages.
class StackedSettingsWindow : public QDialog
{
  Q_OBJECT
public:
  explicit StackedSettingsWindow(QWidget* parent = nullptr);

  void ActivatePane(int index);

protected:
  void AddPane(QWidget*, const QString& name);

  // The same, with the page inside a scroll area.
  void AddWrappedPane(QWidget*, const QString& name);

  // Subclasses call this once every page is added.
  void OnDoneCreatingPanes();

  void changeEvent(QEvent* event) override;

private:
  void RecolourPageList();

  QStackedWidget* m_pages = nullptr;
  QListWidget* m_page_list = nullptr;
  bool m_in_theme_change = false;
};

enum class SettingsWindowPaneIndex : int
{
  General = 0,
  Graphics,
  Controllers,
  Interface,
  OnScreenDisplay,
  Audio,
  Paths,
  GameCube,
  Wii,
  Triforce,
  Advanced,
};

// The main Settings dialog. Its pages are in SettingsWindowPaneIndex order.
class SettingsWindow final : public StackedSettingsWindow
{
  Q_OBJECT
public:
  explicit SettingsWindow(MainWindow* parent);

  void SelectPane(SettingsWindowPaneIndex);

  void closeEvent(QCloseEvent* event) override;
};
