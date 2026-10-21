// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDockWidget>

class QCheckBox;
class QCloseEvent;
class QListWidget;
class QPushButton;
class QRadioButton;
class QVBoxLayout;

// The Log Configuration dock: verbosity, which outputs are written to, and
// which log types are enabled.
class LogConfigWidget final : public QDockWidget
{
  Q_OBJECT
public:
  explicit LogConfigWidget(QWidget* parent = nullptr);
  ~LogConfigWidget() override;

protected:
  void closeEvent(QCloseEvent* event) override;

private:
  void Build();
  void Wire();
  void ReadState();
  void WriteState();
  void ToggleAllTypes();
  void HighlightVerbosity();

  QRadioButton* m_notice;
  QRadioButton* m_error;
  QRadioButton* m_warning;
  QRadioButton* m_info;
  QRadioButton* m_debug;

  QCheckBox* m_to_file;
  QCheckBox* m_to_console;
  QCheckBox* m_to_window;
  QPushButton* m_toggle_types;
  QListWidget* m_types;

  // What the toggle button last set; cleared if any type starts disabled.
  bool m_all_on = true;
  // Set while the toggle button changes every item, to save once at the end.
  bool m_suspend_writes = false;
};
