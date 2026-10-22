// Original code for RetroPixels (PixelU-Wii). Not derived from Dolphin.

#include "DolphinQt/Debugger/AssembleInstructionDialog.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "Common/Assembler/GekkoAssembler.h"
#include "Common/StringUtil.h"

namespace
{
namespace GA = Common::GekkoAssembler;

QString Escaped(std::string_view text)
{
  return QString::fromStdString(std::string(text)).toHtmlEscaped();
}

// Splits the offending source line into the part before the error, the error itself and the rest,
// and underlines the middle part in red.
QString MarkErrorSpan(const GA::AssemblerError& error)
{
  const std::string_view src = error.error_line;
  const size_t from = std::min(error.col, src.size());
  const size_t to = std::min(src.size(), from + std::min(error.len, src.size() - from));

  const QString before = Escaped(src.substr(0, from));
  const QString bad = Escaped(src.substr(from, to - from));
  const QString after = Escaped(src.substr(to));

  // Applied one after the other, like three separate substitutions.
  return QStringLiteral("%1<u><span style=\"color:red; font-weight:bold\">%2</span></u>%3")
      .arg(before)
      .arg(bad)
      .arg(after);
}

QString ColouredWord(const char* colour, const QString& word)
{
  return QStringLiteral("<span style=\"color: %1; font-weight: bold\">%2</span>")
      .arg(QString::fromLatin1(colour))
      .arg(word);
}

QFont MonospaceFamily()
{
  return QFont(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
}
}  // namespace

AssembleInstructionDialog::AssembleInstructionDialog(QWidget* parent, u32 address, u32 value)
    : QDialog(parent), m_result_word(value), m_target_address(address)
{
  setWindowModality(Qt::WindowModal);
  setWindowTitle(tr("Instruction"));

  BuildLayout();

  connect(m_dialog_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_dialog_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_source_line, &QLineEdit::textChanged, this, &AssembleInstructionDialog::Reassemble);
}

void AssembleInstructionDialog::BuildLayout()
{
  m_source_line = new QLineEdit;
  m_status_label = new QLabel;
  m_marked_line_label = new QLabel;
  m_detail_label = new QLabel(tr("No input"));
  m_dialog_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

  m_marked_line_label->setFont(MonospaceFamily());
  m_source_line->setFont(MonospaceFamily());

  auto* column = new QVBoxLayout;
  for (QWidget* w : {static_cast<QWidget*>(m_status_label), static_cast<QWidget*>(m_source_line),
                     static_cast<QWidget*>(m_marked_line_label),
                     static_cast<QWidget*>(m_detail_label),
                     static_cast<QWidget*>(m_dialog_buttons)})
  {
    column->addWidget(w);
  }

  // Start from the current word, written as a raw data directive.
  m_source_line->setText(
      QStringLiteral(".4byte 0x%1").arg(m_result_word, 8, 16, QLatin1Char('0')));

  setLayout(column);
  Reassemble();
}

void AssembleInstructionDialog::Reassemble()
{
  std::string source = m_source_line->text().toStdString();
  Common::ToLower(&source);

  const auto outcome = GA::Assemble(source, m_target_address);
  QPushButton* const ok_button = m_dialog_buttons->button(QDialogButtonBox::Ok);

  if (GA::IsFailure(outcome))
  {
    const GA::AssemblerError& error = GA::GetFailure(outcome);
    ok_button->setEnabled(false);
    m_status_label->setText(
        tr("<span style=\"color: red; font-weight: bold\">Error</span> on line %1 col %2")
            .arg(error.line + 1)
            .arg(error.col + 1));
    m_marked_line_label->setText(MarkErrorSpan(error));
    m_detail_label->setText(QString::fromStdString(error.message).toHtmlEscaped());
    return;
  }

  const std::vector<GA::CodeBlock>& blocks = GA::GetT(outcome);
  const bool nothing_emitted = blocks.empty() || blocks.front().instructions.empty();
  ok_button->setEnabled(!nothing_emitted);
  m_marked_line_label->clear();

  if (nothing_emitted)
  {
    m_status_label->setText(ColouredWord("red", tr("Error")));
    m_detail_label->setText(tr("No input"));
    return;
  }

  // Big-endian: the first emitted byte is the most significant one.
  u32 word = 0;
  const std::vector<u8>& bytes = blocks.front().instructions;
  const size_t take = std::min<size_t>(bytes.size(), 4);
  for (size_t n = 0; n < take; ++n)
    word = (word << 8) | bytes[n];
  m_result_word = word;

  m_status_label->setText(ColouredWord("green", tr("OK")));
  m_detail_label->setText(tr("Instruction: %1").arg(m_result_word, 8, 16, QLatin1Char('0')));
}

u32 AssembleInstructionDialog::GetCode() const
{
  return m_result_word;
}
