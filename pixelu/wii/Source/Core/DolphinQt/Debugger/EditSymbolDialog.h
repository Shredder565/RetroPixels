// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#pragma once
#include <string>

#include <QDialog>
#include <QString>
#include "Common/CommonTypes.h"

class QLineEdit;
class QDialogButtonBox;
class QSpinBox;

// Edits the name and length of a symbol or note in place (through the pointers it is given).
class EditSymbolDialog : public QDialog
{
  Q_OBJECT

public:
  enum class Type
  {
    Symbol,
    Note,
  };

  explicit EditSymbolDialog(QWidget* parent, const u32 symbol_address, u32* symbol_size,
                            std::string* symbol_name, Type type = Type::Symbol);

  bool DeleteRequested() const { return m_wants_delete; }

private:
  void BuildLayout();
  void WireUp();
  void LoadFromTarget();
  void ShowLength(u32 length);
  void Commit();

  QLineEdit* m_label_field = nullptr;
  QSpinBox* m_line_count = nullptr;
  QLineEdit* m_length_hex = nullptr;
  QLineEdit* m_end_hex = nullptr;
  QDialogButtonBox* m_dialog_buttons = nullptr;

  const Type m_kind;
  std::string* const m_target_name;
  u32* const m_target_size;
  const u32 m_start;

  bool m_wants_delete = false;
};
