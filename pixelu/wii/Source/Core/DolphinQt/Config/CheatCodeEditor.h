// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <QDialog>

class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QTextEdit;

namespace ActionReplay
{
struct ARCode;
}

namespace Gecko
{
class GeckoCode;
}

// Edits one Action Replay or Gecko code in place. The dialog has no
// Q_OBJECT of its own, so its texts translate in the QDialog context.
class CheatCodeEditor : public QDialog
{
public:
  explicit CheatCodeEditor(QWidget* parent);

  void SetARCode(ActionReplay::ARCode* code);
  void SetGeckoCode(Gecko::GeckoCode* code);

private:
  void SetGeckoFieldsVisible(bool visible);
  bool StoreARCode();
  bool StoreGeckoCode();

  void accept() override;

  QLabel* m_creator_caption;
  QLabel* m_notes_caption;

  QLineEdit* m_name;
  QLineEdit* m_creator;
  QTextEdit* m_notes;
  QTextEdit* m_lines;
  QDialogButtonBox* m_buttons;

  ActionReplay::ARCode* m_ar_target = nullptr;
  Gecko::GeckoCode* m_gecko_target = nullptr;
};
