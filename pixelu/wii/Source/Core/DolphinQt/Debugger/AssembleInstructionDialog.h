// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "Common/CommonTypes.h"

class QDialogButtonBox;
class QLabel;
class QLineEdit;

// Asks for one line of Gekko assembly and turns it into a 32-bit instruction word.
class AssembleInstructionDialog : public QDialog
{
  Q_OBJECT
public:
  explicit AssembleInstructionDialog(QWidget* parent, u32 address, u32 value);

  u32 GetCode() const;

private:
  void BuildLayout();
  void Reassemble();

  u32 m_result_word;
  u32 m_target_address;

  QLineEdit* m_source_line = nullptr;
  QLabel* m_status_label = nullptr;
  QLabel* m_marked_line_label = nullptr;
  QLabel* m_detail_label = nullptr;
  QDialogButtonBox* m_dialog_buttons = nullptr;
};
