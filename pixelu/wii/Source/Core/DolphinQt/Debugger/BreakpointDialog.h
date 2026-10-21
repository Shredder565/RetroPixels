// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/BreakPoints.h"
#include "Core/PowerPC/PowerPC.h"

class BreakpointWidget;
class QCheckBox;
class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QRadioButton;
class QPushButton;
class QWidget;

// Creates an instruction or memory breakpoint, or edits an existing one, and hands the result to
// the breakpoint widget.
class BreakpointDialog : public QDialog
{
  Q_OBJECT
public:
  enum class OpenMode
  {
    New,
    EditBreakPoint,
    EditMemCheck
  };

  explicit BreakpointDialog(BreakpointWidget* parent);
  BreakpointDialog(BreakpointWidget* parent, const TBreakPoint* breakpoint);
  BreakpointDialog(BreakpointWidget* parent, const TMemCheck* memcheck);

  void accept() override;

private:
  void BuildLayout();
  void WireUp();
  void SyncKindBoxes();
  void SyncRangeFields();
  void SetActionChoice(bool break_on_hit, bool log_on_hit);
  void ExplainConditions();
  void ComplainAbout(const QString& field);

  // Kind of breakpoint
  QRadioButton* m_code_kind = nullptr;
  QRadioButton* m_memory_kind = nullptr;

  // Instruction breakpoint
  QWidget* m_code_section = nullptr;
  QGroupBox* m_code_box = nullptr;
  QLineEdit* m_code_address = nullptr;

  // Memory breakpoint
  QWidget* m_memory_section = nullptr;
  QGroupBox* m_memory_box = nullptr;
  QRadioButton* m_single_address = nullptr;
  QRadioButton* m_address_range = nullptr;
  QLabel* m_first_caption = nullptr;
  QLineEdit* m_first_address = nullptr;
  QLabel* m_last_caption = nullptr;
  QLineEdit* m_last_address = nullptr;
  QRadioButton* m_on_read = nullptr;
  QRadioButton* m_on_write = nullptr;
  QRadioButton* m_on_access = nullptr;

  // What a hit does
  QRadioButton* m_log_only = nullptr;
  QRadioButton* m_break_only = nullptr;
  QRadioButton* m_log_and_break = nullptr;
  QLineEdit* m_condition = nullptr;

  QDialogButtonBox* m_dialog_buttons = nullptr;
  BreakpointWidget* const m_owner;
  const OpenMode m_mode;
};
