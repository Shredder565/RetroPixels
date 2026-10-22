// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

#include "Common/CommonTypes.h"

class QDialogButtonBox;
class QLabel;
class QLineEdit;

// Takes either a raw hex word or one assembly instruction and previews the other form.
class PatchInstructionDialog : public QDialog
{
  Q_OBJECT
public:
  explicit PatchInstructionDialog(QWidget* parent, u32 address, u32 value);

  u32 GetCode() const;

private:
  void Reinterpret();

  u32 m_word;
  u32 m_where;

  QLineEdit* m_entry = nullptr;
  QLabel* m_preview = nullptr;
  QDialogButtonBox* m_dialog_buttons = nullptr;
};
