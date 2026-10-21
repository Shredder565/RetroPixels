// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDockWidget>

#include <vector>

#include "Common/Assembler/GekkoAssembler.h"
#include "Core/Core.h"

class QTabWidget;
class AsmEditor;
class QAction;
class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTextEdit;
class QToolBar;

namespace Core
{
class System;
}  // namespace Core

// Dock with tabs of assembly source, an output pane that shows the assembled bytes as raw hex,
// Action Replay or Gecko codes, and an Inject action that patches the result into memory.
class AssemblerWidget : public QDockWidget
{
  Q_OBJECT
public:
  explicit AssemblerWidget(QWidget* parent);

  // Asks about unsaved tabs before the application quits. False cancels the quit.
  bool ApplicationCloseRequest();

  ~AssemblerWidget() override;

protected:
  void closeEvent(QCloseEvent*) override;

private:
  using Blocks = std::vector<Common::GekkoAssembler::CodeBlock>;

  void BuildLayout();
  void AddShortcuts();
  void FitOutputWidth();
  void RefreshIcons();

  AsmEditor* EditorAt(int index) const;
  AsmEditor* CurrentEditor() const;
  int IndexOf(const AsmEditor* editor) const;

  bool AssembleCurrent(Blocks* blocks);
  void InjectCurrent();
  void OpenFiles();
  void SaveCurrent();
  bool SaveEditor(AsmEditor* editor);
  void AddEditor(const QString& path = QString());
  void RequestCloseTab(int index);
  void RemoveTab(int index, AsmEditor* editor);

  QString TitleFor(const AsmEditor* editor, bool mark_unsaved) const;
  void RetitleTab(AsmEditor* editor);
  void ResolveDuplicateTitles(AsmEditor* added);

  int TakeUntitledNumber();
  void ReturnUntitledNumber(int number);
  void ZoomEverything(int steps);

  Core::System& m_system;

  QTabWidget* m_tabs = nullptr;
  QPlainTextEdit* m_output = nullptr;
  QComboBox* m_format_choice = nullptr;
  QPushButton* m_copy_button = nullptr;
  QTextEdit* m_messages = nullptr;
  QLineEdit* m_base_field = nullptr;
  QToolBar* m_bar = nullptr;
  QAction* m_open_action = nullptr;
  QAction* m_new_action = nullptr;
  QAction* m_assemble_action = nullptr;
  QAction* m_inject_action = nullptr;
  QAction* m_save_action = nullptr;

  std::vector<int> m_returned_numbers;
  int m_next_untitled = 0;
  int m_zoom_total = 0;
  bool m_dark = false;
};
