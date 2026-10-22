// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once

#include <string>

#include <QDialog>

class QComboBox;
class QRadioButton;

// Asks for the size and text encoding of a new memory card image, then for where to save it.
class GCMemcardCreateNewDialog : public QDialog
{
  Q_OBJECT
public:
  explicit GCMemcardCreateNewDialog(QWidget* parent = nullptr);
  ~GCMemcardCreateNewDialog() override;

  std::string GetMemoryCardPath() const;

private:
  bool SaveNewCard();

  QComboBox* m_size = nullptr;
  QRadioButton* m_western = nullptr;
  QRadioButton* m_shift_jis = nullptr;
  std::string m_path;
};
