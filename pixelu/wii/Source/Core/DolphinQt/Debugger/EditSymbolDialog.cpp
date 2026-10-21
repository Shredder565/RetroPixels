// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/EditSymbolDialog.h"

#include <QAbstractButton>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

namespace
{
// Lengths are limited to seven hex digits.
constexpr u32 LENGTH_MASK = 0x0FFFFFFF;

QString EightDigits(u32 value)
{
  return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

bool ParseHex(const QLineEdit* field, u32* out)
{
  bool ok = false;
  const u32 value = field->text().toUInt(&ok, 16);
  if (ok)
    *out = value;
  return ok;
}
}  // namespace

EditSymbolDialog::EditSymbolDialog(QWidget* parent, const u32 symbol_address, u32* symbol_size,
                                   std::string* symbol_name, Type type)
    : QDialog(parent), m_kind(type), m_target_name(symbol_name), m_target_size(symbol_size),
      m_start(symbol_address)
{
  setWindowTitle(m_kind == Type::Symbol ? tr("Edit Symbol") : tr("Edit Note"));
  BuildLayout();
  WireUp();
}

void EditSymbolDialog::BuildLayout()
{
  const bool is_symbol = m_kind == Type::Symbol;

  m_dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
  m_dialog_buttons->addButton(tr("Reset"), QDialogButtonBox::ResetRole);
  m_dialog_buttons->addButton(tr("Delete"), QDialogButtonBox::DestructiveRole);

  // i18n: %1 is an address. %2 is a warning message.
  const QString heading_format =
      is_symbol ? tr("Editing symbol starting at: %1\n%2") : tr("Editing note starting at: %1\n%2");
  auto* heading = new QLabel(heading_format.arg(QString::number(m_start, 16))
                                 .arg(tr("Warning: Must save the symbol map for changes to be kept.")));

  m_label_field = new QLineEdit();
  m_label_field->setPlaceholderText(is_symbol ? tr("Symbol name") : tr("Note name"));

  // i18n: The address where a symbol ends
  auto* end_caption = new QLabel(tr("End Address"));
  // i18n: The number of lines of code
  auto* lines_caption = new QLabel(tr("Lines"));
  // i18n: There's a text field immediately to the right of this label where 8 hexadecimal digits
  // can be entered. The "0x" in this string acts as a prefix to the digits to indicate that they
  // are hexadecimal.
  auto* hex_caption = new QLabel(tr("Size: 0x"));

  m_end_hex = new QLineEdit();
  m_line_count = new QSpinBox();
  m_length_hex = new QLineEdit();

  hex_caption->setAlignment(Qt::AlignCenter | Qt::AlignRight);
  lines_caption->setAlignment(Qt::AlignCenter | Qt::AlignRight);

  // Field widths are measured in pairs of '0' glyphs of the default font.
  const int pair = QFontMetrics(QFont()).horizontalAdvance(QLatin1Char('0')) * 2;
  m_end_hex->setFixedWidth(pair * 6);
  m_length_hex->setFixedWidth(pair * 5);
  m_line_count->setFixedWidth(pair * 5);
  m_length_hex->setMaxLength(7);
  m_line_count->setRange(0, 99999);

  auto* hex_only = new QRegularExpressionValidator(
      QRegularExpression(QStringLiteral("[0-9a-fA-F]{0,8}")), this);
  m_end_hex->setValidator(hex_only);
  m_length_hex->setValidator(hex_only);

  auto* length_row = new QHBoxLayout;
  length_row->addWidget(end_caption);
  length_row->addWidget(m_end_hex);
  length_row->addWidget(hex_caption);
  length_row->addWidget(m_length_hex);
  length_row->addWidget(lines_caption);
  length_row->addWidget(m_line_count);

  auto* column = new QVBoxLayout();
  column->addWidget(heading);
  column->addWidget(m_label_field);
  column->addLayout(length_row);
  column->addWidget(m_dialog_buttons);
  setLayout(column);

  LoadFromTarget();
}

void EditSymbolDialog::LoadFromTarget()
{
  const u32 length = *m_target_size;
  m_label_field->setText(QString::fromStdString(*m_target_name));
  // The spin box goes first: once connected, its change signal rewrites the other two fields,
  // which are then set again from the unrounded length.
  m_line_count->setValue(static_cast<int>(length / 4));
  m_length_hex->setText(QString::number(length, 16));
  m_end_hex->setText(EightDigits(m_start + length));
}

void EditSymbolDialog::ShowLength(u32 length)
{
  length &= LENGTH_MASK;
  m_line_count->setValue(static_cast<int>(length / 4));
  m_length_hex->setText(QString::number(length, 16));
  m_end_hex->setText(EightDigits(m_start + length));
}

void EditSymbolDialog::WireUp()
{
  connect(m_line_count, &QSpinBox::valueChanged, this,
          [this](int lines) { ShowLength(static_cast<u32>(lines * 4)); });

  connect(m_length_hex, &QLineEdit::editingFinished, this, [this] {
    u32 length;
    if (ParseHex(m_length_hex, &length))
      ShowLength(length);
  });

  connect(m_end_hex, &QLineEdit::textEdited, this, [this] {
    u32 end;
    if (ParseHex(m_end_hex, &end) && end > m_start)
      ShowLength(end - m_start);
  });

  connect(m_dialog_buttons, &QDialogButtonBox::accepted, this, &EditSymbolDialog::Commit);
  connect(m_dialog_buttons, &QDialogButtonBox::rejected, this, &EditSymbolDialog::reject);
  connect(m_dialog_buttons, &QDialogButtonBox::clicked, this, [this](QAbstractButton* pressed) {
    switch (m_dialog_buttons->buttonRole(pressed))
    {
    case QDialogButtonBox::ResetRole:
      LoadFromTarget();
      break;
    case QDialogButtonBox::DestructiveRole:
      m_wants_delete = true;
      QDialog::accept();
      break;
    default:
      break;
    }
  });
}

void EditSymbolDialog::Commit()
{
  const std::string typed_name = m_label_field->text().toStdString();
  if (typed_name != *m_target_name)
    *m_target_name = typed_name;

  u32 length;
  if (ParseHex(m_length_hex, &length) && length != *m_target_size)
    *m_target_size = length;

  QDialog::accept();
}
